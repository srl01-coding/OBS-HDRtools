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
| C10 | `test_color.cpp`: WB(0,0) is the exact identity; 14 non-neutral WB matrices map D65 white to the brief's target white (1e-12) with white luminance preserved; + temperature warmer, - cooler, + tint magenta, - tint green | PASS | 2026-10-04: +30 mired white RGB (1.1120, 0.9857, 0.8115); tint +100 (1.1367, 0.9481, 1.1109) |
| C11 | `check_wb_vs_brief.py`: C++ WB matrices vs the brief's section 15 Python `wb_matrix`, mired -90..90 x tint -100/0/100 | PASS | 15 matrices, max abs difference 7.8e-16 |
| C12 | `test_color.cpp`: exposure exact (+1 EV doubles, -1 halves); contrast fixes the pivot, follows `pivot*(Y/pivot)^k` over 1e-4..1e4 nits, monotonic, black stays black, negative Y finite | PASS | |
| C13 | `test_color.cpp`: wheel hue directions luminance-orthogonal (0/120/240 deg = R/G/B); radius clamped at 1; wheel + saturation preserve linear Y; saturation 0 gives Y-neutral grey; black preserved by the full grade; Grade Mix 0 = original and linear | PASS | linear-Y error 6.1e-16 (5,000 random grades) |
| C14 | `test_color.cpp`: float32 mirror of `hdr-color.effect` vs double reference, 20,000 random grades x colours 0.01-10,000 nits | PASS | max error 3.6e-6 of the pixel's largest channel; neutral params bit-exact |
| C15 | `test_color.cpp`: zone windows continuous (max change 0.001 per 0.001 stop over -30..30), weights in [0,1], full strength and zeros where the schema-4 table puts them; Black and Dark cover Y = 0, negative, 1e-30 and 0.01 nits; Highlight and Specular cover up to 1.2e7 nits; adjacent pairs overlap >= 1.4 stops; Dark..Highlight weights sum to 1 (max deviation 0) and equal pushes on them equal a global push; brief stop markers | PASS | 2026-10-04 (schema 4, seven zones). Plot: `plot_zones.py` |
| C16 | `test_color.cpp`: isolated zone gain = 2^(w EV) at the input mask (pure RGB scale, all six zones); frozen mask (Shadow +3 does not pick up Light -3 after the lift); overlap sums EV; neutral or disabled zones (incl. a widened neutral Light) leave another zone's response bit-identical | PASS | gain law error 0 |
| C17 | `test_color.cpp`: zone saturation 0 desaturates fully at full weight and leaves out-of-window pixels bit-identical; zone wheel preserves Y and black; disabled zone keeps its value and is neutral; default zones enable no stage | PASS | |
| C18 | `test_color.cpp`: float mirror of the zone shader vs double, 20,000 random global + six-zone grades with random open ends; edge sanitising (reorder, NaN reset, fixed open ends cannot close, an open side's unused edges never override a used edge, re-closing gives a valid window) | PASS | max error 6.0e-6 of the pixel's largest channel; all finite |
| C19 | `test_color.cpp`: tonal order kept for a push of 0.98 x falloff/1.5 and reversed at 1.1 x, every default zone, both directions | PASS | |
| C20 | `test_color.cpp`: toe and shoulder reproduce the brief 8.2 / 8.3 tables; toe F(0)=0, F(L)=L, slope 1 at L, monotonic, never lifts, beta 0 exact identity (several L, beta); shoulder slope 1 at H, monotonic, stays below P over 0.01..1e7 nits (q 0.01..0.95); q = 0 hard cap | PASS | 2026-10-04; `math_check.py` toe/shoulder samples also pass |
| C21 | `test_color.cpp` in the grade: toe/shoulder as a common RGB gain (ratios kept), Y <= 0 and black untouched, middle region bit-identical, clips still act at Grade Mix 0, enabled clips are not "neutral"; offset added exactly to every channel incl. black, negative kept, mixed by Grade Mix; offset + toe; L > H conflict detected | PASS | |
| C22 | `test_color.cpp`: float mirror vs double with random offset, clips (incl. q = 0), mix and a zone | PASS | max error 2.0e-6 |

## Layer 1b - build

| ID | Test | Status | Evidence |
|---|---|---|---|
| B1 | Sources compile against OBS 32.2.2 headers (`g++ -fsyntax-only -Wall -Wextra`) | PASS | no diagnostics (re-run 2026-10-04 with P2 sources) |
| B2 | CI Windows x64 build (OBS 31.1.1 SDK) | PASS | offset + soft clip: run 37168988698, commit 525333391, artifact `obs-hdr-toolkit-0.1.0-windows-x64-525333391` (schema 2 zones: 37168271759; P3 zones: 37166894772; P2: 37165577267) |
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


### P2 - HDR Color (global stages)

Setup as above; pattern source with any other toolkit filter removed, `HDR Color`
added. HLG figures below are for a 1000-nit nominal peak and independent of W
(the pattern row is generated in absolute nits from the OBS HLG model). Use the
waveform in RGB parade for H5. Settle sliders by typing values.

| ID | Test | Expected | Status |
|---|---|---|---|
| H1 | New instance: every control at its default; *HLG levels only* with **Force shader render** ON vs filter disabled | identical scope (all levels incl. 105/109%); proves the neutral grade is a real shader identity | NOT RUN |
| H2 | Exposure +1 EV, *HLG levels only* | same readings as N3: 50% -> 63.2% (~618), 70% -> 81.3% (~776), 75% -> 86.1% (~818), 90% -> 100.7% (~946), 100/105/109% at 1023 | NOT RUN |
| H3 | Contrast 1.5, pivot 203.2 nits, *HLG levels only* | 75% bar fixed at 75% (721); 50% -> 37.4% (392), 60% -> 51.6% (516), 70% -> 67.4% (655), 80% -> 82.5% (786), 90% -> 97.3% (916), 100% and above at 1023 | NOT RUN |
| H4 | Contrast 0.75, pivot 203.2 | 75% fixed; 10% -> 17.3% (215), 50% -> 56.9% (562), 90% -> 86.3% (820), 100% -> 93.9% (886) | NOT RUN |
| H5 | White balance on the 75% bar (RGB parade): temperature +30 / -30 mired; tint +100 / -100 | +30: R 76.7 / G 74.8 / B 71.5%; -30: R 73.2 / G 75.1 / B 78.2%; tint +100: R 77.1 / G 74.1 / B 76.7%; tint -100: R 72.7 / G 75.8 / B 73.1%. Visually: + warmer, + tint magenta | NOT RUN |
| H6 | Saturation 0 on the Chart | colour row (all patches 100 nits luminance) becomes one grey level at 63.0% (~616); neutral rows unchanged | NOT RUN |
| H7 | Saturation 1.5 and wheel (0.5, 0) on the Chart, Rec.2020 primaries row | no black, white, flashing or NaN pixels; colours shift as expected. (Linear-Y preservation is a CPU test, C13: the scope's luma is non-constant-luminance Y' of HLG-encoded R'G'B', so it legitimately moves with saturation) | NOT RUN |
| H8 | Exposure +1 EV with Grade mix 0.5 | x1.5 nits: 50% -> 58.0% (572), 75% -> 81.5% (778), 90% -> 96.3% (907). Grade mix 0 = identical to H1 | NOT RUN |
| H9 | Set non-default values in every control, save, restart OBS (test collection) | all values restored; scope identical before/after restart; *Reset grade* returns all grade controls (not Advanced) to defaults | NOT RUN |
| H10 | HDR Color on an SDR image source on the HLG canvas, exposure +1 EV | brightens; highlights clip at SDR white (75% at W = 203) because an SDR source stays in its native SDR space (brief 5.1: no hidden SDR -> HDR promotion) | NOT RUN |
| H11 | Log: one `[color] ... input=...` line per context change; out-of-range values in a hand-edited scene file log `(clamped)` | | NOT RUN |


### P3 - HDR Color tonal zones

Setup as P2. Figures for gray reference 18 nits, 1000-nit peak, schema-4 defaults
(seven zones, handovers at -4 / -1 / +1 / +3 stops, Specular full from +5).

| ID | Test | Expected | Status |
|---|---|---|---|
| Z1 | Defaults with Force shader render ON vs filter off (*HLG levels only*) | identical (zone stage inactive when every zone is neutral) | NOT RUN |
| Z2 | Diagnostic *All zones*, *HLG levels only* | 0% Black + Dark; 5% mostly Black + Dark; 10% Dark/Shadow half each; 20% Shadow; 30% Midtones (grey); 40% Midtones + some Light; 50-60% Light (yellow); 70% Light/Highlight; 75% Highlight + a little Specular; 90% and up Highlight + Specular | NOT RUN |
| Z3 | Diagnostic *Zone mask*, Midtones (W = 203) | 30% bar reads 75% (w 1); 40% bar 70.6% (w 0.77); 50% bar 30.7% (w 0.08); 20% bar 11% (w 0.01); 10% at 0 | NOT RUN |
| Z4 | Midtones +1 EV | 30% -> 40.0% (415); 40% -> 49.9% (501); 50% -> 51.1% (512); all other bars unchanged | NOT RUN |
| Z5 | Shadow +1 EV | 10% -> 11.4% (164); 20% -> 26.6% (297); others unchanged | NOT RUN |
| Z6 | Light +1 EV | 40% -> 42.8%; 50% -> 62.3% (609); 60% -> 72.0% (695); 70% -> 75.0% (721); 75% -> 75.9% (729). +1 EV is exactly the tonal-order limit of a 1.5-stop fade, so 70-75% nearly flatten | NOT RUN |
| Z7 | Highlight -1 EV | 70% -> 63.3% (618); 75% -> 64.2% (626); 80% -> 68.6%; 90% -> 79.1% (757); 100% -> 89.3% (847); 109% -> 98.5% (927) | NOT RUN |
| Z8 | Dark +1 EV | 0% stays 0 (gain, not lift); 5% -> 6.7%; 10% -> 11.8% (167) | NOT RUN |
| Z9 | Specular -1 EV | 75% -> 73.2%; 80% -> 74.9%; 90% -> 79.2%; 100% -> 89.3%; 109% -> 98.5% | NOT RUN |
| Z10 | Dark, Shadow, Midtones, Light, Highlight all +0.5 EV | identical to global exposure +0.5 EV (chain weights sum to 1) | NOT RUN |
| Z11 | Untick a zone with a non-zero setting | effect disappears, values kept; tick again restores | NOT RUN |
| Z12 | Dark: untick *Extend down to black* | fields switch to centre / width / both falloffs; mask gains a lower fade at -9..-7 stops; tick again restores the open end | NOT RUN |
| Z13 | Move Midtones centre / width / falloffs; then *Reset this zone* | mask moves accordingly; reset restores defaults and only that zone | NOT RUN |
| Z14 | Save, restart OBS | all zone values, enables and open flags restored; scope identical | NOT RUN |
| Z15 | Global exposure +1 EV with diagnostic *All zones* | colour bands shift one stop down the picture (masks follow global exposure by design) | NOT RUN |

### Offset and soft clip

| ID | Test | Expected | Status |
|---|---|---|---|
| S1 | Offset +0.05 nits, *HLG levels only* | 0% -> 2.8% (code 88); 5% -> 5.5% (112); 10% -> 10.2% (153); 20% and up within 0.1% | NOT RUN |
| S2 | Offset -0.05 nits | 0% and 2% bars at 0% (below black internally, clipped at encode); 3% -> 1.4%; 5% -> 4.4%; 10% -> 9.8% | NOT RUN |
| S3 | High soft clip on (peak 1000, knee 750) with exposure +1 EV | 75% -> 86.1% (818, below the knee: unchanged by the clip); 90% -> 98.1% (924); 100% -> 99.4% (934); 105% -> 99.6%; 109% -> 99.7% (937). Nothing reaches 100%: peak 1000 nits = HLG 100% is an asymptote | NOT RUN |
| S4 | High clip, peak 1000, knee 500, 0 EV | 80% unchanged; 100% -> 95.6% (901); 105% -> 97.0%; 109% -> 97.7% (920) | NOT RUN |
| S5 | High clip with knee typed equal to peak (1000) | hard cap: 100% and above all at 100% (940) | NOT RUN |
| S6 | Low soft clip on, knee 1 nit, strength 1 | 2% -> 0.55%; 5% -> 3.3%; 8% -> 7.5%; 10% and up unchanged; 0% stays 0% | NOT RUN |
| S7 | Grade Mix 0 with the high clip on (peak 1000, knee 750) and exposure +1 EV | exposure gone (75% and 90% back at 75% / 90%) but the clip still acts: 100% -> 98.0% (922); 105% -> 98.9%; 109% -> 99.3% | NOT RUN |
| S8 | Both clips on, low knee typed above the high knee | status line shows CONFLICT, picture keeps the previous curves, log warning; fixing the value clears it | NOT RUN |
| S9 | Save, restart | offset and clip settings restored | NOT RUN |

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
