# Third-party components and provenance

This repository packages a local adaptation of existing browser/display
components. It does not claim a single license covering every component.

## Chromium e-ink browser

- Source: <https://github.com/MaximeRivest/chromium>
- Base release: v0.5.0
- Base source commit: `1bedeec94d53e7581b5d9bc5f3df0df13e85dfe0`
- Adapted files include the Go CDP driver, C++ viewer, launch scripts,
  keyboard font data, and initial loading/error images.
- Consult upstream for permission and applicable licensing before public
  redistribution. The upstream repository did not expose a top-level license
  in the version inspected for this adaptation.

## Quill

- Source: <https://github.com/MaximeRivest/quill>
- Bundled `libquill.so`: upstream v0.1.0 convenience ARM64 build.
- Upstream identifies this clean-room implementation as MIT licensed.
- Its dependency `libqsgepaper.so` is proprietary reMarkable software, must
  come from the user's own tablet, and is not included in this repository.

## Other dependencies

- `github.com/gorilla/websocket` v1.5.3: BSD-2-Clause, downloaded by Go.
- Chromium: downloaded separately from Playwright's distribution, with its
  own Chromium and third-party licenses.
- Ubuntu ARM64 shared libraries: downloaded separately; each package retains
  its own license.
- Embedded keyboard font: identified by upstream as Spleen 16×32; retain and
  consult the Spleen project's applicable notices.
- Qt and the reMarkable SDK: external build/runtime dependencies, governed by
  their own applicable terms.
- `icon.png`: derived from the UN-BOUND website's `icon.svg` for this app tile.

No browser profile, cookies, account tokens, API keys, SSH keys, or proprietary
reMarkable display library are included in the intended Git contents.
