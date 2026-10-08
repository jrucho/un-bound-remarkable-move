#!/bin/sh
# Chromium takeover: stop xochitl, drive the on-device headless Chromium, and
# show the page through quill's color path.
#
# Interactive (CDP) mode — used when the chromium-cdp driver ships in this
# bundle: Chromium runs persistently with the DevTools protocol; the driver
# waits for real content (no more SPA spinners), streams screenshots into
# out/current.png, and replays the viewer's taps and swipes as clicks and
# scrolls. ChatGPT works here once you've pushed a logged-in profile with
# chromium-bootstrap/login-on-host.sh.
#
# Fallback (one-shot) mode — no driver binary: render once with --screenshot
# and show the result.
#
# Exit: power button or 5 fingers at once. Two fingers up opens the keyboard;
# two fingers down performs a cleaning refresh. Corner taps have no app action.
# Escape hatch: ssh rm 'systemctl start xochitl'.

HERE=$(cd "$(dirname "$0")" && pwd)

# Settings written by remagic config / the Settings app.
if [ -f "$HERE/settings.env" ]; then
    set -a; . "$HERE/settings.env"; set +a
fi
URL=${CHROMIUM_URL:-https://un-bound.ai.studio}
CHROMIUM_HOME=${CHROMIUM_HOME:-/home/root/chromium-engine}
RENDER_TIMEOUT=${CHROMIUM_RENDER_TIMEOUT:-120}
# Settings fields are free text; a non-numeric value must not wedge the loop.
case "$RENDER_TIMEOUT" in ''|*[!0-9]*) RENDER_TIMEOUT=120;; esac
PORT=${CHROMIUM_CDP_PORT:-9222}
REFRESH=${CHROMIUM_REFRESH:-1s}
# A clean UA (headless announces itself otherwise) — must match the UA used
# by login-on-host.sh so Cloudflare clearance cookies transfer.
CHROMIUM_UA=${CHROMIUM_UA:-"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36"}

# Selector that marks "the page really rendered". ChatGPT's composer (logged
# in) or its login UI (logged out); a sane default elsewhere.
if [ -z "${CHROMIUM_WAIT_SELECTOR:-}" ]; then
    case "$URL" in
        *un-bound.ai.studio*) CHROMIUM_WAIT_SELECTOR='#root > *' ;;
        *chatgpt.com*|*chat.openai.com*) CHROMIUM_WAIT_SELECTOR='#prompt-textarea, [data-testid^="login"], main' ;;
        *) CHROMIUM_WAIT_SELECTOR='main, body' ;;
    esac
fi

CURRENT="$CHROMIUM_HOME/out/current.png"
FRESH="$CHROMIUM_HOME/out/fresh.png"
EVENTS="$CHROMIUM_HOME/out/events"
CHROME="$CHROMIUM_HOME/chromium/chrome-linux/chrome"

VIEWER_PID=
CHROME_PID=
DRIVER_PID=

kill_children() {
    # TERM, give them a moment, then make sure. wait so the EPD is actually
    # released before anyone else (xochitl) grabs the panel.
    [ -n "$DRIVER_PID" ] && kill "$DRIVER_PID" 2>/dev/null
    [ -n "$CHROME_PID" ] && kill "$CHROME_PID" 2>/dev/null
    [ -n "$VIEWER_PID" ] && kill "$VIEWER_PID" 2>/dev/null
    [ -n "$VIEWER_PID" ] && wait "$VIEWER_PID" 2>/dev/null
    [ -n "$DRIVER_PID" ] && wait "$DRIVER_PID" 2>/dev/null
    if [ -n "$CHROME_PID" ]; then
        sleep 1
        kill -9 "$CHROME_PID" 2>/dev/null
        wait "$CHROME_PID" 2>/dev/null
    fi
    # Renderer children can outlive the main chrome process on SIGKILL.
    pgrep -f "user-data-dir=$CHROMIUM_HOME/profile " 2>/dev/null | xargs -r kill -9 2>/dev/null
    VIEWER_PID=; CHROME_PID=; DRIVER_PID=
}

