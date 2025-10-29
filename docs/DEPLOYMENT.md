# Deployment playbook

## DigitalOcean droplet

1. Provision a fresh Ubuntu 24.04 LTS droplet (Basic / Regular / $6 plan).
2. Attach your SSH key and boot; the rest of these steps assume passwordless login.
3. SSH in as root: `ssh root@<droplet-ip>`.

## Install doom_server

```bash
apt-get install -y git
git clone https://github.com/poodlez/doom.git /opt/doom-src
cd /opt/doom-src
./scripts/setup_droplet.sh
```

What the script does:

- Updates the OS and installs `chocolate-doom`, `libjpeg-turbo`, `xvfb`, etc.
- Copies `public/` into `/opt/doom`, downloads the FreeDoom WAD, and builds the C server.
- Installs `doom_server` into `/usr/local/bin` and drops a systemd unit that binds to port 80.
- Enables UFW and opens ports 80/443.

You can toggle features via `/etc/default/doom`:

- `DOOM_DISABLE_SPAWN=1` — run server without launching Chocolate Doom (useful for dry runs).
- `DOOM_DISPLAY=:99` — change the X11 display the capture process connects to.
- `DOOM_SERVER_PORT=8080` — move HTTP to a non-privileged port (remember to update firewall rules and systemd `AmbientCapabilities` if binding <1024 as non-root).

Restart after editing the environment file:

```bash
sudo systemctl restart doom
```

## DNS + TLS

1. Point `A` records (e.g. `doom.c`) at the droplet IP.
2. Once DNS propagates, install TLS (optional):

```bash
apt-get install -y certbot
certbot certonly --standalone -d doom.c -d www.doom.c
```

For production TLS, front the service with nginx or Caddy; terminate SSL there and proxy `127.0.0.1:8080`.

## Verification

- `curl http://<ip>/healthz` → `ok`
- Visit `http://<ip>/` to verify the MJPEG canvas loads (expect a color test pattern until Doom is wired up).
- Tail logs with `sudo doomctl logs`.

## Tear-down

```bash
systemctl disable --now doom
rm -rf /opt/doom /var/lib/doom_sessions /usr/local/bin/doom_server
```

Destroy the droplet from the DigitalOcean dashboard when finished.
