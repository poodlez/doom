# doom.c deployment kit

Pure-C experiment to stream a playable FreeDoom session over HTTP. This repo
captures the scripts, configs, and server code needed to spin up a tiny
DigitalOcean droplet that runs Chocolate Doom headless under Xvfb, injects
keyboard events through the XTest extension, and exposes an MJPEG stream to
browsers.

> ⚠️ **Status:** Work in progress. The first milestone focuses on getting the
> minimal scaffolding in place – a headless C HTTP server, streaming pipeline,
> and reproducible deployment script. Tighten the loop before inviting players.

## Pieces

- `src/doom_server.c` — standalone HTTP server that launches Chocolate Doom,
  captures frames from an X11 window, streams MJPEG, and injects keyboard input
  via XTest.
- `scripts/setup_droplet.sh` — one-shot provisioning script for Ubuntu 24.04.
- `public/index.html` — barebones control pad that posts inputs to the server.
- `systemd/doom.service` — unit file used in production to keep the server alive.
- `docs/` — architecture notes, debugging tips, and follow-up todos.

## Deploy (DigitalOcean $6/mo)

1. Create a fresh Ubuntu 24.04 droplet and SSH in as root.
2. Copy the contents of `scripts/setup_droplet.sh` and run it once.
3. Point your `A` record (e.g. `doom.c`) to the droplet IP.
4. Browse to `http://your-domain/` to confirm you see the MJPEG canvas.

The script installs Chocolate Doom, FreeDoom WADs, compiles the C server, and
enables the systemd unit. Re-run it only on clean hosts; it is intentionally
destructive.

## Local iteration (optional)

The server expects access to an X11 display where Chocolate Doom renders. Run
under Xvfb (as the deployment script does) or point `DOOM_DISPLAY` at a local
X server. For macOS/WSL users, lean on a VM or remote droplet until we land a
pure-software renderer in-tree.

## Roadmap

- [ ] Harden session lifecycle and idle cleanup.
- [ ] Gate input endpoints with crude rate limiting.
- [ ] Optional nginx front-end for TLS + static asset serving.
- [ ] Observability: basic logging, watchdog scripts, and crash dumps.

Questions? File an issue or drop into the TODO list in `docs/`.
