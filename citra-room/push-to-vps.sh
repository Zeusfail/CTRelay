#!/usr/bin/env bash
# Ship the locally-built azahar-room image + deploy files to the VPS and
# start the service. Run this ON THE PC THAT HAS BOTH the built image
# (see build-image.sh) AND the VPS SSH key, from inside this folder.
#
# Usage:
#   ./push-to-vps.sh user@vps-host [ssh-port]
#
# It streams the image over SSH (docker save | ssh docker load) - no large
# intermediate file - then runs deploy.sh on the VPS (firewall + systemd).
set -euo pipefail

DEST="${1:?Usage: ./push-to-vps.sh user@vps-host [ssh-port]}"
PORT="${2:-22}"
REMOTE_DIR="citra-room-deploy"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SSH="ssh -p $PORT"
SCP="scp -P $PORT"
# Resolve sudo on the remote only if we are not already root there.
REMOTE_SUDO='S=; [ "$(id -u)" -ne 0 ] && S=sudo;'

echo "==> Ensuring remote dir ~/$REMOTE_DIR on $DEST"
$SSH "$DEST" "mkdir -p ~/$REMOTE_DIR"

echo "==> Copying deploy files"
$SCP "$HERE"/docker-compose.yml "$HERE"/entrypoint.sh "$HERE"/.env.example \
     "$HERE"/citra-room.service "$HERE"/deploy.sh "$HERE"/build-image.sh \
     "$HERE"/Dockerfile.lowmem "$HERE"/README.md \
     "$DEST:~/$REMOTE_DIR/"

# Transfer the image itself.
if docker image inspect azahar-room:latest >/dev/null 2>&1; then
    echo "==> Streaming azahar-room:latest to the VPS (this sends ~150-250 MB)"
    docker save azahar-room:latest | gzip \
        | $SSH "$DEST" "$REMOTE_SUDO gunzip | \$S docker load"
elif [ -f "$HERE/azahar-room.tar.gz" ]; then
    echo "==> Copying azahar-room.tar.gz and loading it on the VPS"
    $SCP "$HERE/azahar-room.tar.gz" "$DEST:~/$REMOTE_DIR/"
    $SSH "$DEST" "$REMOTE_SUDO gunzip -c ~/$REMOTE_DIR/azahar-room.tar.gz | \$S docker load"
else
    echo "ERROR: no local azahar-room:latest image and no azahar-room.tar.gz." >&2
    echo "Build it first:  ./build-image.sh   (see README.md)" >&2
    exit 1
fi

echo "==> Running deploy.sh on the VPS (firewall + systemd service)"
$SSH -t "$DEST" "cd ~/$REMOTE_DIR && chmod +x *.sh && $REMOTE_SUDO \$S ./deploy.sh"

echo
echo "==> Done. Check it with:  $SSH $DEST 'sudo docker logs -f citra-room'"
