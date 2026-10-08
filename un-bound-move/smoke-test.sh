#!/bin/sh
# Render through the same CDP path as the app without taking over the display.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
CHROMIUM_HOME=${CHROMIUM_HOME:-/home/root/chromium-engine}
URL=${CHROMIUM_URL:-https://un-bound.ai.studio}
PORT=${CHROMIUM_SMOKE_PORT:-9231}
OUT=${1:-$CHROMIUM_HOME/out/un-bound-cdp-smoke.jpg}
CHROME="$CHROMIUM_HOME/chromium/chrome-linux/chrome"
CHROME_PID=

cleanup() {
    [ -n "$CHROME_PID" ] && kill "$CHROME_PID" 2>/dev/null || true
    [ -n "$CHROME_PID" ] && wait "$CHROME_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

mkdir -p "$CHROMIUM_HOME/profile-smoke" "$CHROMIUM_HOME/out"
LD_LIBRARY_PATH="$CHROMIUM_HOME/lib:$CHROMIUM_HOME/lib/nss" \
    "$CHROME" \
    --headless=new --no-sandbox --disable-gpu --disable-dev-shm-usage \
    --disable-crash-reporter --disable-breakpad --renderer-process-limit=2 \
    --user-data-dir="$CHROMIUM_HOME/profile-smoke" --password-store=basic \
    --window-size=1620,2160 --hide-scrollbars \
    --remote-debugging-port="$PORT" about:blank \
    >"$CHROMIUM_HOME/out/smoke-chrome.log" 2>&1 &
CHROME_PID=$!

"$HERE/chromium-cdp" -port "$PORT" -url "$URL" -out "$OUT" \
    -selector '#root > *' -wait 120 -vw 480 -vh 848 -dpr 2 -oneshot -quit
wait "$CHROME_PID" 2>/dev/null || true
CHROME_PID=
echo "$OUT"
