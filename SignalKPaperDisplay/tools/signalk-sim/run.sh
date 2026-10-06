#!/usr/bin/env bash
# A throwaway SignalK test rig: a SignalK server on port 3001, fed with made-up boat data by sim.js.
# Point the dashboard (or OpenCPN, KIP, anything) at <this machine>:3001.
#
#   ./run.sh up        create (if needed) and start the server and the simulator
#   ./run.sh down      stop and remove both containers (the server's data volume is kept)
#   ./run.sh reset     restart the server, which forgets every value it had cached, then the
#                      simulator, which starts from its beginning (see the README for why)
#   ./run.sh gps-off   stop sending the boat's own GPS position (everything else carries on)
#   ./run.sh gps-on    start sending it again
#   ./run.sh status    what is running, and a sample of what the server holds
#
# Needs Docker, and a Linux shell (on Windows, run it from WSL). PORT=3002 ./run.sh up uses another port.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
IMAGE="${IMAGE:-signalk/signalk-server}"
NET=signalk-net
SERVER=signalk-kindle
SIM=signalk-sim
PORT="${PORT:-3001}"

up() {
  docker network inspect "$NET" >/dev/null 2>&1 || docker network create "$NET" >/dev/null
  # --no-securityenabled: a fresh server of this version refuses anonymous access, which is not wanted for a test rig
  docker inspect "$SERVER" >/dev/null 2>&1 || docker run -d --name "$SERVER" --network "$NET" -p "$PORT:3000" \
    -v "${SERVER}-data:/home/node/.signalk" --restart unless-stopped "$IMAGE" --no-securityenabled >/dev/null
  # The image already has the 'ws' module on NODE_PATH, so the script needs nothing installed.
  docker inspect "$SIM" >/dev/null 2>&1 || docker run -d --name "$SIM" --network "$NET" -v "$DIR:/sim:ro" \
    -e SK_URL="ws://${SERVER}:3000/signalk/v1/stream?subscribe=none" --restart unless-stopped \
    --entrypoint node "$IMAGE" /sim/sim.js >/dev/null
  docker start "$SERVER" "$SIM" >/dev/null
  echo "up: SignalK on port $PORT (admin: http://localhost:$PORT/admin), fed by $SIM"
}

wait_server() {
  for _ in $(seq 1 40); do curl -s -m 2 "http://127.0.0.1:$PORT/signalk" >/dev/null && return 0; sleep 2; done
  echo "the server did not come up" >&2; return 1
}

case "${1:-}" in
  up) up ;;
  down) docker rm -f "$SIM" "$SERVER" >/dev/null 2>&1 || true; echo "removed (the volume ${SERVER}-data is kept)" ;;
  reset) docker restart "$SERVER" >/dev/null; wait_server; docker restart "$SIM" >/dev/null; echo "server cache cleared, simulator restarted" ;;
  gps-off) : > "$DIR/nogps"; echo "own GPS position is no longer sent" ;;
  gps-on) rm -f "$DIR/nogps"; echo "own GPS position is being sent again" ;;
  status)
    docker ps --filter "name=$SERVER" --filter "name=$SIM" --format '{{.Names}}  {{.Status}}  {{.Ports}}'
    [ -e "$DIR/nogps" ] && echo "GPS: OFF (flag file present)" || echo "GPS: on"
    curl -s -m 4 "http://127.0.0.1:$PORT/signalk/v1/api/vessels/self/navigation/position" | head -c 300; echo ;;
  *) sed -n 2,13p "$0"; exit 1 ;;
esac
