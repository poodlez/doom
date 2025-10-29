#!/usr/bin/env bash
#
# Bootstrap a fresh Ubuntu 24.04 droplet so it can host doom_server.

set -euo pipefail

if [[ "$(id -u)" -ne 0 ]]; then
  echo "This script must run as root (sudo -s ...)" >&2
  exit 1
fi

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL_PREFIX="/opt/doom"
SESSION_DIR="/var/lib/doom_sessions"
SERVICE_NAME="doom"
WAD_PATH="${INSTALL_PREFIX}/freedoom1.wad"
WAD_URL="https://github.com/freedoom/freedoom/releases/download/v0.12.1/freedoom1.wad"

echo "==> Installing packages"
apt-get update -y
DEBIAN_FRONTEND=noninteractive apt-get upgrade -y
apt-get install -y \
  build-essential \
  git \
  libjpeg-turbo8-dev \
  libturbojpeg0-dev \
  libx11-dev \
  chocolate-doom \
  xvfb \
  pkg-config \
  rsync \
  wget \
  curl \
  ufw

echo "==> Preparing filesystem layout"
install -d -m 0755 "${INSTALL_PREFIX}"
install -d -m 0777 "${SESSION_DIR}"
rsync -a --delete "${REPO_DIR}/public/" "${INSTALL_PREFIX}/public/"

if [[ ! -f "${WAD_PATH}" ]]; then
  echo "==> Fetching FreeDoom WAD"
  wget -O "${WAD_PATH}" "${WAD_URL}"
  chmod 0644 "${WAD_PATH}"
fi

echo "==> Building doom_server"
make -C "${REPO_DIR}"
install -m 0755 "${REPO_DIR}/doom_server" /usr/local/bin/doom_server

echo "==> Installing systemd unit"
install -m 0755 "${REPO_DIR}/scripts/doomctl" /usr/local/bin/doomctl || true
install -m 0644 "${REPO_DIR}/systemd/doom.service" /etc/systemd/system/doom.service
install -m 0644 "${REPO_DIR}/systemd/doom.env" /etc/default/doom

systemctl daemon-reload
systemctl enable --now "${SERVICE_NAME}.service"

echo "==> Configuring firewall"
ufw allow 80/tcp || true
ufw allow 443/tcp || true
ufw --force enable

systemctl status "${SERVICE_NAME}.service" --no-pager
echo "✅ Deployment complete. Point DNS to this droplet and visit http://<ip>/"
