# OBS HDR Toolkit

HDR-safe video filters for OBS Studio: **HDR Transform** (corner pin, 3D) and
**HDR Color** (exposure, white balance, six tonal zones, soft clipping), working
in OBS's linear HDR space without 8-bit intermediates or hidden clamps.

**Status: P0 development build.** The production filters are not implemented
yet. This build contains two developer tools only:

* **HDR Toolkit: Test Pattern (developer)** - a source with exact HDR values
  (neutral patches 0-10,000 nits, log ramp, highlight staircase, HLG 0-109%
  levels, wide-gamut colours, alpha fixtures). No video decode, so it is not
  affected by OBS's limited-range input clamp.
* **HDR Toolkit: Neutral (developer test)** - a filter that runs a real HDR
  shader and must leave the image unchanged. Used to prove the render path.

Do not install into a production OBS profile. See `docs/VALIDATION.md` for the
test procedure and what has and has not been verified.

## Install (Windows, test profile)

1. Download the `obs-hdr-toolkit-*-windows-x64` artifact from the latest
   successful *Push* run under **Actions**.
2. Close OBS, copy the archive's contents into the OBS install folder
   (`obs-plugins\64bit\obs-hdr-toolkit.dll`,
   `data\obs-plugins\obs-hdr-toolkit\...`).
3. Remove those two paths to uninstall.

## Documentation

* `docs/ARCHITECTURE.md` - pinned versions, layout, build
* `docs/COLOR_PIPELINE.md` - colour-space, units and alpha contract
* `docs/STREAMFX_REFERENCE.md` - StreamFX provenance and licence audit
* `docs/VALIDATION.md` - PASS / FAIL / NOT RUN table

## Licence

GPL-2.0-or-later. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.
