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
import { WebSocketServer } from "ws";

const PORT = Number(process.env.PORT) || 3000;
const TOKEN = process.env.TUNNEL_TOKEN || "";
const LOG = (...a) => console.log(new Date().toISOString(), ...a);

if (!TOKEN) {
  console.error("FATAL: TUNNEL_TOKEN env var is required");
  process.exit(1);
}

// --- ESP control channel -----------------------------------------------------
let esp = null;                                  // the one connected device
const streams = new Map();                       // id -> { socket }
let nextId = 1;

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
}

function attachStream(socket, firstChunk) {
  if (!espAlive()) {
    socket.end("HTTP/1.1 502 Bad Gateway\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\ntunnel offline\n");
    return;
  }
  const id = nextId++;
  streams.set(id, { socket });
  sendCtrl({ type: "open", id });
  if (firstChunk && firstChunk.length) sendData(id, firstChunk);

  socket.on("data", buf => sendData(id, buf));
  socket.on("close", () => {
    if (streams.delete(id)) sendCtrl({ type: "close", id });
  });
  socket.on("error", () => socket.destroy());
}

function handleEsp(ws) {
  // Reject if another ESP was already connected.
  if (esp && esp.readyState === 1) {
    LOG("tunnel: replacing previous ESP connection");
    try { esp.close(4000, "superseded"); } catch {}
    // Tear down all in-flight streams since their other end is gone.
    teardownStreams("ESP replaced");
  }
  esp = ws;
  LOG("tunnel: ESP connected");

  ws.on("message", (raw, isBinary) => {
    if (isBinary) {
      // Binary data frame: 4-byte BE id || payload bytes.
      if (raw.length < 4) return;
      const id = raw.readUInt32BE(0);
      const s = streams.get(id);
      if (!s) return;
      s.socket.write(raw.subarray(4));
      return;
    }
    // Text frame: JSON control message.
    let m;
    try { m = JSON.parse(raw.toString()); } catch { return; }
    if (!m || typeof m.id !== "number") {
      if (m && m.type === "hello") {
        LOG(`tunnel: hello node=${m.node}`);
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
      streams.delete(m.id);
    }
  });

  ws.on("close", () => {
    if (esp === ws) {
      esp = null;
      LOG("tunnel: ESP disconnected");
      teardownStreams("ESP disconnected");
    }
  });

  ws.on("error", err => LOG("tunnel: ESP ws error:", err.message));
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
    attachStream(socket, chunk);
    socket.resume();
  });
  socket.on("error", () => socket.destroy());
});

server.listen(PORT, () => {
  LOG(`tunnel: listening on :${PORT}`);
});

// --- graceful shutdown -------------------------------------------------------
function shutdown(sig) {
  LOG(`tunnel: ${sig} received, closing`);
  try { esp?.close(1001, "shutdown"); } catch {}
  for (const [, s] of streams) s.socket.destroy();
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(1), 3000).unref();
}
process.on("SIGINT",  () => shutdown("SIGINT"));
process.on("SIGTERM", () => shutdown("SIGTERM"));
