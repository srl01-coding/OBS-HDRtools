# Validation report

Status values: **PASS** / **FAIL** / **NOT RUN**. A build with gates NOT RUN is a
development build, not a verified release (brief 11.9). Record environment and
evidence for every result; never infer a result from a different layer.

## Environment

| Item | Value |
|---|---|
| Authoring/CI environment | Linux container (no GPU, no OBS) + GitHub Actions (template workflows) |
| Build SDK | OBS 31.1.1 (template buildspec) |
| User's OBS version / OS / GPU / backend | NOT RECORDED - fill in when running OBS tests |
| Canvas colour space / format, SDR white, HDR nominal peak | Canvas base 2090x990 (from preview status bar); colour settings NOT RECORDED |
| Capture device, pixel format, source range, camera profile | NOT RECORDED |

## Layer 1 - CPU maths (run in authoring environment)

| ID | Test | Status | Evidence / measured |
|---|---|---|---|
| C1 | Brief section 15 `math_check.py` (geometry round trips, toe/shoulder, WB, luma preservation) | PASS | 2026-10-03, Python 3 + NumPy 2.4.4: bilinear inverse max err 6.7e-16 class, homography round trip 1.2e-15 class, shoulder knee slope 0.99999997, centres (0.5,0.5) vs (0.5,0.375), `all_checks_passed: true` |
| C2 | `test_pattern.cpp`: HLG OETF round trip 0-110% | PASS | max abs error 1.1e-16 |
| C3 | `test_pattern.cpp`: HLG level -> nits -> OBS encoder model -> level | PASS | error < 1e-12; 75% = 203.15 nits, 100% = 1000.00003 nits (BT.2100 constant rounding) |
| C4 | `test_pattern.cpp`: chart neutral patches exact, colour row luminance 100 nits, Rec.2020 primaries have negative Rec.709 components, alpha levels, invalid settings rejected | PASS | |

| C5 | `test_quad.cpp`: brief 4.5 centre fixture | PASS | bilinear (0.5, 0.5), projective (0.5, 0.375) |
| C6 | `test_quad.cpp`: double round trips, 4000 samples over brief quads | PASS | inverse bilinear 6.7e-16, homography 7.8e-16 |
| C7 | `test_quad.cpp`: float32 mirror of the shader vs double, 400,000 interior samples incl. near-parallelograms (g ~ 1e-6, 1e-4) and strong keystones, at 3840x2160 | PASS | bilinear max 0.0016 px, projective max 0.0028 px (brief target < 0.01 px); 0 interior misses |
| C8 | `test_quad.cpp`: outside points transparent (both models), identity exact | PASS | identity max error 0 |
| C9 | `test_quad.cpp`: validation - accepts identity, trapezoid, brief quads, mirrored, offscreen corners, parallelogram, near-parallelogram; rejects bow-tie, duplicate, concave, sliver, collinear, out of range, NaN | PASS | |

## Layer 1b - build

| ID | Test | Status | Evidence |
|---|---|---|---|
| B1 | Sources compile against OBS 32.2.2 headers (`g++ -fsyntax-only -Wall -Wextra`) | PASS | no diagnostics |
| B2 | CI Windows x64 build (OBS 31.1.1 SDK) | PASS | run 37128414670, commit c7f078b49, artifact `obs-hdr-toolkit-0.1.0-windows-x64-c7f078b49` |
| B3 | CI macOS / Ubuntu builds, clang-format 19 + gersemi checks | PASS | same run; builds are not a macOS/Linux support claim (nothing run there) |

## Layer 2 - OBS / GPU (requires the user's machine)

