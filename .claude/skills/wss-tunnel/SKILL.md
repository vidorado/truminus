# Skill: WSS reverse tunnel — operational design + gotchas

The end-to-end story behind `main/wstunnel.cpp` + `main/webserver.cpp` +
`server/app.js`.  Use this when debugging tunnel-specific behaviour
(latency that doesn't reproduce on LAN, dying WebSocket, stuck
"pending timeout, closing" loops, mbedtls/PSA OOM).  Per-symbol
comments in the source explain individual decisions; this file links
them into one story and lists the landmines we already stepped on.

---

## Lifecycle of a single browser request

1. Browser opens `wss://<domain>/<path>`.  Plesk/nginx terminates TLS
   and proxies the raw TCP to the Node bridge (`server/app.js`).
2. Bridge sniffs the first chunk: if it's `GET /tunnel … Upgrade:
   websocket` the bridge handles the device control channel itself;
   anything else becomes a tunneled stream.  Each browser TCP gets a
   monotonic `id`; the bridge sends `{"type":"open","id":N}` to the
   ESP, then forwards the first chunk as a binary frame
   `<4-byte BE id><bytes…>`.
3. ESP (`wstunnel.cpp`) buffers the open in `s_pending` until at least
   8 bytes of request data arrive, then `pick_local_port()` sniffs
   the HTTP method+path:
   - `GET /ws…` → loopback `127.0.0.1:81` (WS-dedicated httpd)
   - everything else → loopback `127.0.0.1:80` (static-asset httpd)
4. `connect_local(port)` opens the loopback socket with
   `TCP_NODELAY=1` (see "Nagle on loopback" below), flushes the
   buffered request, and assigns the new `Stream` slot with
   `is_ws = (port == LOCAL_WS_PORT)`.
5. `pump_task` drains the loopback fd in 4 KB coalesced reads and
   ships each batch to the bridge as a binary WS frame
   (`<4-byte BE id><payload>`).  Bridge unwraps and writes to the
   matching browser TCP.

For ESP → browser (a `setting` update emitted by `main.cpp`):
`wsQueueSend` → `wsQueueDrain` (in `wsPumpTask`, 100 ms cadence) →
`httpd_ws_send_frame_async(wsServer:81, fd, …)` → kernel writes the
WS frame to the loopback → `pump_task` reads it → `send_data` over
the WSS control channel → bridge → browser.

---

## Why two httpd instances (`:80` and `:81`)

A single httpd with `lru_purge_enable=true` is the obvious
configuration, **but** `:80` carries an avalanche of parallel asset
GETs during a page load.  The connected `/ws` becomes the least-
recently-active socket and gets evicted mid-flight, which the browser
surfaces as *"WebSocket is closed before the connection is
established"*.  `ReconnectingWebSocket` then reopens, eating another
slot, and the system reconnect-storms.

The fix is QoS by *socket pool* rather than priority hints:

| httpd | port | max_open_sockets | lru_purge | clients |
|-------|------|------------------|-----------|---------|
| `httpServer` | 80 | 12 | true  | assets + LAN-direct WS |
| `wsServer`   | 81 | 4  | false | tunneled WS only       |

`pick_local_port()` routes `GET /ws` to `:81`; an asset flood on `:80`
can never touch the WS pool.  `:80` keeps a `/ws` handler too so LAN-
direct browsers (no tunnel) work without changes.

`wsCountFromHttpd` and `wsQueueDrain` iterate both servers; broadcasts
reach LAN and tunneled clients alike.

`ctrl_port` (default 32768) **must differ between httpd instances** —
they conflict silently otherwise and you lose the work-queue signal
that wakes `httpd_ws_send_frame_async`.  We use 32768 and 32769.

---

## Idle eviction of zombie keep-alive HTTPs

A browser holds HTTP/1.1 keep-alive connections open for ~5 minutes.
Through the tunnel each one occupies a `Stream` slot indefinitely.
After a few page reloads / tab switches, the 12-slot table fills with
idle zombies and every new `open` from the bridge dies via the
5 s `PENDING_TIMEOUT_US`.

`evict_idle_locked()` solves this: when `try_open_locked` finds no
free slot, it reclaims the least-recently-active **non-WS** stream
that has been idle for > 5 s, sends `close` to the bridge, and retries
the open.  `Stream::is_ws` ensures the live WebSocket is exempt.

`Stream::last_active_us` is updated in two places: `pump_task` after a
successful read, and `handle_data` after queueing bytes for the local
socket.

---

## Keepalive — WS control PING, not text

Plesk/nginx has `proxy_read_timeout=60 s` by default.  With no
ESP → browser traffic for 60 s (heating idle, BMS quiet) the proxy
kills the WS.  The wsReaper task in `webserver.cpp` sends a real
WS PING frame (opcode 0x9, zero payload) every 20 s to every WS fd
across both servers.  Browsers PONG automatically — invisible to JS,
no message handler needed.

We deleted the older approach (`ws.send('ping')` from JS every 10 s
+ no-op handler in `wsHandler`) because it only covered browser →
ESP and added churn to the message channel for no benefit.

---

## Memory: where every byte lives

The default IDF behaviour (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`)
serves anything < 16 KB from internal DRAM — that includes every
mbedtls / PSA allocation (cert chains, key contexts, etc.).  On the
first WSS handshake those compete with `esp-aes`'s DMA-capable
allocations and PSA fails with `PSA_ERROR_INSUFFICIENT_MEMORY (-141)`.

Counter-measures (current `sdkconfig` + `wstunnel.cpp`):

