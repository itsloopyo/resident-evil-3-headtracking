# reframework (vendored plugin SDK headers)

Upstream REFramework plugin SDK headers, taken verbatim. Unlike
`vendor/reframework/`, which holds the loader zip the installer extracts, these
headers are `#include`d by our own sources (`src/plugin_main.cpp`,
`src/camera/camera_hook.cpp`, `src/camera/game_state_detector.cpp`), so upstream
code is compiled into `RE3HeadTracking.dll` and is redistributed in every
release ZIP that carries that DLL.

## Snapshot

- Upstream: https://github.com/praydog/REFramework
- Source path: `include/reframework/`
- Source commit: `ec6c81fd39831b328027ae00e102bc9c9c3f8aa5` (2026-07-28)
- Files: `API.h`, `API.hpp`, `LICENSE`
- Licence: MIT, reproduced verbatim at `LICENSE` beside these headers and in the
  repository's `THIRD-PARTY-NOTICES.md`

The source commit is the SHA embedded in the vendored loader's release tag
(`nightly-01394-ec6c81fd...`), so the headers we compile against and the loader
binary we ship are the same upstream revision.

## Verification

Byte-compared against upstream at that commit on 2026-08-23: `API.h`, `API.hpp`
and `LICENSE` are all identical, with no local modifications. Re-check with:

```sh
SRC=ec6c81fd39831b328027ae00e102bc9c9c3f8aa5
for f in API.h API.hpp; do
    curl -sfL "https://raw.githubusercontent.com/praydog/REFramework/$SRC/include/reframework/$f" \
        | diff - "extern/reframework/$f" && echo "$f identical"
done
curl -sfL "https://raw.githubusercontent.com/praydog/REFramework/$SRC/LICENSE" \
    | diff - extern/reframework/LICENSE && echo "LICENSE identical"
```

Do not edit these files. To move to a newer REFramework, re-copy them from
upstream at the new commit, update the commit above, and re-run the check.
