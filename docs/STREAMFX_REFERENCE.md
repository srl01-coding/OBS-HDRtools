# StreamFX reference record

StreamFX is used as a **behaviour and source-reading reference only**. No StreamFX
code has been copied into this repository. If that changes, record it here and
in `THIRD_PARTY_NOTICES.md` (brief 2.5).

## Pinned revision

| Field | Value |
|---|---|
| Repository | https://github.com/Vhonowslend/StreamFX-Public |
| Commit | `99352c80405a64693ca8c967fafabe141e768e57` (2024-12-13; matches the brief's prefix `99352c8`, branch `master` HEAD on 2026-10-03) |
| Files read | `data/effects/transform.effect`, `components/transform/source/filter/filter-transform.cpp` |

## Verified claims from the brief (at the pinned commit)

| Brief claim | Verified | Evidence |
|---|---|---|
| Corner Pin uses inverse-bilinear mapping, not a homography (S1) | yes | `transform.effect` "Technique: Corner Pin", credits Inigo Quilez's *ibilinear* article |
| Transform allocates `GS_RGBA` cache/output targets (S2) | yes | `filter-transform.cpp` L101-102 `rendertarget(GS_RGBA, ...)` |
| Legacy filter-begin call with `GS_RGBA` (S2) | yes | L412 `obs_source_process_filter_begin(_self, GS_RGBA, OBS_NO_DIRECT_RENDERING)` |
| Mipmap texture allocated as `GS_RGBA` (S2) | yes | L450 |
| Corner coordinates use centred percent, TL = (-100,-100) | yes (default/min/max metadata in `transform.effect`) | |

Consequence: StreamFX 3D Transform at this revision captures its input as
8-bit sRGB (`obs_source_process_filter_begin` = `GS_CS_SRGB`) and would clip/
tone-map an HDR source. Canonical coordinate conversion:
`canonical = (streamfx_percent + 100) / 200`.

## Licensing audit item (unresolved, deliberately)

* `LICENSE` at the pinned commit: **GPL-2.0** text.
* `README.adoc` at the pinned commit: "Licensed under GPLv3 (or later), see LICENSE".
* `transform.effect`: "Copyright (C) 2021-2023 Michael Fabian 'Xaymar' Dirks"; algorithm
  credited to Inigo Quilez.

This inconsistency is why the toolkit implements geometry from the brief's
equations rather than porting StreamFX files.

## Which "3D" filter is in the user's chain?

The user's Ceiling Camera filter list (screenshot, 27 Sep 2026) shows a filter
named **"3D Effect"**. That name matches Exeldro's separate *3D Effect* plugin,
not StreamFX's *3D Transform*. The brief's StreamFX analysis is therefore a
geometry reference, **not** a diagnosis of the user's legacy filter. Identify
the actual plugin/version (OBS log, plugin list) before attributing range loss
to it, and test it with the staircase method in brief 11.5 using
`hdr_toolkit_pattern_dev`.