| Buffer | Size | Location | Why |
|--------|------|----------|-----|
| `s_send_frame`            | IO_CHUNK + 4  | PSRAM   | only bytes we own; PSA needs DRAM |
| `s_pump_buf`              | IO_CHUNK      | PSRAM   | same |
| `s_rx` (RX reassembly)    | up to RX_BUF_MAX (16 KB) | PSRAM | same |
| `s_pending[i].data`       | up to PENDING_BUF_MAX (16 KB) | PSRAM | same |
| `esp-aes` DMA scratch     | ~IO_CHUNK     | DRAM (MALLOC_CAP_DMA) | HW crypto cannot use PSRAM on P4 today |
| mbedtls / PSA contexts    | 0.5–4 KB each | PSRAM (threshold 2 KB) | freed DRAM for crypto |
| `httpd` per-socket scratch | `CONFIG_HTTPD_MAX_REQ_HDR_LEN` (4 KB) each | DRAM | sized at httpd_start, persistent |

Sdkconfig knob: `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048` — anything
≥ 2 KB lands in PSRAM.  The 200 MHz HEX PSRAM penalty is negligible
for the access patterns here (8-cycle latency on bursty I/O bytes,
not in a tight loop).

---

## IO_CHUNK sizing

`IO_CHUNK = 4096` is the sweet spot:

- Halves the TLS records per ~50–100 KB font transfer vs 2 KB
  (~25–40 % throughput win measured).
- Each TLS record costs a fixed HW crypto setup + GCM tag + WS
  framing; bigger payload amortises better.
- `esp-aes` DMA buffer scales linearly; 4 KB fits comfortably in
  DRAM with the PSRAM-offloaded mbedtls allocations above.
- WiFi MTU 1500 → ~3 segments per record; past 8 KB the
  segments-per-record curve flattens (no further win) while DMA
  pressure rises.
- `esp_websocket_client cfg.buffer_size = 8192` — must be
  ≥ IO_CHUNK + 4 (binary stream-id header) with headroom so the
  client doesn't internally fragment.

Going to 16 KB would force moving AES DMA to PSRAM (driver-side
work) — not worth it.

---

## Nagle on loopback is poison

**The most painful bug of this session.**  When `httpd:81` writes a
50-byte WS `setting` frame (or a 2-byte PING/PONG) to its loopback
socket, Nagle's algorithm waits for "more data" up to the delayed-
ACK timer.  Combined with idle ACKs from our end, observed latency
spiked to **5+ seconds** for LCD → web propagation through the
tunnel.  LAN-direct browsers were unaffected because there's no
loopback hop — `httpd:80` writes straight to WiFi.

Fix: `setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes))`
on the wstunnel side of every `connect_local` socket.  Loopback has
zero RTT and zero congestion, so Nagle buys nothing.  The httpd side
flushes per-write anyway because it emits chunks individually.

If you ever wonder why LAN is fast but tunnel is laggy, this is the
first place to look.

---

## Troubleshooting quick-reference

| Symptom | First place to look |
|---------|---------------------|
| LCD → web latency 5 s+ on tunnel, fast on LAN | TCP_NODELAY in `connect_local` |
| "WebSocket is closed before the connection is established" repeating | `lru_purge_enable` on the server hosting `/ws`; or wsServer cap exceeded |
| `pending timeout, closing` after a few page loads | Stream-table full of keep-alive zombies; `evict_idle_locked` not working or too conservative |
| `esp-aes: Failed to allocate memory for len buffer` | IO_CHUNK too big, or PSRAM threshold too high pushing mbedtls into DRAM |
| `PSA -141 (INSUFFICIENT_MEMORY)` on initial WSS connect | DRAM pressure at boot — check `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`, `MAX_OPEN_SOCKETS_WS`, `wsCfg.stack_size` |
| 4-byte WS frames spamming the monitor | `pump_task` not coalescing — httpd's chunked-transfer encoding emits size-hex / payload / CRLF separately |
| WS disconnects every ~60 s when no settings change | `wsReaper` not sending control PING; check the task is started and the ping branch fires |
| `wsCount reaper: 2 → 1` warnings | benign drift — `httpd_close_fn` occasionally misses decrements; reconcile fixes it |
| `httpd_register_uri_handler: handler /ws already registered` | URI ordering — `/ws` must register BEFORE the `/*` wildcard when `httpd_uri_match_wildcard` is set |

---

## Validation recipes

- **Latency**: tap `+` on the LCD setpoint, expect ≤ 200 ms before the
  web UI updates over tunnel.  Slower → enable a temporary
  `ESP_LOGI` in `broadcastControlChanges::emit` to time-stamp the
  firmware emit; compare against `stream N: M bytes → server` in
  the wstunnel log.
- **Routing**: monitor should show `stream N → :81, flushed M bytes`
  for `/ws` and `stream N → :80, flushed M bytes` for assets.  If
  `/ws` lands on `:80` the sniff in `pick_local_port` is failing.
- **Keepalive**: `wsReaper` logs `ws keepalive ping: sent=K failed=0`
  every 20 s.  DevTools can't reliably show control frames; trust
  the log + observed long-run stability.
- **Tunnel slot health**: `evicting idle stream N (idle X ms)` lines
  should be rare and only after several reloads.  If they appear
  every few seconds in steady state, the LRU threshold is too tight
  or the browser is misbehaving.

---

## What NOT to change without measuring first

- Don't raise `MAX_OPEN_SOCKETS_WS` beyond 4 unless DRAM has visible
  headroom — each socket consumes 4 KB of httpd scratch from
  internal heap.
- Don't disable the loopback `TCP_NODELAY`.
- Don't re-enable JS text heartbeats; the WS control PING is
  sufficient and one heartbeat is easier to reason about than two.
- Don't return `lru_purge_enable=true` on `wsServer`.  WS clients
  are deliberately long-lived; any eviction policy on that pool
  re-introduces the connect-storm bug.
