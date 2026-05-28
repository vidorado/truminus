// TruMinus reverse tunnel — Plesk side.
//
// One ESP device opens a WSS to /tunnel?token=<shared>. Browsers hit the same
// host on /, /ws, /anything; we hijack the raw TCP socket and ship the bytes
// to the ESP over the control WS as JSON+base64 frames. The ESP relays them
// to its loopback HTTP server (esp_http_server on 127.0.0.1:80). The tunnel
// never parses HTTP for the public traffic — WebSocket upgrades on the
// inner /ws ride for free as opaque bytes.
//
// Wire protocol:
//
//   Control frames are *text* (JSON):
//     ESP -> srv  {"type":"hello","node":"<id>","token":"<shared>"}
//     srv -> ESP  {"type":"open", "id":<n>}
//     both        {"type":"close","id":<n>}
//
//   Data frames are *binary* — first 4 bytes are the stream id (big-
//   endian), followed by the raw stream bytes.  Skipping base64+JSON for
//   the bulk traffic saves ~33% bandwidth and a parse/format round-trip
//   on both sides.

import http from "node:http";
import https from "node:https";
import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { WebSocketServer } from "ws";

const PORT = Number(process.env.PORT) || 3000;
const TOKEN = process.env.TUNNEL_TOKEN || "";

// LOG_FILE lets the operator pin the application log to a known path that
// survives Passenger restarts and isn't buried under Plesk's log layout.
// Defaults to ./logs/server.log next to the app so operators don't have to
// hunt for Plesk's Passenger log; override with LOG_FILE for an explicit
// path.  Falls back to stdout-only if the chosen directory isn't writable
// (same fallback shape as CACHE_DIR).  We still write to stdout in any
// case so Passenger's own log captures everything; the file is a
// parallel append-only mirror.
const LOG_FILE = process.env.LOG_FILE || path.join(process.cwd(), "logs", "server.log");
let logStream = null;
try {
  fs.mkdirSync(path.dirname(LOG_FILE), { recursive: true });
  logStream = fs.createWriteStream(LOG_FILE, { flags: "a" });
} catch (err) {
  console.log(new Date().toISOString(), `log file disabled: cannot use ${LOG_FILE}: ${err.message}`);
}

// Verbosity control.  Default WARN — only abnormal events.  Bump to INFO
// for boot/state changes (ESP connect/disconnect, tunnel listening,
// keepalive enabled) and to DEBUG for every per-request forward/done
// and cache hit/miss/store line — those flood the log under normal
// browsing and are only useful when actively diagnosing.
const LOG_LEVELS = { ERROR: 0, WARN: 1, INFO: 2, DEBUG: 3 };
const LOG_LEVEL  = LOG_LEVELS[(process.env.LOG_LEVEL || "").toUpperCase()] ?? LOG_LEVELS.WARN;
function emit(level, label, args) {
  if (LOG_LEVELS[label] > LOG_LEVEL) return;
  const line = `${new Date().toISOString()} [${label}] ${args.join(" ")}`;
  console.log(line);
  if (logStream) logStream.write(line + "\n");
}
const LOG = (...a) => emit("WARN", "WARN", a);   // bare call defaults to WARN
LOG.error = (...a) => emit("ERROR", "ERROR", a);
LOG.warn  = (...a) => emit("WARN",  "WARN",  a);
LOG.info  = (...a) => emit("INFO",  "INFO",  a);
LOG.debug = (...a) => emit("DEBUG", "DEBUG", a);
if (logStream) LOG.info(`log file at ${LOG_FILE}`);
LOG.info(`log level: ${Object.keys(LOG_LEVELS)[LOG_LEVEL]}`);

// Dedicated log file for blocked vulnerability-scanner hits.  fail2ban tails
// this file; one line per attempt, with a stable "BLOCKED <ip> <method> <path>"
// shape so the filter regex stays trivial.  Defaults to a path under Plesk's
// per-domain logs dir; override with BLOCK_LOG env if you run elsewhere.
const BLOCK_LOG = process.env.BLOCK_LOG || "";
function logBlocked(ip, line) {
  const msg = `${new Date().toISOString()} BLOCKED ${ip} ${line}\n`;
  process.stderr.write(msg);
  if (BLOCK_LOG) fs.appendFile(BLOCK_LOG, msg, () => {});
}

