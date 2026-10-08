#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
HOST=${1:-root@10.11.99.1}
BASE=${CHROMIUM_HOME:-/home/root/chromium-engine}
BUNDLE="$HERE/bundle"
KEY=${RM_SSH_KEY:-$HOME/.ssh/id_ed25519_remarkable}

SSH_OPTIONS=(-o IdentitiesOnly=yes)
if [ -f "$KEY" ]; then
    SSH_OPTIONS+=(-i "$KEY")
fi

if [ ! -f "$BUNDLE/chromium-linux-arm64.zip" ] || [ ! -d "$BUNDLE/lib" ]; then
    echo "Preparing the Chromium ARM64 engine..."
    python3 "$HERE/prepare-engine.py"
fi

echo "Creating $BASE on $HOST"
ssh "${SSH_OPTIONS[@]}" "$HOST" "mkdir -p '$BASE/chromium' '$BASE/lib' '$BASE/profile' '$BASE/out'"

echo "Copying Chromium (about 170 MB compressed)"
scp -O "${SSH_OPTIONS[@]}" "$BUNDLE/chromium-linux-arm64.zip" "$HOST:$BASE/chromium/"
ssh "${SSH_OPTIONS[@]}" "$HOST" "cd '$BASE/chromium' && rm -rf chrome-linux && unzip -q chromium-linux-arm64.zip"

echo "Copying ARM64 shared libraries"
scp -O -r "${SSH_OPTIONS[@]}" "$BUNDLE/lib" "$HOST:$BASE/"
scp -O "${SSH_OPTIONS[@]}" "$HERE/run-chromium.sh" "$HOST:$BASE/run-chromium.sh"
ssh "${SSH_OPTIONS[@]}" "$HOST" "chmod +x '$BASE/run-chromium.sh'"

echo "Checking the installed engine"
ssh "${SSH_OPTIONS[@]}" "$HOST" "LD_LIBRARY_PATH='$BASE/lib:$BASE/lib/nss' '$BASE/chromium/chrome-linux/chrome' --version"
