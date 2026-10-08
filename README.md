# UN-BOUND for reMarkable Paper Pro Move

A dedicated **AppLoad** app for <https://un-bound.ai.studio>, using on-device
Chromium and an e-ink viewer adapted from
[MaximeRivest/chromium](https://github.com/MaximeRivest/chromium).

## Features

- Mobile layout: 480 × 848 CSS pixels at DPR 2, rendering at the Move's
  native 960 × 1696 resolution.
- Google/Firebase popup login displayed on the tablet, with return to the
  original page when the popup closes.
- Persistent Chromium profile on the tablet.
- Clean fullscreen interface with no corner decorations or keyboard hotspot.
- Touch and pen input, scrolling, and an on-screen keyboard.
- Faster capture bursts (180 ms, then another 700 ms) and file-change-driven
  repainting using inotify. Background recapture defaults to once per second.

E-ink refreshes and screenshot encoding still affect responsiveness. This is
a Chromium-based e-ink browser, not an LCD-speed browser.

## Requirements

- **reMarkable Paper Pro Move**, with developer mode, SSH, xovi and AppLoad.
- A computer with Bash, Python 3, SSH/SCP, `ar`, and `tar`.
- Internet access and sufficient tablet storage for the shared browser engine
  (approximately 800 MB, plus profile and cache).

The included viewer has been used on a Move reporting Codex 5.6.75, glibc
2.39 and Qt 6.8.2. Other firmware versions need compatibility checks. This
bundle is ARM64 and is **not a reMarkable 2 build**.

## Install

Clone this repository, open its directory in a terminal, then run:

```sh
bash ./install-un-bound-move.sh root@10.11.99.1
```

The installer uses `~/.ssh/id_ed25519_remarkable` if it exists. Select another
key with:

```sh
RM_SSH_KEY="$HOME/.ssh/my-tablet-key" bash ./install-un-bound-move.sh root@10.11.99.1
```

Use the tablet's Wi-Fi SSH address instead of the USB address if needed.
The installer prepares and installs the engine if it is missing, then copies
the app to:

```text
/home/root/xovi/exthome/appload/un-bound/
```

On the tablet, choose **AppLoad → Reload → UN-BOUND**.

The engine bootstrap downloads Playwright Chromium revision 1140 and ARM64
Ubuntu Jammy dependency packages. Dependency package SHA256 checksums are
checked against the Ubuntu package indexes. On macOS, a Zstandard Python
helper is installed into a private bootstrap directory when necessary.

## Controls

| Gesture / key | Action |
| --- | --- |
| Tap | Click a link, button, or field |
| One-finger swipe | Scroll |
| Two-finger vertical swipe up | Open keyboard |
| Two-finger vertical swipe down | Cleaning full-screen refresh |
| Five fingers at once | Exit |
| Power button | Exit |
| Keyboard `hide` | Close keyboard |
| Keyboard `bksp` | Delete before cursor |
| Keyboard `del` | Delete after cursor |
| `shift`, then `2` | Type `@` |
| Keyboard `url` / `back` | Navigate / go back |

Two-finger gestures require approximately 160 physical pixels of vertical
movement. Tap an input field before opening the keyboard to focus it.

## Login and settings

Tap **Continue with Google**, then complete Google's sign-in flow. The driver
shows the popup page in place of the main page. Authentication credentials
are entered on the tablet; none are included in this repository.

The shared profile is stored in `/home/root/chromium-engine/profile`.
Google may show the OAuth application name **UN-RELEASED**, as configured by
the website. Provider policy and site changes can affect authentication.

Defaults are in `un-bound-move/settings.env`; the AppLoad settings schema is
`un-bound-move/settings.schema.json`. To override background recapture, set
`CHROMIUM_REFRESH=2s`, for example.

## Source and builds

```text
un-bound-move/       AppLoad bundle, including ARM64 binaries
un-bound-cdp/        Go Chromium DevTools driver and build script
un-bound-viewer/     Qt/C++ e-ink viewer source and build script
un-bound-engine/     Portable Chromium engine bootstrap and installer
```

Build the driver using Go 1.24 or later:

```sh
bash ./un-bound-cdp/build.sh
```

Build the viewer on Linux with a compatible reMarkable SDK. Copy
`libqsgepaper.so` from **your own tablet** into `un-bound-viewer/` first:

```sh
scp -O root@10.11.99.1:/usr/lib/plugins/scenegraph/libqsgepaper.so un-bound-viewer/
RM_SDK="$HOME/rm-sdk-3.27" bash ./un-bound-viewer/build.sh
```

This proprietary library is ignored by Git and is not distributed here.
The viewer links to the bundled `libquill.so` and the tablet's Qt/display
libraries. The included viewer was built with the 5.7.119 SDK and its
runtime requirements were checked against the connected Move.

## Diagnostics

The headless smoke checks do not take over the tablet's display:

```sh
ssh root@10.11.99.1 /home/root/xovi/exthome/appload/un-bound/smoke-test.sh
ssh root@10.11.99.1 /home/root/xovi/exthome/appload/un-bound/popup-smoke-test.sh
```

The popup check uses an isolated test profile, opens Google sign-in, closes
the popup, and checks return to the original target. It does not sign in to
an account. Do not run it concurrently with another browser smoke check.

App logs are in `/home/root/chromium-engine/out/` and:

```sh
ssh root@10.11.99.1 'journalctl -u un-bound-app --no-pager -n 100'
```

If the app needs to be stopped over SSH:

```sh
ssh root@10.11.99.1 'systemctl stop un-bound-app; systemctl start xochitl'
```

## Credits and licensing

See [THIRD-PARTY.md](THIRD-PARTY.md) for upstream sources and component
licensing notes. Not affiliated with reMarkable, Google, or the UN-BOUND site.
