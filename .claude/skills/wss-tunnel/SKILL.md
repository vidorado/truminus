# Skill: WSS reverse tunnel — firmware side

The story behind `main/wstunnel.cpp` + `main/webserver.cpp`. Use this
when debugging tunnel-specific firmware behaviour: latency that
doesn't reproduce on LAN, dying WebSocket, stuck "pending timeout,
closing" loops, mbedtls/PSA OOM during TLS handshake.

The **server-side bridge** is a separate codebase (under `server/`
during this monorepo phase, intended to split into its own repo). Its
operational design — cache, queue, Passenger keepalive, nginx
gotchas, scanner block list — lives in `server/.claude/skills/
tunnel-bridge/SKILL.md`. This file covers only the device.

---

## Lifecycle of a single browser request (device perspective)

1. Bridge sends `{"type":"open","id":N}` followed by the request bytes
   as a binary WS frame `<4-byte BE id><payload>`.
2. `wstunnel.cpp` buffers the open in `s_pending` until at least 8
   bytes of request data arrive, then `pick_local_port()` sniffs the
   HTTP method+path:
   - `GET /ws…` → loopback `127.0.0.1:81` (WS-dedicated httpd)
   - everything else → loopback `127.0.0.1:80` (static-asset httpd)
3. `connect_local(port)` opens the loopback socket with
   `TCP_NODELAY=1` (see "Nagle on loopback" below), flushes the
   buffered request, and assigns the new `Stream` slot with
   `is_ws = (port == LOCAL_WS_PORT)`.
4. `pump_task` drains the loopback fd in 4 KB coalesced reads and
   ships each batch to the bridge as a binary WS frame
   `<4-byte BE id><payload>`. Bridge unwraps and writes to the
   matching browser TCP.

For ESP → browser (a `setting` update emitted by `main.cpp`):
`wsQueueSend` → `wsQueueDrain` (in `wsPumpTask`, 100 ms cadence) →
`httpd_ws_send_frame_async(wsServer:81, fd, …)` → kernel writes the
WS frame to the loopback → `pump_task` reads it → `send_data` over
the WSS control channel → bridge → browser.

Wire protocol is documented in `server/.claude/skills/tunnel-bridge/
SKILL.md`. Both sides must match.

---

## Why two httpd instances (`:80` and `:81`)

A single httpd with `lru_purge_enable=true` is the obvious
configuration, **but** `:80` carries an avalanche of parallel asset
GETs during a page load. The connected `/ws` becomes the least-
recently-active socket and gets evicted mid-flight, which the browser
surfaces as *"WebSocket is closed before the connection is
established"*. `ReconnectingWebSocket` then reopens, eating another
slot, and the system reconnect-storms.

The fix is QoS by *socket pool* rather than priority hints:

| httpd | port | max_open_sockets | lru_purge | clients |
|-------|------|------------------|-----------|---------|
| `httpServer` | 80 | 12 | true  | assets + LAN-direct WS |
| `wsServer`   | 81 | 4  | false | tunneled WS only       |

`pick_local_port()` routes `GET /ws` to `:81`; an asset flood on `:80`
can never touch the WS pool. `:80` keeps a `/ws` handler too so LAN-
direct browsers (no tunnel) work without changes.

`wsCountFromHttpd` and `wsQueueDrain` iterate both servers; broadcasts
reach LAN and tunneled clients alike.

`ctrl_port` (default 32768) **must differ between httpd instances** —
they conflict silently otherwise and you lose the work-queue signal
that wakes `httpd_ws_send_frame_async`. We use 32768 and 32769.

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
the open. `Stream::is_ws` ensures the live WebSocket is exempt.

`Stream::last_active_us` is updated in two places: `pump_task` after a
successful read, and `handle_data` after queueing bytes for the local
socket.

Note: the bridge now also closes streams proactively when it sees the
chunked terminator in the response, so most streams free their slot
before idle eviction kicks in. The firmware-side eviction remains as
the safety net for clients that disregard the close frame.

---

## BasicAuth gate (tunnel-only)

Tunneled requests are challenged with HTTP BasicAuth before they reach
the loopback httpds: fixed user `truminus`, password from NVS
`tunnel/pass` (set on the LCD tunnel screen; empty = disabled).
LAN-direct access is never challenged.

The check lives in `try_open_locked()`, NOT in `webserver.cpp`,
because (a) only tunnel traffic passes through there, and (b)
`esp_http_server` never invokes the URI handler for the WS-upgrade
leg, so an httpd-side check could not 401 the `/ws` handshake.
Mechanics:

- The expected `base64("truminus:<pass>")` is precomputed into
  `s_auth_b64` (`rebuild_auth_locked()`) on config load/save.
- The open defers (same mechanism as the port sniff) until the full
  header block (`\r\n\r\n`) is buffered, then scans for
  `Authorization: Basic <value>`. Bad/missing → a canned `401` +
  `WWW-Authenticate: Basic realm="TruMinus"` is sent back through the
  tunnel via `send_data` + `close`. The browser shows its native
  prompt and then attaches the header to every request, including the
  WS upgrade.
