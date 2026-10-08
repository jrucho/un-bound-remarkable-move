#!/bin/sh
set -eu
BASE=${CHROMIUM_HOME:-/home/root/chromium-engine}
URL=${1:-https://un-bound.ai.studio}
OUT=${2:-$BASE/out/page.png}
mkdir -p "$BASE/profile" "$BASE/out"
exec env LD_LIBRARY_PATH="$BASE/lib:$BASE/lib/nss${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$BASE/chromium/chrome-linux/chrome" \
    --headless=new \
    --no-sandbox \
    --disable-gpu \
    --disable-dev-shm-usage \
    --disable-crash-reporter \
    --disable-breakpad \
    --renderer-process-limit=2 \
    --disk-cache-size=104857600 \
    --media-cache-size=0 \
    --user-data-dir="$BASE/profile" \
    --password-store=basic \
    --window-size=1620,2160 \
    --hide-scrollbars \
    --screenshot="$OUT" \
    "$URL"
