# TruMinus tunnel — server side

Public Node.js bridge that lets browsers reach a TruMinus ESP32-P4 sitting
behind CGNAT. The device dials out to this server over WSS; this server
hijacks the public TCP port (Plesk's vhost or your own VPS) and muxes
browser traffic to the device over the WS as opaque byte streams.

```
  Browser ── HTTPS ──► nginx ──► Node (this) ──── WSS ◄──── ESP32-P4
                                       └── reverse cache ──┘
```

## What's in here

- **One process, one device.** A single ESP is expected per server; if a
  second one connects it replaces the first.
- **Reverse cache.** Cache-busted assets (URLs with `?v=<hash>`) are
  stored on disk after the first fetch and replayed from there on
  subsequent requests, byte-for-byte. The ESP isn't touched.
- **Concurrency queue.** At most `MAX_CONCURRENT=3` browser→ESP streams
  in flight at once. Overflow is held in a paused-TCP queue. Prevents
  the firmware's pump task from drowning in parallel transfers.
- **Scanner block list.** Common vulnerability-probe paths (`.env`,
  `.git/*`, `actuator`, `_next`, `%22…`, etc.) are 404'd at the edge
  and logged for fail2ban.
- **Self-ping keepalive.** Hits its own `/__keepalive__` every 60 s so
  Phusion Passenger doesn't idle-kill the worker (Passenger doesn't
  count raw-TCP traffic after the WS upgrade as "HTTP activity").

The wire protocol is documented at the top of `app.js`.

## Requirements

- Node.js 18 or newer
- One TCP port reachable from the internet (443 in front of nginx is the
  intended deploy; nothing stops you from running it bare on a VPS)

## Configuration — environment variables

| Var               | Default                       | Required | Notes |
|-------------------|-------------------------------|:--------:|-------|
| `TUNNEL_TOKEN`    | —                             |    ✓     | Shared secret with the ESP (must match the firmware's `tunnel/token` NVS key). |
| `PORT`            | `3000`                        |          | TCP port to bind. Passenger overrides this automatically on Plesk. |
| `CACHE_DIR`       | `./cache`                     |          | Where the reverse cache writes `<sha256>.bin` entries. Falls back to disabled if the dir isn't writable. |
| `LOG_FILE`        | `./logs/server.log`           |          | Append-only mirror of stdout. Falls back to stdout-only if creation fails. |
| `LOG_LEVEL`       | `WARN`                        |          | `ERROR` / `WARN` / `INFO` / `DEBUG`. Bump to `DEBUG` to see every request flow through. |
| `KEEPALIVE_URL`   | _auto-detected_               |          | Public origin used by the self-ping (e.g. `https://example.com`). Latched from the first inbound `Host:` header if unset. |
| `BLOCK_LOG`       | _none_                        |          | Optional path to append `"<ts> BLOCKED <ip> <method> <path>"` for fail2ban. Stderr always carries the same line. |

Paths are resolved against the process working directory.

## Running on plain Unix

```bash
git clone <your repo> /opt/truminus-tunnel
cd /opt/truminus-tunnel/server
npm ci

# Pick a long random token; this MUST match the device's NVS tunnel/token
export TUNNEL_TOKEN="$(head -c 32 /dev/urandom | base64)"
export PORT=8443

node app.js
```

In production use a process supervisor. Minimal systemd unit:

```ini
# /etc/systemd/system/truminus-tunnel.service
[Unit]
Description=TruMinus reverse tunnel
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/truminus-tunnel/server
ExecStart=/usr/bin/node app.js
Restart=on-failure
RestartSec=5
User=truminus
Environment=PORT=8443
Environment=TUNNEL_TOKEN=changeme
Environment=KEEPALIVE_URL=https://truminus.example.com
Environment=LOG_LEVEL=WARN

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now truminus-tunnel
journalctl -u truminus-tunnel -f
```

Put nginx (or Caddy/Apache) in front for TLS termination and forward
`/` to `127.0.0.1:8443`. Bump the proxy read timeout so brief congestion
on the tunnel doesn't kill the WSS:

```nginx
location / {
    proxy_pass             http://127.0.0.1:8443;
    proxy_http_version     1.1;
    proxy_set_header       Upgrade $http_upgrade;
    proxy_set_header       Connection $http_connection;
    proxy_set_header       Host $host;
    proxy_set_header       X-Forwarded-Proto $scheme;
    proxy_set_header       X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_read_timeout     120s;
    proxy_send_timeout     120s;
}
```

When you don't run Plesk you don't need the self-ping — the daemon
runs forever on its own. Leave `KEEPALIVE_URL` unset and the periodic
self-ping won't activate (or set it anyway, it's harmless).

