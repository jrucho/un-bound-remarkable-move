#!/bin/sh
# Open UN-BOUND, tap its Google button, and verify the OAuth page target appears.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
CHROMIUM_HOME=${CHROMIUM_HOME:-/home/root/chromium-engine}
URL=${CHROMIUM_URL:-https://un-bound.ai.studio}
PORT=${CHROMIUM_POPUP_SMOKE_PORT:-9232}
OUT=${1:-$CHROMIUM_HOME/out/un-bound-popup-smoke.jpg}
EVENTS="$CHROMIUM_HOME/out/popup-smoke-events"
LOG="$CHROMIUM_HOME/out/popup-smoke-cdp.log"
CHROME="$CHROMIUM_HOME/chromium/chrome-linux/chrome"
CHROME_PID=
DRIVER_PID=

cleanup() {
    [ -n "$DRIVER_PID" ] && kill "$DRIVER_PID" 2>/dev/null || true
    [ -n "$DRIVER_PID" ] && wait "$DRIVER_PID" 2>/dev/null || true
    [ -n "$CHROME_PID" ] && kill "$CHROME_PID" 2>/dev/null || true
    [ -n "$CHROME_PID" ] && wait "$CHROME_PID" 2>/dev/null || true
    rm -f "$EVENTS"
}
trap cleanup EXIT INT TERM

mkdir -p "$CHROMIUM_HOME/profile-popup-smoke" "$CHROMIUM_HOME/out"
rm -f "$OUT" "$EVENTS" "$LOG"
mkfifo "$EVENTS"

LD_LIBRARY_PATH="$CHROMIUM_HOME/lib:$CHROMIUM_HOME/lib/nss" \
    "$CHROME" \
    --headless=new --no-sandbox --disable-gpu --disable-dev-shm-usage \
    --disable-crash-reporter --disable-breakpad --renderer-process-limit=2 \
    --user-data-dir="$CHROMIUM_HOME/profile-popup-smoke" --password-store=basic \
    --window-size=960,1696 --hide-scrollbars --disable-popup-blocking \
    --remote-debugging-port="$PORT" about:blank \
    >"$CHROMIUM_HOME/out/popup-smoke-chrome.log" 2>&1 &
CHROME_PID=$!

"$HERE/chromium-cdp" -port "$PORT" -url "$URL" -out "$OUT" \
    -fifo "$EVENTS" -selector '#root > *' -wait 120 -refresh 1s \
    -vw 480 -vh 848 -dpr 2 >"$LOG" 2>&1 &
DRIVER_PID=$!

tries=0
while [ ! -s "$OUT" ] && [ "$tries" -lt 120 ]; do
    kill -0 "$DRIVER_PID" 2>/dev/null || { echo "CDP driver exited before rendering" >&2; exit 1; }
    tries=$((tries + 1))
    sleep 1
done
[ -s "$OUT" ] || { echo "UN-BOUND did not render" >&2; exit 1; }

# Center of the large mobile-layout Google button on the 960x1696 Move panel.
printf 'TAP 480 1050\n' >"$EVENTS"
sleep 15

if ! grep -q 'target .* -> ' "$LOG"; then
    echo "Google OAuth popup target did not appear" >&2
    exit 1
fi
cp "$OUT" "$OUT.oauth"
POPUP_ID=$(awk '/target .* -> / { id=$NF } END { print id }' "$LOG")
wget -qO- "http://127.0.0.1:$PORT/json/close/$POPUP_ID" >/dev/null
sleep 5
TRANSITIONS=$(grep -c 'target .* -> ' "$LOG")
if [ "$TRANSITIONS" -lt 2 ]; then
    echo "Driver did not return to UN-BOUND after the OAuth popup closed" >&2
    exit 1
fi
echo "$OUT.oauth"
