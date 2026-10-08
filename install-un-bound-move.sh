#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
HOST=${1:-root@10.11.99.1}
SRC="$HERE/un-bound-move"
DST=/home/root/xovi/exthome/appload/un-bound
ENGINE=/home/root/chromium-engine/chromium/chrome-linux/chrome
KEY=${RM_SSH_KEY:-$HOME/.ssh/id_ed25519_remarkable}

SSH_OPTIONS=(-o IdentitiesOnly=yes)
if [ -f "$KEY" ]; then
    SSH_OPTIONS+=(-i "$KEY")
fi

if ! ssh "${SSH_OPTIONS[@]}" "$HOST" "test -d /home/root/xovi/exthome/appload"; then
    echo "AppLoad was not found on $HOST. Install or repair xovi + AppLoad first." >&2
    exit 1
fi

if ! ssh "${SSH_OPTIONS[@]}" "$HOST" "test -x '$ENGINE'"; then
    "$HERE/un-bound-engine/install-engine.sh" "$HOST"
fi

ssh "${SSH_OPTIONS[@]}" "$HOST" "systemctl stop un-bound-app 2>/dev/null || true; mkdir -p '$DST'"
scp -O -r "${SSH_OPTIONS[@]}" "$SRC"/* "$HOST:$DST/"
ssh "${SSH_OPTIONS[@]}" "$HOST" "chmod +x '$DST/'*.sh '$DST/chromium-cdp' '$DST/chromium_viewer'"

echo "UN-BOUND is installed. On the tablet, open AppLoad, tap Reload, then tap UN-BOUND."