restore() {
    kill_children
    rm -f "$EVENTS"
    if [ -z "${REMAGIC_SESSION:-}" ]; then
        rm -f /tmp/epframebuffer.lock
        systemctl start xochitl
    fi
}
# BusyBox ash resumes the script after a trapped signal, so the INT/TERM
# handler must exit itself or cleanup runs twice and the script marches on
# over a restarted xochitl.
trap restore EXIT
trap 'restore; trap - EXIT; exit 143' INT TERM
if [ -z "${REMAGIC_SESSION:-}" ]; then
    systemctl stop xochitl
    rm -f /tmp/epframebuffer.lock      # stale EPD lock blocks the engine
    sleep 1
fi

# libquill.so ships in this bundle; libqsgepaper.so comes from the device's
# own scenegraph plugin dir. Extra args (a FIFO path) enable gesture mode.
show() {
    if [ -n "$VIEWER_PID" ]; then
        # The user may have just closed the old viewer to leave — don't
        # resurrect the app on top of their exit gesture.
        if ! kill -0 "$VIEWER_PID" 2>/dev/null; then
            VIEWER_PID=
            return 1
        fi
        kill "$VIEWER_PID" 2>/dev/null
        wait "$VIEWER_PID" 2>/dev/null
    fi
    LD_LIBRARY_PATH="$HERE:/usr/lib/plugins/scenegraph" \
    CHROMIUM_TOUCH_FLIP="${CHROMIUM_TOUCH_FLIP:-}" \
        "$HERE/chromium_viewer" "$@" &
    VIEWER_PID=$!
}

# First display of the session: if the vendor engine loses the EPD-handoff
# race with a still-dying predecessor ("Failed to initialize SWTCON"), the
# viewer aborts within a second — give it one settled retry.
first_show() {
    show "$@"
    sleep 2
    if ! kill -0 "$VIEWER_PID" 2>/dev/null; then
        echo "chromium: viewer died at startup (EPD handoff race?), retrying once" >&2
        VIEWER_PID=
        sleep 2
        show "$@"
    fi
}

fail_screen() {
    # Publish the error screen through the watched file so whichever viewer
    # is up displays it.
    cp -f "$HERE/error.png" "$CURRENT" 2>/dev/null
}

if [ ! -x "$CHROME" ]; then
    echo "chromium: engine missing at $CHROMIUM_HOME — run install-to-tablet.sh" >&2
    first_show "$HERE/error.png" stretch 1
    wait "$VIEWER_PID"
    VIEWER_PID=
    exit 1
fi

mkdir -p "$CHROMIUM_HOME/out"