// Known scanner targets.  Most are config / secret files for popular stacks
// (PHP, WordPress, Node, Python, Rails, .NET) plus generic dotfiles and SCM
// metadata.  Anchored to the start of the request line, so legitimate paths
// that happen to contain "env" elsewhere are unaffected.
const BLOCKED_PATH_RE = new RegExp(
  "^(?:GET|POST|HEAD|PUT|OPTIONS|DELETE|PATCH) " +
  "(\\/(?:" +
    "\\.env|\\.git|\\.svn|\\.hg|\\.htaccess|\\.htpasswd|\\.aws|\\.ssh|" +
    "\\.bash|\\.bashrc|\\.npmrc|\\.yarnrc|\\.pypirc|\\.docker|\\.vscode|" +
    "\\.idea|\\.firebase|\\.gitlab-ci|\\.git-credentials|" +
    "wp-|wp_|wordpress|phpmyadmin|pma|adminer|admin\\.php|xmlrpc\\.php|" +
    "composer\\.(?:json|lock)|package(?:-lock)?\\.json|yarn\\.lock|" +
    "appsettings|local\\.settings|" +
    "application(?:-[\\w-]+)?\\.(?:yml|yaml|properties|json)|" +
    "database\\.yml|secrets\\.yml|" +
    "config(?:[\\w\\/.-]*)\\.(?:php|yml|yaml|json|ini|xml)|" +
    "local_settings\\.py|settings(?:[\\w\\/.-]*)\\.py|" +
    "next\\.config|nuxt\\.config|vite\\.config|" +
    "firebase\\.json|amplify\\.yml|web\\.config|" +
    "Dockerfile|docker-compose|Jenkinsfile|" +
    "cgi-bin|cgi/|" +
    "backup\\.sql|dump\\.sql|database\\.sql|db\\.sql|" +
    "storage\\/logs|logs?\\/[\\w.-]+\\.log|var\\/log\\/|" +
    // Spring Boot / Actuator probes
    "actuator(?:\\/|$)|" +
    // Framework-tree probes for stacks we don't ship
    "_next\\/|_nuxt\\/|static\\/js\\/main|" +
    // Quote-injection probes: any path starting with %22 (URL-encoded ")
    // is a scanner trying to fingerprint a vulnerable parser.
    "%22|%27" +
  ")[\\S]*?) HTTP\\/", "i"
);

function parseClientIp(head, fallback) {
  const m = /\r\nX-Forwarded-For:\s*([^,\r\n]+)/i.exec(head);
  return m ? m[1].trim() : (fallback || "unknown");
}

function extractRequestLine(head) {
  const m = /^([A-Z]+ \S+) HTTP\//.exec(head);
  return m ? m[1] : "?";
}

if (!TOKEN) {
  console.error("FATAL: TUNNEL_TOKEN env var is required");
  process.exit(1);
}

// --- ESP control channel -----------------------------------------------------
let esp = null;                                  // the one connected device
const streams = new Map();                       // id -> { socket }
let nextId = 1;

// Cap concurrent in-flight tunneled streams toward the ESP.  The ESP's
// pump task services streams sequentially and a congested TLS send can
// stall all of them; capping below the firmware's MAX_STREAMS=12 leaves
// headroom for the long-lived /ws session and prevents the cascade
// where one congested send blocks every other loopback socket.  When
// the cap is reached, new browser connections sit in a FIFO with their
// TCP paused; the next active close drains the queue.
const MAX_CONCURRENT       = 3;
// 90 s is long enough for the first cold load to drain even when the ESP
// only manages ~5 s per stream over the WSS tunnel (≈15 assets × 5 s ÷ 3
// cap = 25 s).  With the old 10 s window the queue tail timed out before
// the ESP could process it, so the reverse cache never got populated and
// every reload looked equally broken.  Browsers' own connection timeout
// (~60–90 s) terminates abandoned items if the user navigates away.
const PENDING_TIMEOUT_MS   = 90000;
const PENDING_QUEUE_LIMIT  = 64;
const pendingQueue = [];   // { socket, firstChunk, timer, onClose }

