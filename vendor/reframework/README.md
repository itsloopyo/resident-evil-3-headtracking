# reframework (vendored)

This directory contains a bundled copy of the upstream mod loader. It is the install-time
source of truth: install.cmd extracts directly from here and never reaches out to the network.
Refresh manually with `pixi run update-deps`, then commit.

## Snapshot

- Asset: `RE3.zip`
- Tag: `nightly-01366-6216ec39697c5b3469e08baf0b98db0baff49c49`
- Commit: `0436e043af6f81a5d3fef49ae27d35e63431e566`
- Upstream URL: https://github.com/praydog/REFramework-nightly/releases/download/nightly-01366-6216ec39697c5b3469e08baf0b98db0baff49c49/RE3.zip
- SHA-256: `7dae046c1a22ba5b801aa479c53dae7db75b8d2048928a80249483cd70c8d6c5`
- Fetched at: 2026-05-29T08:23:05.3602203+01:00
- Source: github

Do not edit this directory by hand. Run ``pixi run package`` (or CI release) to refresh.
