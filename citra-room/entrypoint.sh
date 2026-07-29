#!/bin/sh
# Entrypoint for the azahar-room (citra_room) dedicated server.
# Builds the argument list from environment variables, skipping empty
# optional flags so the binary never receives blank values.
#
# The room binary requires BOTH --room-name and --preferred-app to be
# non-empty, otherwise it prints help and exits with an error.
set -u

ROOM_NAME="${ROOM_NAME:-Azahar Room}"
PREFERRED_APP="${PREFERRED_APP:-Homebrew}"
ROOM_PORT="${ROOM_PORT:-24872}"
MAX_MEMBERS="${MAX_MEMBERS:-16}"
LOG_FILE="${LOG_FILE:-/data/citra-room.log}"
BAN_LIST_FILE="${BAN_LIST_FILE:-/data/ban-list.txt}"

# Mandatory + always-set flags.
set -- \
    --room-name     "$ROOM_NAME" \
    --preferred-app "$PREFERRED_APP" \
    --port          "$ROOM_PORT" \
    --max_members   "$MAX_MEMBERS" \
    --log-file      "$LOG_FILE" \
    --ban-list-file "$BAN_LIST_FILE"

# Optional flags: only appended when the variable is set and non-empty.
if [ -n "${ROOM_DESCRIPTION:-}" ]; then
    set -- "$@" --room-description "$ROOM_DESCRIPTION"
fi
if [ -n "${PREFERRED_APP_ID:-}" ]; then
    set -- "$@" --preferred-app-id "$PREFERRED_APP_ID"
fi
if [ -n "${ROOM_PASSWORD:-}" ]; then
    set -- "$@" --password "$ROOM_PASSWORD"
fi

echo "Starting azahar-room on UDP port ${ROOM_PORT} (max ${MAX_MEMBERS} players)..."
# Private mode: no --token / --web-api-url => the room is NOT announced publicly.
exec azahar-room "$@"