# ---------------- interactive CDP mode ----------------
if [ -x "$HERE/chromium-cdp" ]; then
    # Leftover browsers on this profile hold the SingletonLock; clear them.
    pgrep -f "user-data-dir=$CHROMIUM_HOME/profile " 2>/dev/null | xargs -r kill 2>/dev/null
    sleep 1
    pgrep -f "user-data-dir=$CHROMIUM_HOME/profile " 2>/dev/null | xargs -r kill -9 2>/dev/null

    rm -f "$EVENTS"
    mkfifo "$EVENTS"

    # Show the last page (or the loading screen) instantly; the driver will
    # rename fresh frames over $CURRENT and the viewer repaints on change.
    [ -f "$CURRENT" ] || cp -f "$HERE/loading.png" "$CURRENT"
    first_show "$CURRENT" stretch 1 "$EVENTS"

    LD_LIBRARY_PATH="$CHROMIUM_HOME/lib:$CHROMIUM_HOME/lib/nss" \
        "$CHROME" \
        --headless=new --no-sandbox --disable-gpu --disable-dev-shm-usage \
        --disable-crash-reporter --disable-breakpad \
        --renderer-process-limit=2 --disk-cache-size=104857600 --media-cache-size=0 \
        --user-data-dir="$CHROMIUM_HOME/profile" \
        --password-store=basic \
        --window-size=960,1696 --hide-scrollbars --disable-popup-blocking \
        --user-agent="$CHROMIUM_UA" \
        --disable-blink-features=AutomationControlled \
        --remote-debugging-port="$PORT" \
        about:blank >"$CHROMIUM_HOME/out/chrome.log" 2>&1 &
    CHROME_PID=$!

    "$HERE/chromium-cdp" -port "$PORT" -url "$URL" -out "$CURRENT" \
        -fifo "$EVENTS" -selector "$CHROMIUM_WAIT_SELECTOR" \
        -wait "$RENDER_TIMEOUT" -refresh "$REFRESH" \
        -vw 480 -vh 848 -dpr 2 \
        >"$CHROMIUM_HOME/out/cdp.log" 2>&1 &
    DRIVER_PID=$!

    # Session runs until the user closes the viewer. If the browser/driver
    # die early, put the error screen up instead of a stale page.
    while kill -0 "$VIEWER_PID" 2>/dev/null; do
        if [ -n "$DRIVER_PID" ] && ! kill -0 "$DRIVER_PID" 2>/dev/null; then
            wait "$DRIVER_PID" 2>/dev/null
            DRIVER_RC=$?
            DRIVER_PID=
            # In session mode the driver only exits if the browser died or
            # it crashed — either way the page is frozen: show the error.
            echo "chromium: driver exited rc=$DRIVER_RC (see out/cdp.log)" >&2
            fail_screen
        fi
        sleep 1
    done
    VIEWER_PID=
    echo "chromium: closed, restoring xochitl"
    exit 0
fi

# ---------------- one-shot fallback mode ----------------
# Show something immediately: the previous page if we have one.
if [ -f "$CURRENT" ]; then
    first_show "$CURRENT" stretch 1
else
    first_show "$HERE/loading.png" stretch 1
fi

# Re-render in the background while the user looks at the instant image.
rm -f "$FRESH"
CHROMIUM_HOME="$CHROMIUM_HOME" "$CHROMIUM_HOME/run-chromium.sh" "$URL" "$FRESH" \
    >"$CHROMIUM_HOME/out/cdp.log" 2>&1 &
CHROME_PID=$!

elapsed=0
while kill -0 "$CHROME_PID" 2>/dev/null; do
    # User closed the viewer (corner tap / power / 5-finger) mid-render: leave.
    kill -0 "$VIEWER_PID" 2>/dev/null || exit 0
    if [ "$elapsed" -ge "$RENDER_TIMEOUT" ]; then
        echo "chromium: render timed out after ${RENDER_TIMEOUT}s" >&2
        kill "$CHROME_PID" 2>/dev/null
        sleep 2
        kill -9 "$CHROME_PID" 2>/dev/null    # a wedged chrome must not strand the wait below
        break
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done
wait "$CHROME_PID" 2>/dev/null
CHROME_RC=$?
CHROME_PID=

# Chrome writes the PNG in place (no tmp+rename), so a kill mid-write leaves
# a truncated file. Only promote a complete PNG (IEND trailer present).
if [ -s "$FRESH" ] && tail -c 12 "$FRESH" | grep -q IEND; then
    mv -f "$FRESH" "$CURRENT"
    show "$CURRENT" stretch 1 || exit 0
else
    rm -f "$FRESH"
    echo "chromium: render failed (chrome rc=$CHROME_RC), keeping previous page (see out/cdp.log)" >&2
    if [ ! -f "$CURRENT" ]; then
        show "$HERE/error.png" stretch 1 || exit 0
    fi
fi

wait "$VIEWER_PID"
VIEWER_PID=
echo "chromium: closed, restoring xochitl"