// --- Reverse asset cache -----------------------------------------------------
// Static assets are served with cache-busting ?v=<sha1> querystrings, so the
// URL itself is a content key — when bytes change, the URL changes.  We cache
// the raw HTTP response (status line + headers + chunked body, exactly as the
// ESP emitted it) keyed by sha256(METHOD + URL) and replay it byte-for-byte
// on subsequent requests, bypassing the tunnel entirely.  Survives across
// browser cache wipes, incognito, and most importantly Ctrl+F5 — the
// scenario where every parallel asset request hits the ESP cold.
const CACHE_DIR        = process.env.CACHE_DIR || path.join(process.cwd(), "cache");
const CACHE_MAX_BYTES  = 2 * 1024 * 1024;      // per-response ceiling
const CACHE_KEY_RE     = /\?v=[a-f0-9]+/i;     // require a cache-bust hash
const CHUNK_TERMINATOR = Buffer.from("0\r\n\r\n");

// Surface mkdir/permission problems loudly: a silent failure here means
// every request is a cache miss for the lifetime of the process, with no
// log line on each miss to give the operator a clue.  Print the resolved
// path on success too so deployments can sanity-check the location.
let cacheEnabled = false;
try {
  fs.mkdirSync(CACHE_DIR, { recursive: true });
  fs.accessSync(CACHE_DIR, fs.constants.W_OK);
  cacheEnabled = true;
  LOG.info(`cache enabled at ${CACHE_DIR}`);
} catch (err) {
  LOG.warn(`cache DISABLED: cannot use ${CACHE_DIR}: ${err.message}`);
}

function cacheKey(method, url) {
  return crypto.createHash("sha256").update(`${method} ${url}`).digest("hex");
}
function cachePath(key) { return path.join(CACHE_DIR, `${key}.bin`); }

function parseRequestLine(chunk) {
  const head = chunk.toString("ascii", 0, Math.min(chunk.length, HEAD_PEEK_BYTES));
  const m = /^([A-Z]+) (\S+) HTTP\/1\.[01]\r\n/.exec(head);
  return m ? { method: m[1], url: m[2] } : null;
}

function isCacheable(req) {
  return cacheEnabled && req && req.method === "GET" && CACHE_KEY_RE.test(req.url);
}

function tryServeFromCache(socket, req) {
  if (!cacheEnabled || !isCacheable(req)) return false;
  let buf;
  try { buf = fs.readFileSync(cachePath(cacheKey(req.method, req.url))); }
  catch { return false; }
  socket.end(buf);
  LOG.debug(`cache HIT ${req.url} (${buf.length}B)`);
  return true;
}

// Validate a buffered response before persisting.  Must be HTTP 200, chunked
// transfer (the ESP's exclusive framing for served files), and end with the
// proper terminator chunk.  Anything else means partial/error response that
// would poison the cache.
function isCompleteOk200(buf) {
  const hdrEnd = buf.indexOf("\r\n\r\n");
  if (hdrEnd < 0) return false;
  const headersStr = buf.slice(0, hdrEnd).toString("ascii");
  if (!/^HTTP\/1\.[01] 200 /.test(headersStr)) return false;
  if (!/\r\nTransfer-Encoding:\s*chunked/i.test(headersStr)) return false;
  return buf.subarray(buf.length - CHUNK_TERMINATOR.length).equals(CHUNK_TERMINATOR);
}

function cacheCommit(req, buf) {
  try {
    if (!isCompleteOk200(buf)) {
      LOG.debug(`cache SKIP ${req.url} (${buf.length}B, not a complete 200 chunked)`);
      return;
    }
    const p = cachePath(cacheKey(req.method, req.url));
    fs.writeFile(p, buf, err => {
      if (err) LOG.error(`cache write failed ${req.url}: ${err.message}`);
      else     LOG.debug(`cache MISS→STORE ${req.url} (${buf.length}B)`);
    });
  } catch (err) {
    LOG.error(`cache commit error: ${err.message}`);
  }
}

