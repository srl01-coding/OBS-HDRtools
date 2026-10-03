# OBS HDR Toolkit - working notes for Claude

Governing spec: *OBS HDR Toolkit: implementation brief for Claude Code* v1.0
(3 Oct 2026). It supersedes earlier conversations. Work packages P0-P5 (brief 12.1).

## Non-negotiables (brief 3, 12.4)
- No `GS_RGBA` intermediate for non-SRGB spaces; no `saturate`/`max(rgb,0)` in HDR paths.
- Do not decode HLG/PQ; do not treat 940 or 1.0 as a ceiling.
- Space policy: preserve native (`docs/COLOR_PIPELINE.md`); libobs converts at the consumer.
- `OBS_NO_DIRECT_RENDERING`; premultiplied output, blend ONE/INVSRCALPHA, push/pop state.
- Identity must be proven with forced shader render, not with a skip path.
- New instances neutral; soft clips/gamut limiter default OFF.
- Production IDs `hdr_toolkit_transform_v1` / `hdr_toolkit_color_v1`: register only when
  their schema is real; any processing-order change needs a versioned migration.
- Mark tests NOT RUN unless actually executed; never infer results.

## Environment
- No OBS/GPU here. Build = GitHub Actions (template), push to `main`.
- Syntax check locally against OBS 32.2.2 headers; CPU tests in `tests/cpu`.
- Format: clang-format 19 (`.clang-format`), gersemi for CMake (CI check-format on main).

## Status
P0: neutral dev filter + pattern source + docs. Next: P1 Corner Pin (bilinear + projective).
