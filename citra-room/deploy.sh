#!/usr/bin/env bash
# One-shot installer for the Azahar citra_room server on a Debian VPS.
#
# Run as root from the directory that contains these deploy files:
#   sudo ./deploy.sh
#
# It will:
#   1. install the deploy files into /opt/citra-room
#   2. create .env from .env.example on first run
#   3. verify the azahar-room:latest image exists (build/load it otherwise)
#   4. open ONLY the room's UDP port in the firewall (ufw if present)
#   5. install + enable the citra-room systemd service
set -euo pipefail

INSTALL_DIR="/opt/citra-room"
SERVICE="citra-room.service"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: run as root (sudo ./deploy.sh)" >&2
    exit 1
fi

if ! command -v docker >/dev/null 2>&1; then
    echo "ERROR: docker is not installed." >&2
    echo "Install it first, e.g.:  curl -fsSL https://get.docker.com | sh" >&2
    exit 1
fi

# --- 1. Install files -------------------------------------------------------
echo "==> Installing deploy files into $INSTALL_DIR"
mkdir -p "$INSTALL_DIR"
for f in docker-compose.yml entrypoint.sh .env.example "$SERVICE"; do
    cp -f "$SRC_DIR/$f" "$INSTALL_DIR/$f"
done
chmod +x "$INSTALL_DIR/entrypoint.sh"

# --- 2. .env ----------------------------------------------------------------
if [ ! -f "$INSTALL_DIR/.env" ]; then
    cp "$INSTALL_DIR/.env.example" "$INSTALL_DIR/.env"
    echo "==> Created $INSTALL_DIR/.env from the example."
    echo "    Edit it (ROOM_NAME, PREFERRED_APP, ROOM_PORT, ...) then re-run,"
    echo "    or continue now with the defaults."
fi

# Read ROOM_PORT from .env (default 24872).
ROOM_PORT="$(grep -E '^ROOM_PORT=' "$INSTALL_DIR/.env" | tail -n1 | cut -d= -f2 | tr -d '[:space:]')"
ROOM_PORT="${ROOM_PORT:-24872}"
echo "==> Room UDP port: $ROOM_PORT"

# --- 3. Image ---------------------------------------------------------------
if ! docker image inspect azahar-room:latest >/dev/null 2>&1; then
    echo "WARNING: image azahar-room:latest not found locally." >&2
    if [ -f "$SRC_DIR/azahar-room.tar.gz" ]; then
        echo "==> Loading image from azahar-room.tar.gz"
        gunzip -c "$SRC_DIR/azahar-room.tar.gz" | docker load
    elif [ -d "$SRC_DIR/../azahar" ]; then
        echo "==> Building image from ../azahar (this can take a while)"
        "$SRC_DIR/build-image.sh" "$SRC_DIR/../azahar"
    else
        echo "ERROR: no image, no azahar-room.tar.gz, and no ../azahar source." >&2
        echo "Provide one of them, then re-run. See README.md." >&2
        exit 1
    fi
fi

# --- 4. Firewall (open only the room UDP port) ------------------------------
if command -v ufw >/dev/null 2>&1; then
    echo "==> Configuring ufw: allow ${ROOM_PORT}/udp"
    ufw allow "${ROOM_PORT}/udp" || true
    ufw --force enable || true
    ufw status verbose || true
else
    echo "NOTE: ufw not found. Open UDP $ROOM_PORT yourself, e.g. with nftables:"
    echo "      nft add rule inet filter input udp dport $ROOM_PORT accept"
fi

# --- 5. systemd service -----------------------------------------------------
echo "==> Installing systemd service"
cp -f "$INSTALL_DIR/$SERVICE" "/etc/systemd/system/$SERVICE"
systemctl daemon-reload
systemctl enable --now "$SERVICE"

echo
echo "==> Done. Status:"
systemctl --no-pager --full status "$SERVICE" || true
echo
echo "Logs:   docker logs -f citra-room   (or: journalctl -u $SERVICE)"
echo "Config: edit $INSTALL_DIR/.env then: systemctl restart $SERVICE"
