#!/bin/bash
# Flash nowtube to /dev/ttyUSB0. If the port is busy, optionally kill the process holding it.
# Usage: scripts/flash.sh [PORT] [-y]
#   -y  kill process holding the port and flash (no prompt)
set -e
PORT="/dev/ttyUSB0"
AUTO_KILL=""
for arg in "$@"; do
  if [[ "$arg" == "-y" ]]; then AUTO_KILL=1; elif [[ "$arg" != -* ]]; then PORT="$arg"; fi
done

if [[ ! -e "$PORT" ]]; then
  echo "Port $PORT does not exist. Is the device plugged in?"
  exit 1
fi

# Check if something has the port open
HOLDER_PID=""
if command -v lsof &>/dev/null; then
  HOLDER_PID=$(lsof -t "$PORT" 2>/dev/null || true)
elif command -v fuser &>/dev/null; then
  HOLDER_PID=$(fuser "$PORT" 2>/dev/null | awk '{print $1}' || true)
fi

if [[ -n "$HOLDER_PID" ]]; then
  echo "Port $PORT is in use by PID(s): $HOLDER_PID"
  echo "  Often this is a leftover 'idf.py monitor' or serial terminal."
  if [[ -n "$AUTO_KILL" ]]; then
    echo "Killing (run with -y)."
    kill $HOLDER_PID 2>/dev/null || true
    sleep 1
  else
    echo ""
    echo "To free the port and flash, run:  kill $HOLDER_PID"
    echo "Or run this script with -y to kill and flash:  $0 $PORT -y"
    exit 1
  fi
fi

# Source IDF and run flash (build first so we don't need full ninja)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"
source "$HOME/esp/esp-idf/export.sh" 2>/dev/null || true
python3 "$IDF_PATH/tools/idf.py" -p "$PORT" flash

echo ""
echo "Done. To open the serial monitor: idf.py -p $PORT monitor"