function espAlive() { return esp && esp.readyState === 1; }
function sendCtrl(msg) { if (espAlive()) esp.send(JSON.stringify(msg)); }
function sendData(id, buf) {
  if (!espAlive()) return;
  // Binary frame: 4-byte BE id || payload.  Allocate once and copy.
  const out = Buffer.allocUnsafe(4 + buf.length);
  out.writeUInt32BE(id >>> 0, 0);
  buf.copy(out, 4);
  esp.send(out, { binary: true });
}

function drainPendingQueue() {
  while (streams.size < MAX_CONCURRENT && pendingQueue.length > 0) {
    const item = pendingQueue.shift();
    clearTimeout(item.timer);
    item.socket.removeListener("close", item.onClose);
    if (item.socket.destroyed || item.socket.writableEnded) continue;
    attachStreamNow(item.socket, item.firstChunk, item.req);
  }
}

function clearPendingQueue(why) {
  while (pendingQueue.length > 0) {
    const item = pendingQueue.shift();
    clearTimeout(item.timer);
    item.socket.removeListener("close", item.onClose);
    if (!item.socket.destroyed && !item.socket.writableEnded) {
      item.socket.end(`HTTP/1.1 502 Bad Gateway\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n${why}\n`);
    }
  }
}

// Cleanly close every in-flight tunneled stream. If a stream hasn't sent
// any bytes back to the client yet we synthesise a 502 so Passenger /
// nginx see a well-formed response instead of a half-open socket (which
// surfaces in the browser as "Incomplete response received from
// application"). `socket.end()` flushes the FIN so the TCP peer sees a
// graceful close rather than the RST that `destroy()` would send.
function teardownStreams(why) {
  for (const [, s] of streams) {
    if (s.socket.writableEnded || s.socket.destroyed) continue;
    if (s.socket.bytesWritten === 0) {
      s.socket.end(`HTTP/1.1 502 Bad Gateway\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n${why}\n`);
    } else {
      s.socket.end();
    }
  }
  streams.clear();
  clearPendingQueue(why);
}

function attachStream(socket, firstChunk) {
  // Cache hit: serve from disk, never wake the ESP.  Works even when the
  // tunnel is offline — the static asset survives outages by definition.
  const req = parseRequestLine(firstChunk);
  if (tryServeFromCache(socket, req)) return;

  if (!espAlive()) {
    socket.end("HTTP/1.1 502 Bad Gateway\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\ntunnel offline\n");
    return;
  }
  if (streams.size >= MAX_CONCURRENT) {
    if (pendingQueue.length >= PENDING_QUEUE_LIMIT) {
      socket.end("HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\nRetry-After: 1\r\nConnection: close\r\n\r\ntunnel queue full\n");
      return;
    }
    socket.pause();
    const item = { socket, firstChunk, req, timer: null, onClose: null };
    item.timer = setTimeout(() => {
      const i = pendingQueue.indexOf(item);
      if (i < 0) return;
      pendingQueue.splice(i, 1);
      socket.removeListener("close", item.onClose);
      if (!socket.destroyed && !socket.writableEnded) {
        socket.end("HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\nRetry-After: 1\r\nConnection: close\r\n\r\ntunnel busy\n");
      }
    }, PENDING_TIMEOUT_MS);
    item.onClose = () => {
      const i = pendingQueue.indexOf(item);
      if (i < 0) return;
      pendingQueue.splice(i, 1);
      clearTimeout(item.timer);
    };
    socket.once("close", item.onClose);
    pendingQueue.push(item);
    return;
  }
  attachStreamNow(socket, firstChunk, req);
}

