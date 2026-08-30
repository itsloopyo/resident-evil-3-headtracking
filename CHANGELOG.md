# Changelog

## [Unreleased]

Pre-release. Distributed as dev builds (`0.0.0-nightly.<date>.<sha>`); no stable version tagged yet.

### Logging

- Capped the per-frame world-marker projection trace at five lines per session. It ran every 30 frames for the whole session, about 1.3 MB an hour at 60 fps into REFramework's log, which buried the startup lines a user is asked to send.
- The log now names the config file it actually read (`Config loaded from <path>`), so an edit made to the wrong `HeadTracking.ini` is visible in the log instead of costing a support round trip.
- A one-shot `First tracker pose received: yaw/pitch/roll (local|remote connection)` line the first time a tracker packet reaches the mod. It is emitted ahead of every enable/gameplay gate, so its absence means the packets never arrived rather than that tracking was off or the camera hook had not engaged.
- Corrected the log path in the docs. It is `<game>/re2_framework_log.txt`, not `reframework/reframework_log.txt`; REFramework uses that generic name for every RE Engine title.

### Changed
- `Page Up` / `Ctrl+Shift+G` turns positional tracking off and on again instead
  of cycling three modes. The third mode disabled head rotation, and it sat
  directly after the mode a `[Position] Enabled=false` config starts in, so one
  press of a key labelled "toggle position" switched head rotation off.
- The world-marker trace carries the head's lean again, and now also the third
  component of the marker's anchor read, which is where a per-marker depth would
  have to come from.
- The mod keeps no centre of its own. Every tracker app centres itself, so a
  centre in the mod was a second one in series with the tracker's, and the two
  drifted apart because each side moved at moments the other could not see.
  The mod now applies the pose it receives as absolute: centre it in your
  tracker app (OpenTrack's Center bind, the CENTER button in Headcam, SteamVR's
  reset). The `Home` key, the `Ctrl+Shift+T` chord and the
  `[Hotkeys] RecenterKey` ini entry are gone.
- Smoothing is now two user-configurable parameters in a new `[Smoothing]` section of `HeadTracking.ini`: `LocalSmoothing` (default 0.0) for a tracker running on this machine, and `RemoteSmoothing` (default 0.15) for a tracker on a remote network device. The value is picked per connection from the packet source address and is re-evaluated while the game runs, so switching between a local OpenTrack instance and a phone on WiFi takes effect without a restart.
- Removed the `[Position] Smoothing` key. Both new parameters cover rotation and position, so there is no separate position smoothing setting.
- Removed the hidden 0.15 baseline smoothing floor that silently overrode the configured value. Local users now get zero-latency tracking by default.

### Added
- Decoupled head tracking via OpenTrack (UDP 4242)
- 6DOF positional tracking with configurable sensitivity and limits
- Aim decoupling: head moves camera, mouse controls aim independently
- Game state detection: tracking pauses in menus, loading screens, cutscenes, and pause
- Configurable hotkeys: toggle (End), position toggle (PgUp), yaw mode (PgDn)
- INI configuration file with sensitivity, position limits, smoothing, and hotkey settings
- Automated installer with bundled REFramework
- Frame-rate independent smoothing and interpolation pipeline