- The check is **per-connection** (first request of each keep-alive
  stream only). Deliberate: subsequent requests reuse an
  authenticated TCP stream; a fresh attacker connection always hits
  the gate.
- The bridge is untouched — the 401 flows as opaque stream bytes.

---

## Reconnection ladder — every way the WSS comes back

`esp_websocket_client` (v1.7.0) **permanently kills its task on a
server-initiated clean close** unless `enable_close_reconnect` is set:
the close path dispatches `WEBSOCKET_EVENT_CLOSED` (NOT
`DISCONNECTED`) and exits. A bridge restart/redeploy does exactly a
clean close. Field symptom: tunnel dead after every server deploy,
`s_connected` stuck `true`, watchdog blind, until power cycle.

Defense in depth, in order:

1. **Transport errors** (RST, TLS failure, no PONG in 30 s) →
   component auto-reconnect (`reconnect_timeout_ms = 5000`).
2. **Clean server close** → `cfg.enable_close_reconnect = true`
   makes the component reconnect instead of exiting.
3. **Task exits anyway** (`WEBSOCKET_EVENT_CLOSED` /
   `WEBSOCKET_EVENT_FINISH` handlers) → `s_connected = false`, which
   arms…
4. **Disconnect watchdog** (`WEDGE_REBUILD_MS`, 2 min in `pump_task`):
   disconnected + enabled + WiFi up → full client rebuild via
   `EV_RECONNECT`.
5. **Connected-but-silent watchdog** (`RX_SILENCE_REBUILD_MS`, 90 s):
   every inbound frame (PONGs included) stamps `s_last_rx_ms` in
   `WEBSOCKET_EVENT_DATA`; with our 10 s pings, >90 s of RX silence
   while "connected" means the client task is wedged beyond delivering
   events → rebuild. Covers the stuck-TLS-write wedge where
   `s_connected` would lie forever.

Deadlock trap when touching the event handler: `DISCONNECTED`/
`CONNECTED` cleanup takes `s_lock` with a **bounded** (2 s) timeout.
The supervisor holds `s_lock` while blocked in
`esp_websocket_client_stop()` waiting for the client task to set
STOPPED_BIT — and these events are dispatched *from* the client task.
An unbounded take deadlocks both. Skipped cleanup is safe:
`stop_client_locked()` and the CONNECTED branch drop streams anyway.

`CONNECTED` must drop stale streams: after a clean-close recovery the
bridge restarts its stream-id counter, and a surviving entry with the
same id would swallow the new stream's bytes.

---

## Keepalive — WS control PING, not text

Plesk/nginx has `proxy_read_timeout` set to a few seconds by default
on a fresh install. With no ESP → browser traffic for that window
(heating idle, BMS quiet) the proxy kills the WS. The wsReaper task
in `webserver.cpp` sends a real WS PING frame (opcode 0x9, zero
payload) every 20 s to every WS fd across both servers. Browsers
PONG automatically — invisible to JS, no message handler needed.

We deleted the older approach (`ws.send('ping')` from JS every 10 s
+ no-op handler in `wsHandler`) because it only covered browser →
ESP and added churn to the message channel for no benefit.

This covers the **inner** browser-side WebSocket (browsers ↔ device
through the tunnel). The **outer** ESP-to-bridge WSS is kept alive by
`esp_websocket_client`'s built-in ping (10 s default) plus the
bridge's nginx `proxy_read_timeout 120s` setting.

---

## Memory: where every byte lives

