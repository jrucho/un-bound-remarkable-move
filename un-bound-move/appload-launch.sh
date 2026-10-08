#!/bin/sh
# AppLoad entry point for takeover mode. AppLoad runs this inside xochitl's
# world, which is about to be stopped — so detach the real launch into a
# transient systemd unit (PID-1-owned, survives xochitl) and exit immediately.
HERE=$(cd "$(dirname "$0")" && pwd)
systemctl is-active --quiet un-bound-app && exit 0
# ExecStopPost is the safety net the in-script trap can't be: it runs even if
# the app is SIGKILLed or OOM-killed, so the tablet never stays UI-less.
systemd-run --unit=un-bound-app --collect \
    --property="ExecStopPost=-/bin/systemctl start xochitl" \
    /bin/sh "$HERE/chromium-takeover.sh" \
  || systemd-run --unit=un-bound-app --collect /bin/sh "$HERE/chromium-takeover.sh"
exit 0
