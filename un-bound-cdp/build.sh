#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
GO=${GO:-go}

cd "$HERE"
CGO_ENABLED=0 GOOS=linux GOARCH=arm64 "$GO" build \
    -trimpath -ldflags="-s -w" \
    -o "$HERE/../un-bound-move/chromium-cdp" .

echo "Built un-bound-move/chromium-cdp"
