# Third-Party Notices

## REFramework

- **Version:** nightly-01366 (commit `0436e043af6f81a5d3fef49ae27d35e63431e566`)
- **License:** MIT
- **Upstream:** https://github.com/praydog/REFramework
- **Usage:** Plugin host and SDK for RE Engine games. Provides method hooking, type system access, ImGui overlay, and D3D rendering hooks.
- **Bundled:** yes. Bundled in the release ZIP and used as the install-time source.

---

## OpenTrack

- **Version:** N/A (UDP protocol only)
- **License:** ISC
- **Upstream:** https://github.com/opentrack/opentrack
- **Usage:** Head tracking source. We receive tracking data via the UDP protocol (port 4242). No OpenTrack code is bundled.
- **Bundled:** no.

---

## CameraUnlock Core

- **Version:** git submodule (see `cameraunlock-core`)
- **License:** MIT
- **Upstream:** https://github.com/itsloopyo/cameraunlock-core
- **Usage:** Shared C++ library providing the UDP receiver, tracking processing pipeline, smoothing, interpolation, hotkey input, and math utilities. Compiled into the plugin DLL.
- **Bundled:** no.

---
