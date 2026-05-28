# Skill: TruMinus tunnel bridge — server-side design + gotchas

The end-to-end story of `app.js`. Use this when debugging the bridge:
streams stuck, cache not filling, Passenger respawning, browser 502/503,
`tunnel: ESP disconnected` loops. Per-symbol comments in `app.js`
explain individual decisions; this file links them into one story and
catalogs the landmines we already stepped on.

The firmware end of the same tunnel lives in `main/wstunnel.cpp` of the
TruMinus repo — both sides must agree on the wire protocol below.

---

## Wire protocol (shared contract with firmware)

Control frames are **text JSON**, bulk data is **binary**:

```
ESP   → srv  {"type":"hello","node":"truminus","token":"<shared>"}   text
srv   → ESP  {"type":"open", "id":<n>}                                text
both         <4-byte BE id><payload bytes…>                          binary
both         {"type":"close","id":<n>}                                text
```

`id` is a monotonic uint32 minted by the bridge per browser TCP.
Binary frames save ~33 % bandwidth and a parse/format roundtrip vs.
the original JSON+base64 phase-1 protocol; both sides must match.

A change here breaks both sides simultaneously — bump the protocol
version (currently implicit "v2 binary") and gate by hello.

---

## Why we hijack raw TCP

A naïve approach would be `http.createServer((req, res) => proxyToEsp(req, res))`,
but `req` and `res` go through Node's HTTP parser and only support
request/response semantics. The browser also speaks WebSocket on
`/ws` after an `Upgrade` handshake; once upgraded the connection is
opaque bytes and Node's `http.Server` won't let us touch them.

Hijacking at the TCP layer (replacing the default `connection`
listener on `http.Server` with our sniffer) gives us one uniform code
path for HTTP and WS:

- Sniff the first 1 KB to decide what this connection is
- `GET /tunnel … Upgrade: websocket` → ESP control channel, hand back
  to Node's HTTP parser so `WebSocketServer` can finish the handshake
- Anything else → opaque byte stream, forward to the ESP as a tunneled
  stream

The trick that made this work under Plesk: we **must** still expose
the bound port via `http.Server.prototype.listen` (Phusion Passenger
hooks it to detect readiness). `net.Server.listen` works fine but
Passenger times out the spawn with *"A timeout occurred while spawning
an application process."*

---

## Concurrency queue (browser → ESP)

Browsers fire 10–20 parallel asset requests on cold loads. The ESP's
pump task drains its outbound TLS sequentially; if it has > 3-ish
streams competing for the same WSS pipe, every one of them blocks and
cascades into ENOTCONN reads, dropped close ctrl frames, and the
ESP's local httpd choking on full loopback buffers
(`httpd_sock_err: error in send : 11`).

