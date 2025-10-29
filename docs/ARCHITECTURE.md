# doom.c architecture notes

## Control flow

1. **Browser** keeps an `<img>` tag pointed at `/doom.mjpeg?session=N`. The
   server responds with `multipart/x-mixed-replace` data, rendering each JPEG
 chunk in sequence for a ~30 fps effect.
2. **Inputs** are posted to `/input?session=N`. The HTTP handler resolves the
   payload to an X11 keysym and synthesises press/release events via the XTest
   extension so Chocolate Doom receives them directly.
3. **Session lifecycle**: connecting to `/doom.mjpeg` (or `/session/new`) will
   fork a Chocolate Doom instance if the slot is free. Idle sessions are torn
   down after a timeout.

## Processes

- `doom_server` (C) — Accepts HTTP connections, manages session structs that
  track process IDs, X11 state, and frame buffers for JPEG encoding.
- `chocolate-doom` — Launched per session with `DISPLAY=:99` and
  `SDL_VIDEODRIVER=x11`, drawing into a 320×200 window hosted by Xvfb.

## Outstanding questions

- Harden X11 window discovery so we survive Chocolate Doom restarts and
  multi-session experiments.
- Evaluate whether we need to juggle focus or pointer grabs to keep input
  deterministic when multiple viewers connect.
- How to throttle multiple viewers per session while keeping input authority
  to one client?

## Logging + observability ideas

- Collect per-session logs somewhere under `/var/log/doom/`.
- Include a lightweight heartbeat endpoint (`/healthz`).
- Collect backtraces on crashes via `backtrace()` + `addr2line`.