Setup for all: a **test scene collection/profile**, not production. Canvas
Rec.2100 HLG, record SDR white W; HDR nominal peak 1000 nits (expected HLG values below
assume a peak <= 1000, OBS's x10 normalisation path). Source: `HDR Toolkit: Test
Pattern (developer)`, Chart, 1920x1080. Scope: srl01-coding/obs-color-monitor
`hdr-hlg` build (HLG % and 10-bit code scales), target = the pattern scene.

| ID | Test | Expected | Status |
|---|---|---|---|
| G0 | Plugin loads; log shows `plugin loaded (version 0.1.0, built against libobs 31.1.1, running on <ver>)`; no `failed to compile` error | load OK | PASS (2026-10-04, user: OBS 32.2.2 Windows; filter and source both listed and rendering; log line not yet captured) |
| P1 | Pattern alone, mode *HLG levels only*: reads 0,10,...,100% on the lines, 105% at ~984, 109% at ~1019 | exact to scope resolution; proves values above 940 reach the scope without the input-decoder clamp | NOT RUN |
| P2 | Pattern alone: neutral 203-nit patch at 75% only when W = 203; other W moves it (W-relative storage, absolute nits meaning) | consistent with W | NOT RUN |
| N1 | Pattern -> Neutral (force render ON, gain 0): scope identical to P1 (every bar, ramp shape, 105/109% bars) | no change. Note: user saw patch-border differences when the item was scaled - neutral effect used a point sampler for the final (scaled) draw; changed to linear in P1 build, re-check | PASS (qualitative, 2026-10-04: Chart mode, filter off vs on at 0 EV, docked scope after the obs-color-monitor minification fix; traces visually identical. Exact per-level reading in *HLG levels only* mode still to do) |
| N2 | Same with force render OFF (skip path) | identical to N1 | NOT RUN |
| N3 | Neutral with test gain +1 EV (x2 nits) on the HLG row: 50% -> 63.2% (code ~618), 60% -> 72.0% (~695), 70% -> 81.3% (~776), 75% -> 86.1% (~818), 80% -> 90.9% (~860), 90% -> 100.7% (~946); 100/105/109% bars all at code 1023 (OBS HLG encode ceiling, ~110%) | shader live, no clamp at SDR white (a clamp at 1.0 would pile everything >= 75%-at-W=203 onto one level) | NOT RUN |
| N3b | Neutral with negative test gain on the Chart: brightest patch (10,000 nits) brought down to the HLG ceiling. Model: code 1023 (E' = 1.0947) = 1866 nits, so 10,000 nits reaches it at -2.42 EV | consistent with no clamp at SDR white: a clamp at linear 1.0 would cap the chart at 75% at 0 EV and lower for any negative gain | PASS (observational, 2026-10-04: user reports about -2.3 to -2.35 EV puts the top at 109%; within reading precision of -2.42) |
| N4 | Two Neutral instances stacked: identical to N1 (no progressive tone mapping) | | NOT RUN |
| N5 | SDR preview of the HDR chain looks tone-mapped (expected), while the scope (HDR) is unchanged | boundary conversion only | NOT RUN |
| N6 | Alpha row over a bright background in a scene: no dark/bright fringes, alpha 0 patch invisible, 0.0001 patch invisible to the eye | | NOT RUN |
| N7 | Neutral on an SDR source (e.g. image) on HLG canvas: unchanged vs without filter | | NOT RUN |
| N8 | Log: one `[neutral] ... input=... consumer=...` line per change, not per frame | | NOT RUN |

### P1 - HDR Transform (Corner Pin)

Setup as above; pattern source with `HDR Toolkit: Neutral` removed, `HDR Transform` added.

| ID | Test | Expected | Status |
|---|---|---|---|
| T1 | Identity, *Force shader render* ON vs filter disabled, pattern *HLG levels only* | identical scope (levels and order) | NOT RUN |
| T2 | Moderate warp (TL 10%,5% / TR 90%,0% / BR 100%,100% / BL 0%,95%), both warp models, *HLG levels only* | every level still present at the same heights (geometry moves columns, never levels); 105%/109% bars intact; only patch-border pixels show interpolated in-between values with bilinear sampling, none with point | NOT RUN |
| T3 | Brief 4.5 trapezoid TL 20%,0 / TR 80%,0 / BR 100%,100% / BL 0%,100% on the Chart | row boundaries (source v = 0.18 / 0.32 / 0.48 / 0.64 / 0.80) at y = 18 / 32 / 48 / 64 / 80% with Bilinear, and 11.6 / 22.0 / 35.6 / 51.6 / 70.6% with Projective | NOT RUN |
| T4 | Drag TL past TR (bow-tie), and TL onto TR | status "Geometry INVALID ..." with reason; image stays on the last valid shape; typed numbers not reset; fixing them recovers | NOT RUN |
| T5 | Warped chart over a bright coloured background source | outside the quad shows the background; alpha row correct; no dark or bright fringes on edges | NOT RUN |
| T6 | Corners outside the canvas (e.g. TL -20%,-20%) | image clipped at the output rectangle, no smearing of edge pixels | NOT RUN |
| T7 | Transform on an SDR image source on the HLG canvas | warps correctly, colours unchanged vs no filter | NOT RUN |
| T8 | Log: one `[transform] ... input=...` line per change; invalid geometry logs a warning | | NOT RUN |

## Layer 3 - production path

| ID | Test | Status |
|---|---|---|
| E1 | Camera chain A/B with Neutral inserted at each position of the existing filter list (staircase from pattern where possible) | NOT RUN |
| E2 | Encoded HDR output metadata unchanged by plugin | NOT RUN |

## Diagnosis record (brief 11.5)

| Date | Chain | Observation | Classification |
|---|---|---|---|
| 2026-09-27 | 10-bit HLG Media Source -> obs-shaderfilter 2.6.0 (x4.17 linear gain) | all bars >= ~50% HLG identical, histogram spike at SDR white (~code 740 at W=203) | clipping at linear 1.0 inside/after obs-shaderfilter |
| 2026-09-27 | 10-bit HLG Media Source (Color Range Auto) | code 1019 bar identical to 940 | OBS input decoder limited-range clamp (`YUV_to_RGB`) |
