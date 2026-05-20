// TruMinus reverse tunnel — Plesk side.
//
// One ESP device opens a WSS to /tunnel?token=<shared>. Browsers hit the same
// host on /, /ws, /anything; we hijack the raw TCP socket and ship the bytes
// to the ESP over the control WS as JSON+base64 frames. The ESP relays them
// to its loopback HTTP server (esp_http_server on 127.0.0.1:80). The tunnel
// never parses HTTP for the public traffic — WebSocket upgrades on the
// inner /ws ride for free as opaque bytes.
//
// Wire protocol (text WS frames, JSON):
//   ESP   -> srv  {"type":"hello","node":"<id>","token":"<shared>"}
//   srv   -> ESP  {"type":"open", "id":<n>}
//   both         {"type":"data", "id":<n>, "b":"<base64>"}
//   both         {"type":"close","id":<n>}

import http from "node:http";
import net from "node:net";
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
  esp.send(JSON.stringify({ type: "data", id, b: buf.toString("base64") }));
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
    for (const [, s] of streams) s.socket.destroy();
    streams.clear();
  }
  esp = ws;
  LOG("tunnel: ESP connected");

  ws.on("message", raw => {
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
    if (m.type === "data" && typeof m.b === "string") {
      s.socket.write(Buffer.from(m.b, "base64"));
    } else if (m.type === "close") {
      s.socket.end();
      streams.delete(m.id);
    }
  });

  ws.on("close", () => {
    if (esp === ws) {
      esp = null;
      LOG("tunnel: ESP disconnected");
      for (const [, s] of streams) s.socket.destroy();
      streams.clear();
    }
  });

  ws.on("error", err => LOG("tunnel: ESP ws error:", err.message));
}

// --- WS control server (handles ONLY /tunnel upgrades) -----------------------
// We never bind this http server to a port; we hand-feed it sockets that the
// raw TCP server identifies as the device's control upgrade.
const ctrlServer = http.createServer();
const wss = new WebSocketServer({ noServer: true });

ctrlServer.on("upgrade", (req, socket, head) => {
  let url;
  try { url = new URL(req.url, "http://x"); } catch { socket.destroy(); return; }
  if (url.pathname !== "/tunnel") { socket.destroy(); return; }
  if (url.searchParams.get("token") !== TOKEN) {
    socket.end("HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n");
    return;
  }
  wss.handleUpgrade(req, socket, head, ws => handleEsp(ws));
});

// --- Public raw-TCP entrypoint ----------------------------------------------
// Plesk Passenger forwards plain HTTP on PORT to us. We peek at the first
// chunk: if it's the device's WS upgrade for /tunnel, we hand it to the ws
// stack; otherwise we tunnel raw bytes to the ESP.
const HEAD_PEEK_BYTES = 1024;

function looksLikeTunnelUpgrade(buf) {
  // Cheap ASCII probe — we only need the request line + a couple of headers.
  const head = buf.toString("ascii", 0, Math.min(buf.length, HEAD_PEEK_BYTES));
  const m = head.match(/^GET (\/tunnel(?:\?[^\s]*)?) HTTP\/1\.[01]\r\n/);
  if (!m) return false;
  // Only treat it as ESP if it also carries an Upgrade: websocket header.
  return /\r\nupgrade:\s*websocket\r\n/i.test(head + "\r\n");
}

const publicServer = net.createServer(socket => {
  socket.once("data", chunk => {
    socket.pause();
    if (looksLikeTunnelUpgrade(chunk)) {
      // Re-emit as a fresh connection on the WS-only http server, with the
      // already-read bytes pushed back so its parser sees the whole request.
      socket.unshift(chunk);
      ctrlServer.emit("connection", socket);
      socket.resume();
      return;
    }
    // Browser / API client → forward to ESP as opaque bytes.
    attachStream(socket, chunk);
    socket.resume();
  });
  socket.on("error", () => socket.destroy());
});

publicServer.listen(PORT, () => {
  LOG(`tunnel: listening on :${PORT}`);
});

// --- graceful shutdown -------------------------------------------------------
function shutdown(sig) {
  LOG(`tunnel: ${sig} received, closing`);
  try { esp?.close(1001, "shutdown"); } catch {}
  for (const [, s] of streams) s.socket.destroy();
  publicServer.close(() => process.exit(0));
  setTimeout(() => process.exit(1), 3000).unref();
}
process.on("SIGINT",  () => shutdown("SIGINT"));
process.on("SIGTERM", () => shutdown("SIGTERM"));