`MAX_CONCURRENT = 3` (a quarter of the firmware's `MAX_STREAMS = 12`)
gives the device enough breathing room. Browser connections beyond the
cap sit in `pendingQueue` with their TCP paused (kernel buffers
incoming bytes for us). Drain triggers on:

- A `socket.on("close")` from the browser side (rare for keep-alive
  TCPs — see below)
- The proactive chunked-terminator detection firing on the ESP-side
  data stream — this is the **common path**
- A `{"type":"close","id":N}` arriving from the ESP
- The 30-s idle reaper sweeping a stuck slot (safety net)

`PENDING_TIMEOUT_MS = 90000` is deliberately long: the first cold load
through a slow tunnel can take 30+ s to drain, and timing items out
early prevents the reverse cache from ever populating. The browser's
own connection timeout (~60–90 s) terminates abandoned items if the
user navigates away.

`PENDING_QUEUE_LIMIT = 64` caps memory against burst/scanner storms.

---

## Reverse cache

Cache-busted asset URLs (`?v=<sha1>`) are immutable by construction —
the firmware build emits a different querystring whenever bytes change
(see `scripts/cache_bust.py` in the firmware repo). The bridge caches
the **raw HTTP response from the ESP**, including status line, headers
and the chunked-encoded body, on disk:

```
cache/<sha256("METHOD URL")>.bin
```

**Reads.** `tryServeFromCache` is called *before* the queue check:
cache hits never wake the ESP, even when the queue is full and even
when the ESP is offline (cached assets survive outages). Reads are
synchronous `fs.readFileSync` — files are < 200 KB and disk hits cost
microseconds; making this async would add latency without measurable
gain.

**Writes.** During a miss we accumulate every binary payload from the
ESP in `stream.buf` (capped at `CACHE_MAX_BYTES = 2 MB` to bound
heap). On *any* stream close (proactive terminator detection,
browser-close, or ESP close) we run `cacheCommit` which validates
before persisting:

- `^HTTP/1.[01] 200 ` (only successful responses)
- `Transfer-Encoding: chunked` header (the ESP's exclusive framing)
- Ends with `0\r\n\r\n` (proper chunked terminator)

Anything else is a partial response or an error page and **must not**
poison the cache. The committed bytes can be replayed verbatim.

**Why we capture on the ESP-close path was insufficient.** Originally
we only committed when the ESP sent `{"type":"close"}`. Under TLS
congestion the firmware drops close ctrl frames (1-s
`SEND_CTRL_TICKS` timeout), so the cache never wrote a single file on
the very loads that needed it most. The fix was to *also* commit on
`socket.on("close")` from the browser side, which fires reliably as
soon as the browser sees the chunked terminator. Either path can run
first; whichever runs sets `stream.buf = null` so the other becomes a
no-op.

**Invalidation is the URL.** A firmware reflash regenerates the
`?v=<hash>` querystrings; the next request maps to a different cache
key, falls through to the ESP, and a fresh entry is written next to
the now-orphaned old one. Stale entries accumulate; a cron
`find … -mtime +30 -delete` is enough.

Cache directory writability is **probed at boot** (`fs.accessSync` +
the boot-time `[INFO] cache enabled at …` / `[WARN] cache DISABLED:
…` log). Without this probe a permission regression silently turns
every request into a miss for the lifetime of the process.

---

## Proactive stream close — the keep-alive trap

The single most painful bug of the bridge: streams were never freeing
their slot, the cap-3 queue would fill on the first 3 requests and
never drain.

Root cause: nginx upstream keep-alive (and browser HTTP/2 multiplexed
to nginx, then demuxed to HTTP/1.1 upstream) leaves the underlying
TCPs open after each response. `socket.on("close")` never fires
because the socket is alive and idle, just waiting for the next
request that may never come. From the bridge's view the stream is
"alive" forever and its slot is consumed indefinitely.

Fix at the protocol layer: detect the chunked-transfer terminator
`0\r\n\r\n` in the ESP's response bytes and close the stream
ourselves. A 5-byte rolling tail (`stream.tail`) handles the
terminator landing across two binary frames. On detection:

1. `cacheCommit(stream.req, …)` if applicable
2. `sendCtrl({type:"close", id})` so the ESP can free its loopback slot
3. `socket.end()` — graceful FIN to the browser
4. `streams.delete(id)` + `drainPendingQueue()` — slot recycles

Set `stream.done = true` to short-circuit any straggler binary frames
that arrive after we've already committed (rare but possible).

**`/ws` is exempt** (the firmware's live-data WebSocket inside the
tunnel). Tag at attach time with `isWs = /^\/ws(?:\?|\/|$)/.test(url)`,
return early from terminator detection, and skip in the reaper sweep.
Otherwise we'd kill the live LCD pipe every 30 s.

---

## Idle reaper — safety net only

A `setInterval` every 5 s closes any non-WS stream older than 30 s,
in case the terminator detection ever misses (non-chunked responses,
buggy edge cases). In practice the proactive close handles 100 % of
the well-formed traffic; the reaper has never fired in normal
operation. Keep it as belt-and-braces.

If it ever starts firing routinely in steady state, something is
emitting non-chunked responses or the terminator detection's tail
logic regressed.

---

## Passenger idle-kill — the self-ping workaround

Plesk's Phusion Passenger idle-kills workers whose only traffic is
raw TCP after a WS upgrade. The post-upgrade byte stream doesn't
count as "HTTP activity", so even with constant ESP→browser data
flowing through us, Passenger considers the worker idle and kills it
after `passenger_pool_idle_time` (default 300 s). The ESP then has to
reconnect; every queued asset 502's during the gap.

The clean fix would be `passenger_pool_idle_time 0;`, but that
directive lives in nginx's `http` context (global), and Plesk's
*"Additional nginx directives"* per-vhost panel is `server` context —
the directive is rejected at config-validate time:

> `"passenger_pool_idle_time" directive is not allowed here in vhost_nginx.conf:N`

Workaround: feed Passenger one legitimate HTTP request per minute
from inside the app.

- `setInterval(60 s)` fires `https.get(<origin>/__keepalive__)`
- The connection sniffer recognises `GET /__keepalive__` (matched
  *before* `attachStream`) and answers with a tiny `200 OK`,
  **never forwarding to the ESP**

The public origin is auto-latched from the first inbound `Host:`
header (with `X-Forwarded-Proto` if Plesk supplies it, falling back
to `https`). `KEEPALIVE_URL` env var overrides the auto-detect — only
useful for testing or if the latch never warms up (no real traffic
ever arrived).

Belt-and-braces: `passenger_min_instances 1;` in the Plesk vhost
directives keeps at least one worker pre-spawned, so even if a kill
slips through, the gap shortens to milliseconds.

---

## nginx in front (Plesk or your own)

`proxy_read_timeout` defaults to 60 s on stock nginx but Plesk
ships a vhost template with **5 s** in places (we saw it on a fresh
Plesk install). When the ESP's data send blocks for 5 s on slow TLS
(`SEND_DATA_TICKS` on the firmware side), it can't ping during that
window — nginx sees no upstream traffic for 5 s and FINs the WSS
connection. The ESP logs `ESP_ERR_ESP_TLS_TCP_CLOSED_FIN` and
reconnects.

Mandatory directive in Plesk → *Additional nginx directives*:

```nginx
proxy_read_timeout 120s;
proxy_send_timeout 120s;
```

`120 s` is loose enough to swallow even pathological TLS backpressure
without holding zombie connections.

---

## HTTP/2 from browser, HTTP/1.1 to upstream

Plesk's front nginx serves browsers over HTTP/2 but uses HTTP/1.1
toward our Node app (this is `proxy_pass`'s default; HTTP/2 upstream
isn't standard). Our connection sniffer expects HTTP/1.1 request
lines and works fine because of this demultiplexing.

The user-visible side effect: nginx access logs say `HTTP/2.0` for
every request, which is **not** what we see internally. Don't be
confused — our `BLOCKED_PATH_RE`, `parseRequestLine`, etc. all match
the HTTP/1.1 form because that's what arrives at our TCP socket.

---

## Scanner block list

Internet-wide scanners constantly probe public hosts for
`.env`, `.git/*`, `actuator/env`, `_next/static/...`, `%22…`, and so
on. Without filtering, the bridge forwards every probe to the ESP,
which has to allocate a tunnel stream, fetch from LittleFS, and 404 —
wasted bandwidth and scheduler cycles, plus pollutes the queue under
the cap-3 limit.

`BLOCKED_PATH_RE` matches in the sniffer **before** `attachStream`.
On hit:

- Answer `404 Not Found` from Node directly (12-byte body, fast close)
- Log to `stderr` with the stable format `"<iso-ts> BLOCKED <ip>
  <method> <path>"` so fail2ban can tail it
- Optionally mirror to `BLOCK_LOG` file path (env var)

Client IP comes from `X-Forwarded-For` (Plesk's front nginx sets it),
falls back to `socket.remoteAddress`. The fail2ban filter regex is
trivial — see the README.

When a new probe family appears in the access logs (something like
`/random_new_path`), extend the regex; tests live as a sanity script
in the commit messages.

---

## Logging — levels and what goes where

`LOG_LEVEL` env (`ERROR` / `WARN` / `INFO` / `DEBUG`, default `WARN`)
gates a struct-with-methods exposed as `LOG.error / .warn / .info /
.debug`. Lines prefix the level in brackets (`[WARN] …`) so a
filter-by-level grep stays trivial.

| Level | Examples | Frequency |
|-------|----------|-----------|
| ERROR | `cache write failed`, `cache commit error`, `keepalive: bad origin`, `stream X capture error` | rare, persistent failures |
| WARN  | `tunnel: ESP disconnected`, `cache DISABLED`, `keepalive ping error`, `reaping idle stream`, `tunnel: replacing previous` | unusual but recoverable |
| INFO  | `tunnel: listening`, `tunnel: ESP connected`, `hello node=…`, `cache enabled`, `log file at`, `keepalive enabled`, signal received | low-frequency state changes |
| DEBUG | `forward → ESP id=… (slots=X/3, queue=Y)`, `done ← ESP id=…`, `browser-close id=…`, `cache HIT/MISS→STORE/SKIP` | every request — floods at scale |

Default `WARN` is the production setting: silent unless something is
wrong. Bump to `INFO` to watch boot+state changes, or `DEBUG`
temporarily while diagnosing — every request flow becomes traceable
(slot/queue counters in the line make queue-stalling problems
visually obvious).

`LOG_FILE` mirrors to a file alongside stdout (defaults to
`./logs/server.log` relative to cwd). Falls back to stdout-only if
the directory isn't writable — same fallback shape as `CACHE_DIR`.

---

## ESP-replacement on duplicate hello

If a `hello` arrives while `esp` is already set, the previous WS is
closed with code `4000 "superseded"` and `teardownStreams("ESP
replaced")` synthesises 502 to every in-flight browser stream so
Passenger/nginx never see "Incomplete response received from
application". Without that synthesis the browser hangs on a half-open
TCP until the OS keepalive fires (~10 min).

This path is hit when:

- The ESP's auto-reconnect races our close-detection: it reconnects
  while we still think the old session is alive
- A second physical ESP was paired with the same token (rare, but
  the firmware doesn't enforce uniqueness)

The log line is `[WARN] tunnel: replacing previous ESP connection`.

---

## Troubleshooting quick-reference

| Symptom | First place to look |
|---------|---------------------|
| Every request 502 `tunnel offline` | ESP never said hello, or `esp.readyState !== 1`. Check `tunnel:` log for the last connect/disconnect pair. |
| Cap-3 queue stalled, items 503 after 90 s | Streams aren't freeing. Terminator detection regressed (chunked tail not matching) or response isn't actually chunked. `LOG_LEVEL=DEBUG`, look for streams without a matching `done ← ESP`. |
| `cache DISABLED: …` at boot | `CACHE_DIR` not writable. Likely Passenger user vs. directory owner; `chown` or set `CACHE_DIR=` to a writable path. |
| Cache directory exists but no files written | We never reach `cacheCommit`. Either streams aren't closing (see above) or the response fails `isCompleteOk200` validation. Bump to `DEBUG` and look for `cache SKIP` lines. |
| `tunnel: listening on :…` reappears every ~5 min | Passenger idle-kill firing. Confirm `[INFO] keepalive enabled: …` is present at boot; if not, the `Host:` latch never ran (no real traffic between boots) — set `KEEPALIVE_URL`. |
| `tunnel: ESP disconnected` 5–30 s after each request | nginx `proxy_read_timeout` too low. Set to `120s` in vhost directives. |
| Browser sees `ERR_CONTENT_DECODING_FAILED` | Truncated gzip — the ESP-side data send failed mid-response. Usually correlated with `[WARN] stream X capture error` and TLS backpressure. Check `LOG_LEVEL=DEBUG` for partial slots/queue lines. |
| 502s every ~5 min on previously cached assets | The cache was wiped by a deploy or the worker restarted with a fresh cwd. Confirm `CACHE_DIR` is absolute, not relative to a transient cwd. |
| `tunnel: replacing previous ESP connection` flood | ESP firmware is reconnect-looping faster than our close-detection clears the old session. Look for the firmware-side cause (TLS handshake failing, mbedtls OOM, etc.) — the bridge is doing the right thing. |
| Self-ping never fires (`keepalive enabled` log absent) | `Host` header didn't carry the public hostname (proxied through some weird path). Set `KEEPALIVE_URL` explicitly. |

---

## What NOT to change without measuring

- **`MAX_CONCURRENT`** above 3. Raising it reintroduces the
  TLS-contention cascade. The cache fixes user-visible cold loads,
  not the device's per-stream throughput.
- **`PENDING_TIMEOUT_MS`** below ~60 s. Short timeouts in cap-3 mean
  the queue tail 503's before the cache can populate, and the next
  reload looks just as bad.
- **Cache validation rules.** Loosening any of (HTTP 200, chunked,
  `0\r\n\r\n` terminator) lets partial / error responses poison the
  cache. Eviction is by reflash → URL change; there's no expiry
  mechanism beyond that.
- **The TCP-hijack pattern**. Replacing it with `http.createServer(...
  )` handlers loses WebSocket transparency and forces parsing
  responses we currently just forward as opaque bytes.
- **`/ws` exemption.** Removing it kills the LCD data pipe every 30 s.

---

## Validation recipes

- **Cache fills**: tail the log with `LOG_LEVEL=DEBUG`, do a Ctrl+F5,
  see `cache MISS→STORE` for every cacheable asset. Second Ctrl+F5
  should produce only `cache HIT` lines — no `forward → ESP` lines
  for cacheable URLs.
- **Slot recycle**: in `DEBUG`, the `slots=X/3, queue=Y` counters in
  `forward →` and `done ←` lines should pair up cleanly. If `slots`
  monotonically increases and never returns to 0, the proactive close
  isn't firing.
- **Self-ping alive**: `LOG_LEVEL=INFO`, look for `keepalive enabled:
  …` at boot. Errors are at `WARN` (visible by default).
- **Scanner block**: `tail -F $BLOCK_LOG` while a scanner is hitting
  the host (Internet-wide scanners hit any public IPv4 within minutes
  of going live). One line per attempt with stable format.