function attachStreamNow(socket, firstChunk, req) {
  const id = nextId++;
  const cacheable = isCacheable(req);
  // WebSocket sessions to /ws stay open for the lifetime of the browser
  // session.  They must be exempted from both the chunked-terminator
  // proactive close and the idle reaper, otherwise the live LCD data
  // pipe gets killed every 30 s.
  const isWs = !!(req && /^\/ws(?:\?|\/|$)/.test(req.url));
  const stream = {
    socket,
    req,
    isWs,
    buf: cacheable ? [] : null,
    bufLen: 0,
    // Rolling tail of the last bytes we forwarded to the browser, used to
    // detect the chunked terminator (0\r\n\r\n) that marks end-of-response.
    // Without this, keep-alive TCPs from nginx/browser never close on their
    // own and our stream slot stays occupied forever, starving the queue.
    tail: Buffer.alloc(0),
    done: false,
    startedAt: Date.now(),
  };
  streams.set(id, stream);
  if (req) LOG.debug(`forward → ESP id=${id} ${req.method} ${req.url}${cacheable ? " [cacheable]" : ""}${isWs ? " [ws]" : ""} (slots=${streams.size}/${MAX_CONCURRENT}, queue=${pendingQueue.length})`);
  sendCtrl({ type: "open", id });
  if (firstChunk && firstChunk.length) sendData(id, firstChunk);

  socket.on("data", buf => sendData(id, buf));
  socket.on("close", () => {
    if (streams.delete(id)) {
      sendCtrl({ type: "close", id });
      if (stream.buf !== null && stream.bufLen > 0) {
        cacheCommit(stream.req, Buffer.concat(stream.buf, stream.bufLen));
        stream.buf = null;
      }
      LOG.debug(`browser-close id=${id} (slots=${streams.size}/${MAX_CONCURRENT}, queue=${pendingQueue.length})`);
      drainPendingQueue();
    }
  });
  socket.on("error", () => socket.destroy());
  socket.resume();
}

// Safety net: if a stream survives 30 s without traffic (the chunked
// terminator never arrived, the browser never closed, etc.) reap it so
// the slot doesn't leak.  Catches buggy edge cases — the proactive
// terminator detection handles the common path.
const STREAM_IDLE_MAX_MS = 30000;
setInterval(() => {
  if (streams.size === 0) return;
  const now = Date.now();
  for (const [id, s] of streams) {
    if (s.isWs) continue;                            // /ws stays open forever
    if (!s.startedAt) s.startedAt = now;            // backfill for first sweep
    if (now - s.startedAt < STREAM_IDLE_MAX_MS) continue;
    LOG.warn(`reaping idle stream id=${id} (age=${now - s.startedAt}ms)`);
    try { s.socket.end(); } catch {}
    sendCtrl({ type: "close", id });
    streams.delete(id);
  }
  if (pendingQueue.length > 0) drainPendingQueue();
}, 5000).unref();

// Passenger idle-kills workers whose only traffic is raw TCP after a WS
// upgrade — it doesn't count the post-upgrade byte stream as "activity".
// Default passenger_pool_idle_time is 300 s, after which our worker dies
// and the ESP has to reconnect (with all in-flight queued items 502'd).
// passenger_pool_idle_time has to live in nginx's http context which
// Plesk doesn't expose per-domain, so we feed Passenger a real HTTP
// request every 60 s.  We latch the public origin from the first inbound
// request's Host header so no env var is needed; KEEPALIVE_URL can still
// override (useful for testing or if the latch never warms up).
const KEEPALIVE_INTERVAL_MS = 60000;
let keepaliveOrigin = process.env.KEEPALIVE_URL || "";   // e.g. https://truminus.victordorado.es
let keepaliveTimer = null;

function startKeepalive() {
  if (keepaliveTimer || !keepaliveOrigin) return;
  let urlObj;
  try { urlObj = new URL(keepaliveOrigin.replace(/\/+$/, "") + "/__keepalive__"); }
  catch (err) { LOG.error(`keepalive: bad origin ${keepaliveOrigin}: ${err.message}`); return; }
  const lib = urlObj.protocol === "https:" ? https : http;
  keepaliveTimer = setInterval(() => {
    const req = lib.get(urlObj, res => { res.resume(); });
    req.setTimeout(15000, () => req.destroy());
    req.on("error", err => LOG.warn(`keepalive ping error: ${err.message}`));
  }, KEEPALIVE_INTERVAL_MS);
  keepaliveTimer.unref();
  LOG.info(`keepalive enabled: ${urlObj.href} every ${KEEPALIVE_INTERVAL_MS/1000}s`);
}

