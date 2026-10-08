#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
SDK=${RM_SDK:-$HOME/rm-sdk-3.27}
ENV=$(ls "$SDK"/environment-setup-* 2>/dev/null | head -n 1 || true)

if [ -z "$ENV" ]; then
    echo "Missing reMarkable SDK under $SDK" >&2
    exit 1
fi
if [ ! -f "$HERE/libqsgepaper.so" ]; then
    echo "Missing libqsgepaper.so copied from the tablet" >&2
    exit 1
fi

unset LD_LIBRARY_PATH
source "$ENV"
QTINC="$SDKTARGETSYSROOT/usr/include"

$CXX -O2 -DNDEBUG -DQT_NO_DEBUG \
    -I "$QTINC" -I "$QTINC/QtCore" -I "$QTINC/QtGui" -I "$HERE" \
    "$HERE/chromium_viewer.cpp" \
    -L "$HERE/../un-bound-move" -lquill \
    -L "$HERE" -lqsgepaper \
    -lQt6Gui -lQt6Core -lstdc++ \
    -Wl,-rpath,'$ORIGIN' \
    -o "$HERE/../un-bound-move/chromium_viewer"

echo "Built un-bound-move/chromium_viewer"
