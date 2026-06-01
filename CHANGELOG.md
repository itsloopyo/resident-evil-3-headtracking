# Changelog

## [Unreleased]

Pre-release. Distributed as dev builds (`0.0.0-nightly.<date>.<sha>`); no stable version tagged yet.

### Added
- Decoupled head tracking via OpenTrack (UDP 4242)
- 6DOF positional tracking with configurable sensitivity and limits
- Aim decoupling: head moves camera, mouse controls aim independently
- Game state detection: tracking pauses in menus, loading screens, cutscenes, and pause
- Auto-recenter on first tracking connection
- Configurable hotkeys: toggle (End), recenter (Home), position toggle (PgUp), reticle toggle (Insert)
- INI configuration file with sensitivity, position limits, smoothing, and hotkey settings
- Automated installer with bundled REFramework
- Frame-rate independent smoothing and interpolation pipeline