// Sniff the Host header out of a request's head buffer to latch the public
// origin.  X-Forwarded-Proto from Plesk's front nginx tells us whether the
// browser hit https or http.  Falls back to https since that's the only
// realistic deployment (Plesk Let's Encrypt is default-on).
function latchKeepaliveFromHead(head) {
  if (keepaliveOrigin) return;
  const hostM  = /\r\nHost:\s*([^\r\n]+)/i.exec(head);
  if (!hostM) return;
  const protoM = /\r\nX-Forwarded-Proto:\s*([^\r\n]+)/i.exec(head);
  const scheme = (protoM && /^https?$/i.test(protoM[1].trim())) ? protoM[1].trim().toLowerCase() : "https";
  keepaliveOrigin = `${scheme}://${hostM[1].trim()}`;
  startKeepalive();
}

if (keepaliveOrigin) startKeepalive();

function handleEsp(ws) {
  // Reject if another ESP was already connected.
  if (esp && esp.readyState === 1) {
    LOG.warn("tunnel: replacing previous ESP connection");
    try { esp.close(4000, "superseded"); } catch {}
    // Tear down all in-flight streams since their other end is gone.
    teardownStreams("ESP replaced");
  }
  esp = ws;
  LOG.info("tunnel: ESP connected");

  ws.on("message", (raw, isBinary) => {
    if (isBinary) {
      // Binary data frame: 4-byte BE id || payload bytes.
      if (raw.length < 4) return;
      const id = raw.readUInt32BE(0);
      const s = streams.get(id);
      if (!s || s.done) return;
      const payload = raw.subarray(4);
      s.socket.write(payload);
      // WS streams: just forward, no terminator detection, no caching.
      if (s.isWs) return;
      try {
        if (s.buf !== null) {
          if (s.bufLen + payload.length > CACHE_MAX_BYTES) {
            s.buf = null;
          } else {
            s.buf.push(Buffer.from(payload));
            s.bufLen += payload.length;
          }
        }
        // Rolling 5-byte tail to spot the chunked-terminator across frame
        // boundaries.  If we see "0\r\n\r\n" the response is complete; close
        // the stream ourselves so its slot frees immediately instead of
        // waiting for nginx/browser keep-alive to eventually time out.
        const merged = s.tail.length
          ? Buffer.concat([s.tail, payload], s.tail.length + payload.length)
          : payload;
        s.tail = merged.length > CHUNK_TERMINATOR.length
          ? Buffer.from(merged.subarray(merged.length - CHUNK_TERMINATOR.length))
          : Buffer.from(merged);
        if (s.tail.length === CHUNK_TERMINATOR.length && s.tail.equals(CHUNK_TERMINATOR)) {
          s.done = true;
          if (s.buf !== null && s.bufLen > 0) {
            cacheCommit(s.req, Buffer.concat(s.buf, s.bufLen));
            s.buf = null;
          }
          // Tell the ESP we're done so it can free its loopback slot, then
          // end the browser TCP gracefully (FIN).  Free our slot and pull
          // the next queued item.
          sendCtrl({ type: "close", id });
          try { s.socket.end(); } catch {}
          streams.delete(id);
          LOG.debug(`done   ← ESP id=${id}${s.req ? " " + s.req.url : ""} (slots=${streams.size}/${MAX_CONCURRENT}, queue=${pendingQueue.length})`);
          drainPendingQueue();
        }
      } catch (err) {
        LOG.error(`stream ${id} capture error: ${err.message}`);
        s.buf = null;
      }
      return;
    }
    // Text frame: JSON control message.
    let m;
    try { m = JSON.parse(raw.toString()); } catch { return; }
    if (!m || typeof m.id !== "number") {
      if (m && m.type === "hello") {
        LOG.info(`tunnel: hello node=${m.node}`);
      }
      return;
    }
    const s = streams.get(m.id);
    if (!s) return;
    if (m.type === "close") {
      // Same shape as teardownStreams: if the ESP closed before writing a
      // single byte, send a 502 ourselves so Passenger doesn't surface the
      // empty stream as "Incomplete response received from application".
      if (s.socket.bytesWritten === 0) {
        s.socket.end("HTTP/1.1 502 Bad Gateway\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nupstream closed\n");
      } else {
        s.socket.end();
      }
      // Persist a complete-looking response before we drop the buffer.
      if (s.buf !== null && s.bufLen > 0) {
        cacheCommit(s.req, Buffer.concat(s.buf, s.bufLen));
        s.buf = null;
      }
      streams.delete(m.id);
      drainPendingQueue();
    }
  });

  ws.on("close", () => {
    if (esp === ws) {
      esp = null;
      LOG.warn("tunnel: ESP disconnected");
      teardownStreams("ESP disconnected");
    }
  });

  ws.on("error", err => LOG.warn("tunnel: ESP ws error:", err.message));
}