## Running on Plesk (Phusion Passenger)

Plesk's Node.js panel runs the app under Passenger, which spawns Node
when nginx receives traffic for the configured application root.

### 1. Set up the Node.js application

Plesk panel → domain → **Node.js**:

| Field                     | Value                                            |
|---------------------------|--------------------------------------------------|
| **Node.js version**       | 18 or newer                                      |
| **Document Root**         | _project root_ (whatever's above `server/`)      |
| **Application Root**      | `server`                                         |
| **Application Startup File** | `app.js`                                      |

Click **NPM install** to pull `ws`.

### 2. Custom environment variables

Same panel, **Custom environment variables**:

```
TUNNEL_TOKEN=<long random string, must match firmware's tunnel/token>
LOG_LEVEL=WARN
```

`PORT` is injected by Passenger automatically — leave it unset.
`KEEPALIVE_URL` is auto-detected from the first request's `Host`
header — leave it unset too unless you have a reason.

### 3. Additional nginx directives

Plesk panel → domain → **Apache & nginx Settings** → **Additional
nginx directives**:

```nginx
passenger_min_instances 1;
proxy_read_timeout 120s;
proxy_send_timeout 120s;
```

- `passenger_min_instances 1` keeps at least one worker ready at all
  times (even during respawn).
- `proxy_*_timeout 120s` gives the WSS enough slack when the device's
  TLS pipe is congested.

`passenger_pool_idle_time` would be the cleanest fix for the
idle-restart problem but Plesk doesn't expose it per-vhost; that's why
the app self-pings.

### 4. Verify

Restart the app from the Node.js panel and tail the log:

```bash
tail -f /var/www/vhosts/<domain>/server/logs/server.log
```

You should see roughly:

```
... [INFO] log file at /var/www/.../server/logs/server.log
... [INFO] log level: WARN
... [INFO] cache enabled at /var/www/.../server/cache
... [INFO] tunnel: listening on :<passenger-port>
... [INFO] tunnel: ESP connected
... [INFO] tunnel: hello node=truminus
... [INFO] keepalive enabled: https://<domain>/__keepalive__ every 60s
```

After a browser hits the site once, the `cache/` directory fills up
with `<sha256>.bin` entries. Subsequent loads (and Ctrl+F5) serve from
those without ever touching the ESP.

## Operations

**Log levels.** Default `WARN` is the production setting — only abnormal
events. Bump to `DEBUG` temporarily while diagnosing to see every
`forward → ESP`, `done ← ESP`, `cache HIT`, `cache MISS→STORE`.

**Reset the cache.** Stop the app and remove the directory:

```bash
rm -rf server/cache/*
```

It rebuilds lazily on the next page load. Old entries become orphans
when the firmware reflash rotates `?v=<hash>` URLs — they're harmless
but accumulate; sweep them with a periodic `find … -mtime +30 -delete`
if you care.

**fail2ban for scanners.** Set `BLOCK_LOG=/path/to/blocked.log`. The
log format is stable: `"<iso-ts> BLOCKED <ip> <method> <path>"`. A
trivial fail2ban filter regex:

```ini
[Definition]
failregex = BLOCKED <HOST> .*
```

**Health check.** The tunnel is healthy if you see periodic
`keepalive` pings succeed (only logged at `WARN` if they fail) and
`tunnel: ESP connected` is the most recent state change. Browser
requests under load go via `LOG.debug` only, so silence at `WARN` is
expected.

## Troubleshooting

| Symptom                                                  | Likely cause | Check |
|----------------------------------------------------------|--------------|-------|
| Browser sees `502 tunnel offline`                        | ESP isn't connected. | Last `tunnel:` line in the log — was the last event `disconnected`? |
| Browser sees `503 tunnel busy` after ~90s                | Stream slots stuck. | `LOG_LEVEL=DEBUG`, look for streams that never log `done ← ESP`. |
| `cache DISABLED: …` at boot                              | `CACHE_DIR` not writable. | `ls -ld <dir>`, fix owner/perms or set `CACHE_DIR` to a writable path. |
| `tunnel: listening on :…` reappears every ~5 min        | Passenger idle-kill firing despite self-ping. | Confirm `KEEPALIVE_URL` got auto-detected (`[INFO] keepalive enabled: …`). If not, set it explicitly. |
| `tunnel: ESP disconnected` 5–30 s after each request    | nginx `proxy_read_timeout` too low. | Bump to 120 s as shown above. |
| Repeated `cache write failed`                            | Disk full or perms regressed. | `df -h`, `ls -la cache/`. |
