#!/usr/bin/env bash
# Build the azahar-room:latest Docker image from the azahar source tree.
#
# Usage:
#   ./build-image.sh [--jobs N] [path-to-azahar-source]
#
#   --jobs N   Limit ninja to N parallel compile jobs (uses Dockerfile.lowmem).
#              Use on machines with limited RAM: each C++ job can need ~1-2 GB,
#              so pick roughly RAM_GB / 2. Without it, ninja uses ALL cores,
#              which on a low-RAM box triggers the OOM killer.
#
# If the source path is omitted and no ./azahar exists, the repo is cloned
# automatically (shallow). Requirements on this machine: Docker, git, and
# ideally >=8 GB RAM and ~15 GB free disk for a comfortable build.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JOBS=""
SRC=""

while [ $# -gt 0 ]; do
    case "$1" in
        --jobs|-j) JOBS="${2:?--jobs needs a number}"; shift 2 ;;
        --jobs=*)  JOBS="${1#*=}"; shift ;;
        -*)        echo "Unknown option: $1" >&2; exit 1 ;;
        *)         SRC="$1"; shift ;;
    esac
done
SRC="${SRC:-$SCRIPT_DIR/azahar}"

# Clone the source if it is not already present.
if [ ! -f "$SRC/docker/azahar-room/Dockerfile" ]; then
    if [ -e "$SRC" ]; then
        echo "ERROR: '$SRC' exists but has no docker/azahar-room/Dockerfile." >&2
        exit 1
    fi
    echo "==> Cloning azahar into $SRC (shallow)"
    git clone --depth 1 https://github.com/azahar-emu/azahar.git "$SRC"
fi

if [ -n "$JOBS" ]; then
    echo "==> Building azahar-room:latest from $SRC (ninja -j $JOBS, low-mem)"
    docker build -f "$SCRIPT_DIR/Dockerfile.lowmem" \
                 --build-arg NINJA_JOBS="$JOBS" \
                 -t azahar-room:latest "$SRC"
else
    echo "==> Building azahar-room:latest from $SRC (full parallelism)"
    docker build -f "$SRC/docker/azahar-room/Dockerfile" \
                 -t azahar-room:latest "$SRC"
fi

echo
echo "Done. Image: azahar-room:latest"
docker image inspect azahar-room:latest --format '  size: {{.Size}} bytes'
echo "Next: ./push-to-vps.sh user@vps-host   (ships it to the VPS and starts it)"