// --- Public HTTP server -------------------------------------------------------
// Plesk's Phusion Passenger hooks `http.Server.prototype.listen` to detect
// when the Node app is ready, so we *must* expose the bound server as an
// http.Server (not net.Server) — otherwise Passenger times out spawning us
// with "A timeout occurred while spawning an application process".
//
// We still want raw-TCP muxing for everything that isn't the ESP's control
// upgrade. To achieve both: replace Node's default connection handler on the
// http server with our own sniffer. Connections whose first bytes look like
// `GET /tunnel … Upgrade: websocket` are routed back into Node's HTTP parser
// (so the wss.handleUpgrade path below fires); every other byte stream is
// tunneled opaquely to the ESP.
const HEAD_PEEK_BYTES = 1024;

function looksLikeTunnelUpgrade(buf) {
  const head = buf.toString("ascii", 0, Math.min(buf.length, HEAD_PEEK_BYTES));
  if (!/^GET (\/tunnel(?:\?[^\s]*)?) HTTP\/1\.[01]\r\n/.test(head)) return false;
  return /\r\nupgrade:\s*websocket\r\n/i.test(head + "\r\n");
}

const server = http.createServer();
const wss = new WebSocketServer({ noServer: true });

server.on("upgrade", (req, socket, head) => {
  let url;
  try { url = new URL(req.url, "http://x"); } catch { socket.destroy(); return; }
  if (url.pathname !== "/tunnel") { socket.destroy(); return; }
  if (url.searchParams.get("token") !== TOKEN) {
    socket.end("HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n");
    return;
  }
  wss.handleUpgrade(req, socket, head, ws => handleEsp(ws));
});

// Swap out the default 'connection' listener (Node's HTTP parser dispatcher)
// for our sniffer, so non-/tunnel traffic never enters the HTTP parser.
const defaultConnectionListener = server.listeners("connection")[0];
server.removeAllListeners("connection");
server.on("connection", socket => {
  socket.once("data", chunk => {
    socket.pause();
    if (looksLikeTunnelUpgrade(chunk)) {
      socket.unshift(chunk);
      defaultConnectionListener.call(server, socket);
      socket.resume();
      return;
    }
    // Short-circuit known vulnerability-scanner paths before they reach the
    // ESP.  Logging the (IP, request-line) pair lets fail2ban ban repeaters.
    const head = chunk.toString("ascii", 0, Math.min(chunk.length, HEAD_PEEK_BYTES));
    latchKeepaliveFromHead(head);
    if (BLOCKED_PATH_RE.test(head)) {
      const ip = parseClientIp(head, socket.remoteAddress);
      logBlocked(ip, extractRequestLine(head));
      socket.end("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
      return;
    }
    // Self-ping endpoint used by our own setInterval to feed Passenger
    // legitimate HTTP traffic and prevent idle-kill.  Answer directly,
    // never forward to the ESP.
    if (/^GET \/__keepalive__ HTTP\/1\.[01]\r\n/.test(head)) {
      socket.end("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
      return;
    }
    attachStream(socket, chunk);
    socket.resume();
  });
  socket.on("error", () => socket.destroy());
});

server.listen(PORT, () => {
  LOG.info(`tunnel: listening on :${PORT}`);
});

// --- graceful shutdown -------------------------------------------------------
function shutdown(sig) {
  LOG.info(`tunnel: ${sig} received, closing`);
  try { esp?.close(1001, "shutdown"); } catch {}
  for (const [, s] of streams) s.socket.destroy();
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(1), 3000).unref();
}
process.on("SIGINT",  () => shutdown("SIGINT"));
process.on("SIGTERM", () => shutdown("SIGTERM"));
