# Third-Party Notices

RE3HeadTracking bundles, statically links, or credits the third-party components
listed below. Each remains the property of its authors and is used under its own
licence. Where a licence requires the copyright notice, the conditions and the
disclaimer to accompany a binary distribution, the full text is reproduced here
verbatim, and this file ships at the root of every release ZIP we publish.

Nothing in this repository is derived from, or redistributes any part of,
Resident Evil 3.

| Component | Version | Licence | How it ships |
|-----------|---------|---------|--------------|
| REFramework (loader) | nightly-01394-ec6c81fd39831b328027ae00e102bc9c9c3f8aa5 | MIT | Bundled verbatim in the installer ZIP |
| REFramework (plugin SDK headers) | source commit `ec6c81fd39831b328027ae00e102bc9c9c3f8aa5` | MIT | Compiled into `RE3HeadTracking.dll`, so it ships in **both** ZIPs |
| cameraunlock-core | f441e29427b7422a584ba492dddd7788881804b0 | MIT | Compiled into `RE3HeadTracking.dll`, so it ships in **both** ZIPs |
| OpenTrack | n/a | ISC | Not bundled; UDP protocol interoperability only |

The MIT text is identical for all three and is reproduced in full under each
entry below. Both release ZIPs additionally carry it as discrete files under
`licenses/`, so a binary extracted on its own never travels without its notice.

---

## REFramework

REFramework reaches this project two separate ways, and both are covered by the
one MIT licence reproduced below.

**1. The loader binary**, vendored at `vendor/reframework/`, shipped in the
installer ZIP and used as the install-time source. Taken from the upstream
release asset untouched; the upstream licence file ships beside it at
`vendor/reframework/LICENSE`.

- Source code: https://github.com/praydog/REFramework
- Release asset: https://github.com/praydog/REFramework-nightly/releases/download/nightly-01394-ec6c81fd39831b328027ae00e102bc9c9c3f8aa5/REFramework.zip
- Release tag: `nightly-01394-ec6c81fd39831b328027ae00e102bc9c9c3f8aa5`
- Built from REFramework source commit `ec6c81fd39831b328027ae00e102bc9c9c3f8aa5`
  (the SHA the tag embeds), published 2026-07-28
- Tag ref in the `REFramework-nightly` release-hosting repo:
  `0436e043af6f81a5d3fef49ae27d35e63431e566`. That repo hosts release assets
  only, so this SHA does not exist in the source repo above and must not be
  quoted as if it did.
- SHA-256 of `vendor/reframework/RE3.zip`:
  `a3d24f04e41933a7a3a6e1d6402b7de18ca677245d9ca0dda9f6a5ca20e9b94e`

**2. The plugin SDK headers**, vendored at `extern/reframework/` and `#include`d
by our own translation units, so upstream code is compiled into
`RE3HeadTracking.dll`. That binary is the entire payload of the Nexus ZIP, which
carries no vendored loader, so this is why the MIT notice must travel with both
ZIPs and not only with the loader.

- `extern/reframework/API.h` and `extern/reframework/API.hpp`, taken verbatim
  from `include/reframework/` at source commit
  `ec6c81fd39831b328027ae00e102bc9c9c3f8aa5`
- Unmodified: both files were byte-compared against upstream at that commit on
  2026-08-23 and are identical. `extern/reframework/LICENSE` is likewise the
  verbatim upstream file. See `extern/reframework/README.md`.

```
MIT License

Copyright (c) 2019 praydog

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## cameraunlock-core

Git submodule at `cameraunlock-core/`, compiled into `RE3HeadTracking.dll`. Our own code,
MIT licensed, reproduced here so the notices are complete.

- Pinned commit: `f441e29427b7422a584ba492dddd7788881804b0`

```
MIT License

Copyright (c) 2026 itsloopyo

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## OpenTrack

Not bundled and not linked. This mod implements the OpenTrack UDP pose datagram
layout so that OpenTrack (https://github.com/opentrack/opentrack, ISC licence)
and compatible trackers can drive it. No OpenTrack code, headers or binaries
are copied, linked or redistributed, so its licence triggers no notice
obligation here. It is credited because the wire format is its work.

---

## Resident Evil 3

Resident Evil 3 and all related names, logos, characters and marks are
trademarks of their respective owners. They are used here only to identify the
game this mod applies to, which is nominative use and not a claim of any right
in them. This project is an unofficial, fan-made modification. It is not
affiliated with, endorsed by, or sponsored by the game's developers, its
publishers, its engine vendor, or any other rights holder. It redistributes no
game code, no game assets and no proprietary DLLs, and it requires a
legitimately purchased copy of the game. Any engine structure offsets,
function addresses or byte patterns referenced in the source were derived by
the authors through independent analysis of a legitimately owned copy. They
are factual measurements recorded as numbers; no decompiled or disassembled
game code is stored in this repository.
