# UN-BOUND for reMarkable Paper Pro Move

This AppLoad bundle opens `https://un-bound.ai.studio` in the Chromium e-ink
browser from <https://github.com/MaximeRivest/chromium>.

## Controls

- Tap links and buttons; swipe to scroll.
- Swipe two fingers up to open the on-screen keyboard when Google asks for an
  email, password, or confirmation; use `hide` to close it.
- Swipe two fingers down to perform a cleaning full-screen refresh.
- Use the keyboard's `url` key to enter another address and `back` to return.
- Use five fingers at once to exit and restore the reMarkable interface.
- There are no visible browser or corner decorations over the site.

The browser engine is shared at `/home/root/chromium-engine`. From the parent
directory, `./install-un-bound-move.sh` installs it when needed and then copies
this app to AppLoad.

This local bundle derives from the Chromium 0.5.0 release. The CDP driver,
viewer behavior, manifest, default settings, icon, and service name are
customized for UN-BOUND.