The default IDF behaviour (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`)
serves anything < 16 KB from internal DRAM — that includes every
mbedtls / PSA allocation (cert chains, key contexts, etc.). On the
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
≥ 2 KB lands in PSRAM. The 200 MHz HEX PSRAM penalty is negligible
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

**The most painful early bug.** When `httpd:81` writes a 50-byte WS
`setting` frame (or a 2-byte PING/PONG) to its loopback socket,
Nagle's algorithm waits for "more data" up to the delayed-ACK timer.
Combined with idle ACKs from our end, observed latency spiked to
**5+ seconds** for LCD → web propagation through the tunnel.
LAN-direct browsers were unaffected because there's no loopback hop —
`httpd:80` writes straight to WiFi.

Fix: `setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes))`
on the wstunnel side of every `connect_local` socket. Loopback has
zero RTT and zero congestion, so Nagle buys nothing. The httpd side
flushes per-write anyway because it emits chunks individually.

If you ever wonder why LAN is fast but tunnel is laggy, this is the
first place to look.

---

## send_text / send_data timeouts

Split timeouts on the outbound WSS:

- `SEND_CTRL_TICKS = 1 s` for the ~26-byte control frames (open /
  close / hello). If TLS can't flush 26 bytes in 1 s the connection
  is dead; the bridge tolerates a missing close (it has its own
  close paths).
- `SEND_DATA_TICKS = 5 s` for data frames (≤ 4 KB). Long enough to
  ride TLS backpressure spikes, short enough that the pump task
  doesn't stall the rest of the slot table.

After any `send_data` failure the pump `break`s the iteration instead
of `continue`ing — the next iteration re-snapshots the stream table.
Trying more streams in the same iteration just stacks timeouts.

---

## ENOTCONN / EBADF on loopback reads

After httpd finishes a chunked response and closes its loopback end,
lwIP can surface either `r=0` (EOF) or `ENOTCONN` (errno 128 on
picolibc) on the next non-blocking read, depending on TCP
state-machine timing. Both mean "transaction done, no more data" —
the pump treats them identically (`LOGD` not `LOGW`).

The pump snapshots `{id, sock}` under the lock and reads outside the
lock. If `handle_close` or `handle_data`'s write-fail path closes the
stream during that window, the snapshotted fd may have been recycled
to a different stream by `connect_local`. The close-path **must**
re-check under the lock that the stream still has the SAME fd as the
snapshot before freeing it or telling the server about it — otherwise
we close a stranger's stream and the server double-deletes.

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
| `Could not lock ws-client within 1000 timeout` followed by `ws send dropped (n=-1, len=24)` | Outbound TLS write congested; close ctrl couldn't get the mutex within 1 s. Benign at low frequency (server tolerates missing close), but if it floods, the data path is the real bottleneck. |
| `TCP_CLOSED_FIN errno=119` from `esp_websocket_client` | The remote side (nginx in front of the bridge) closed the WSS. Almost always nginx `proxy_read_timeout` too low — fix on the **bridge side**, not here. |

---

## Validation recipes

- **Latency**: tap `+` on the LCD setpoint, expect ≤ 200 ms before the
  web UI updates over tunnel. Slower → enable a temporary
  `ESP_LOGI` in `broadcastControlChanges::emit` to time-stamp the
  firmware emit; compare against `stream N: M bytes → server` in
  the wstunnel log.
- **Routing**: monitor should show `stream N → :81, flushed M bytes`
  for `/ws` and `stream N → :80, flushed M bytes` for assets. If
  `/ws` lands on `:80` the sniff in `pick_local_port` is failing.
- **Keepalive**: `wsReaper` logs `ws keepalive ping: sent=K failed=0`
  every 20 s. DevTools can't reliably show control frames; trust
  the log + observed long-run stability.
- **Tunnel slot health**: `evicting idle stream N (idle X ms)` lines
  should be rare and only after several reloads. If they appear
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
- Don't return `lru_purge_enable=true` on `wsServer`. WS clients
  are deliberately long-lived; any eviction policy on that pool
  re-introduces the connect-storm bug.
- Don't extend `SEND_DATA_TICKS` back to 10 s. The pump task is
  single-threaded; longer timeouts cascade across the slot table.

---

## Local httpd / WS server gotchas (`webserver.cpp`)

- **`esp_http_server` does not invoke the URI handler for the WS upgrade leg.**
  The handshake is handled internally before any handler runs
  (`components/esp_http_server/src/httpd_uri.c:362`: *"If the request is
  websocket handshake, then do not call the uri->handler"*). `req->method`
  stays `HTTP_GET` for every subsequent frame, so a
  `if (req->method == HTTP_GET) handshake()` branch eats every frame without
  reading it. Use `CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y` +
  `ws_post_handshake_cb` to detect new clients; `close_fn` for disconnects.
- **`httpd_get_client_list(handle, &fds, fds_array)` requires `fds_array` ≥
  `max_open_sockets`** — otherwise it returns `ESP_ERR_INVALID_ARG` silently and
  the WS broadcast loop sees zero clients. Use one `MAX_OPEN_SOCKETS` constant
  for both `cfg.max_open_sockets` and the local `int fds[…]`.
- **Do NOT set `Content-Length` when using `httpd_resp_send_chunk()`.** The two
  are mutually exclusive in HTTP; sending both produces invalid HTTP that
  confuses browsers and kills the tunnel (symptom: "502 Bad Gateway" +
  `errno=128` in `wstunnel` logs). `serveFile()` uses chunked transfer with a
  16 KB PSRAM buffer; the browser infers length from the final zero-length chunk.
- **`serveFile()` buffer lives in PSRAM** (`heap_caps_malloc(MALLOC_CAP_SPIRAM)`),
  not on the httpd task stack. The httpd stack is only 8 KB
  (`cfg.stack_size = 8192`); a 16 KB stack buffer would overflow it. Always use
  PSRAM for large per-request allocations.
- **Dead WS fds must be closed actively.** `httpd_ws_send_frame_async` returning
  an error does **not** trigger `close_fn` — the socket stays open, occupying
  one of the limited slots. The `wsReaper` task PINGs every 20 s and
  `wsQueueDrain` broadcasts to all fds; both call `httpd_sess_trigger_close()`
  on failure, which invokes `close_fn` → decrements `wsClientCount` → frees the
  slot. Without this, tunnel reconnects accumulate zombie fds until no browser
  can connect.
