# doom.c architecture notes

## Control flow

1. **Browser** keeps an `<img>` tag pointed at `/doom.mjpeg?session=N`. The
   server responds with `multipart/x-mixed-replace` data, rendering each JPEG
   chunk in sequence for a ~30 fps effect.
2. **Inputs** are posted to `/input?session=N`. The HTTP handler writes the raw
   character payload into the session FIFO, which Chocolate Doom reads via an
   adapter.
3. **Session lifecycle**: connecting to `/doom.mjpeg` (or `/session/new`) will
   fork a Chocolate Doom instance if the slot is free. Idle sessions are torn
   down after a timeout.

## Processes

- `doom_server` (C) — Accepts HTTP connections, manages session structs that
  track process IDs, FIFOs, and framebuffers.
- `chocolate-doom` — Launched per session with `SDL_VIDEODRIVER=fbcon`. Its
  output is directed to a virtual framebuffer (`/dev/fbdoomN`).
- `fbcp`/`kmsgrab` (TBD) — In production we may need a helper to populate
  `/dev/fbdoomN` when running inside a hypervisor with no physical display.

## Outstanding questions

- Confirm `chocolate-doom` can draw into a software framebuffer on a headless
  Ubuntu droplet (may require `kmscube` or `xfbdev`).
- Decide whether we multiplex multiple framebuffers or lock to a singleton.
- Investigate latency from FIFO → Doom input queue; might need UDP proxy.
- How to throttle multiple viewers per session while keeping input authority
  to one client?

## Logging + observability ideas

- Dump per-session logs to `SESSION_DIR/session.log`.
- Include a lightweight heartbeat endpoint (`/healthz`).
- Collect backtraces on crashes via `backtrace()` + `addr2line`.
