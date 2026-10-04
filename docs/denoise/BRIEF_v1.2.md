# OBS HDR Denoise — Claude Code Implementation Brief v1.1

## 0. Context and target

This brief supersedes the earlier denoise handoff where they conflict. It incorporates the implementation learnings from the working OBS HDR Toolkit.

Target environment:
- Windows 10
- OBS runtime 32.2.2
- existing HDRtools build against OBS 31.1.1 SDK
- Rec.2100 HLG canvas
- nominal peak 1000 nits
- SDR white 203 nits
- GTX 1650 Super 4 GB
- i7-7700K
- 3840x2160 30 fps
- denoise the finished program canvas **once**, after switching/dissolves, not each camera separately

Required algorithms:
1. HQDN3D — lightweight spatial + temporal mode
2. NLMeans — stronger spatial mode

Temporal NLMeans is deferred.

---

# 1. Architectural rule

Do not invent a second HDR pipeline.

Reuse the exact OBS HDR handling already proven in `OBS-HDRtools`:

- query/propagate the target's native graphics color space;
- support `GS_CS_709_EXTENDED`;
- use the high-precision format OBS chooses for the space;
- do not decode HLG/PQ inside denoise;
- do not encode HLG/PQ inside denoise;
- do not treat code 940 as an internal target;
- do not clamp RGB to `[0,1]`;
- preserve values above SDR white and above nominal 1000-nit working levels;
- do not introduce an 8-bit intermediate;
- do not use CPU readback for the GPU production path.

The denoiser operates on OBS's HDR working representation, not the camera's encoded HLG signal.

---

# 2. Reuse the lessons from the HDRtools build

These are established project learnings and should be treated as defaults unless current source/runtime testing disproves them.

## 2.1 Measurement is P0, not polish

Stock OBS scopes are not adequate. Use the existing HLG-aware `obs-color-monitor` fork and the exact-value synthetic test source.

For every deterministic runtime gate, give the user:
- expected HLG percentage;
- expected 10-bit code;
- expected qualitative behavior;
- expected log line(s).

Record PASS / FAIL / NOT RUN honestly. "Looks fine" is qualitative, not a formal PASS.

## 2.2 Decoder range can lie to you

OBS limited-range input decode clamps before filters. Super-whites beyond nominal range may never reach denoise.

Therefore:
- exact-value synthetic source is authoritative for range testing;
- do not debug denoise because a decoded file/source failed to carry super-white;
- when needed, explicitly test source Color Range overrides separately.

## 2.3 Current HLG landmarks for this production setup

Useful current references:
- ~75% HLG = SDR white = 203 nits
- 100% HLG = 1000 nits
- encoded headroom extends to roughly 109%; code 1023 is around 1866 nits in the current OBS HLG model

The denoiser must not assume 100% HLG / 1000 nits is the absolute working ceiling.

## 2.4 Known false culprits

Existing filters can cap HDR:
- Compose SDR on HDR: around 480 nits / ~88% HLG
- obs-shaderfilter 2.6.0: observed SDR-white-like ~75% ceiling
- stock Color Correction / Color Grade: skip `GS_CS_709_EXTENDED`

When something caps at 75/88/100%, diagnose the chain before modifying denoise code.

## 2.5 Runtime shader compilation

OBS `.effect` files compile when OBS loads them. GitHub Actions success does **not** prove shaders work.

Each runtime gate must check the OBS log for shader/effect creation failures.

Known effect-language lessons:
- avoid scientific exponent literals like `1e-6`; use decimal literals;
- `ddx` / `ddy` work;
- `Load` works;
- inside dynamic loops, prefer `Load` rather than `Sample`.

## 2.6 Effect/state pitfalls

`gs_technique_end()` resets effect parameter values. If two techniques execute in sequence, set all required parameters again.

The final presentation/scaled draw should normally use linear sampling; point sampling caused visible block/patch boundaries when scaled.

## 2.7 OBS property UI

Do not continuously return `true` from slider/text modified callbacks just to refresh labels. It rebuilds widgets and breaks drags/focus.

Modified callbacks also run while the properties panel opens. Write-through/mirror controls must be idempotent and self-consistent.

Use a development schema until denoise defaults stabilize; do not spend time migrating tuning changes during active development.

---

# 3. Final-program placement

Desired:

```text
Camera A --\
            scene / switch / dissolve --> final HDR program --> HDR Denoise --> output
Camera B --/
```

During a dissolve, both cameras are rendered for the composite, but **denoise should run once on the completed blended program frame**.

Do not attach one expensive NLMeans instance to every camera.

---

# 4. Program texture integration

Investigate OBS 32.2.2 source around:
- `obs_add_main_rendered_callback`
- main video mix/render loop
- main texture reuse
- streaming + recording consumers
- output conversion

The callback/API exists, but do **not** assume every callback invocation is a unique program frame.

The implementation must guarantee:

```text
one unique program frame
        -> one denoise dispatch
        -> one temporal-history update
```

Simultaneous streaming + recording must not double-update temporal history.

Preview/multiview rendering must not advance program denoise history.

Add counters:

```text
program_frames_seen
unique_program_frames
denoise_dispatches
duplicate_callbacks_skipped
history_updates
history_resets
```

Acceptance:

```text
denoise_dispatches == history_updates == unique_program_frames
```

for active denoise modes.

If an ordinary OBS filter can be placed at the true completed-program stage without per-camera duplication, that is preferable. Otherwise use the main-rendered/program-texture route with explicit frame identity.

Do not patch OBS core unless public/plugin APIs prove insufficient.

---

# 5. GPU backend strategy

Do not decide "CUDA" by default merely because the GPU is NVIDIA.

First preference: the GPU path that integrates most cleanly with the already-working HDRtools/OBS graphics architecture.

Candidate A:
- Direct3D 11 compute / native OBS graphics device path

Candidate B:
- CUDA <-> D3D11 interoperability

Decision gate:
- implement a minimal 4K identity/copy compute path in candidate backend(s);
- measure dispatch + synchronization + copies;
- choose total frame cost and maintainability, not theoretical compute throughput.

Never use a GPU->CPU->GPU roundtrip for NLMeans.

CPU HQDN3D is allowed as a benchmark/reference backend because the algorithm is light, but it is not the default production architecture unless end-to-end 4K30 timing proves it safe.

---

# 6. Algorithm selector

```text
Algorithm:
  Off
  HQDN3D
  NLMeans
```

No automatic switching in v1.

Common controls:
- Mix
- Reset History
- Debug View
- Debug Timing

---

# 7. HQDN3D — why and what

HQDN3D is the lightweight mode.

FFmpeg's implementation is derived from MPlayer. Its key behavior is:
- temporal nonlinear low-pass against a previous filtered frame;
- spatial nonlinear recursive filtering with prior filtered neighbors;
- separate luma/chroma and spatial/temporal strengths.

Reference:
- FFmpeg `libavfilter/vf_hqdn3d.c`
- FFmpeg `libavfilter/vf_hqdn3d.h`

Do not directly copy GPL code unless the licensing decision is deliberate and documented. A clean reimplementation from algorithmic behavior is preferred.

Historical FFmpeg default strengths are useful only as behavioral references. They are **not** suitable numeric defaults for linear HDR working values.

---

# 8. HQDN3D implementation order

## P1A — temporal-only first

Implement temporal-only HQDN3D before spatial HQDN3D.

Why:
- no spatial recurrence;
- one persistent history frame;
- per-pixel parallel;
- cheap;
- validates stateful program-frame processing;
- validates cut reset and transition protection.

Conceptually:

```text
difference = comparison(history, current)
weight/response = nonlinear_function(difference, temporal_strength)
history_new = lowpass(history, current, response)
output = history_new
history = history_new
```

The exact response should have a CPU reference.

### History reset conditions

Reset/initialize history from current frame on:
- plugin start;
- resolution change;
- color-space/format change;
- algorithm change into temporal mode;
- manual reset;
- hard cut;
- graphics/device recreation.

On reset:
- output current frame;
- set history = current frame;
- do not blend stale pixels.

---

# 9. HDR denoise comparison domain

A fixed threshold in linear RGB is undesirable because noise significance varies enormously from shadows to highlights.

Use an HDR-appropriate comparison domain while reconstructing/averaging the **original linear HDR values**.

Candidate signed compression:

```text
f(x) = sign(x) * log2(1 + abs(x) / K)
```

Requirements:
- defined for negative components;
- monotonic;
- stable at zero;
- no NaN/Inf;
- float32 safe;
- useful strength behavior in darks and highlights;
- zero strength = identity.

Do not freeze `K` from theory alone. Tune against:
- synthetic HDR ramps/noise;
- actual a6400/a6700 footage;
- scope + difference view.

Alternative noise-aware mappings are allowed if better justified and tested.

---

# 10. Luma/chroma model

The filter receives RGB working textures, not encoded YUV.

Use linear Rec.709-primary luminance:

```text
Y = 0.2126 R + 0.7152 G + 0.0722 B
```

Derive reversible chroma residuals/opponent coordinates so the UI can expose:
- Spatial Luma
- Spatial Chroma
- Temporal Luma
- Temporal Chroma

Requirements:
- neutral remains neutral;
- independent luma/chroma control;
- no color cast;
- moderate saturated colors remain stable;
- negative/out-of-gamut components are not needlessly destroyed.

A simple RGB temporal prototype is acceptable for P1A plumbing, but luma/chroma separation is required before HQDN3D is considered complete.

---

# 11. HQDN3D hard cuts and dissolves

Because denoise is after the program switcher, temporal history belongs to the **program stream**.

## Hard cut

Previous-camera history must not contaminate the new angle.

Use a cheap frame-change detector:
- downsampled luminance SAD/MAD;
- or histogram distance;
- or small luminance pyramid.

Do not run a separate expensive full-resolution detector.

If change >= hard-cut threshold:
- reset history to current frame;
- output current frame;
- increment `history_resets`.

## Dissolve / blend transition

Do not reset on every frame of a dissolve.

Strong temporal filtering can resist the dissolve and create trails.

Implement **Transition Protection**:
- below hard-cut threshold, as inter-frame change rises, reduce temporal strength smoothly;
- restore configured strength as the picture settles.

Controls:
- Scene Cut Reset [on]
- Cut Sensitivity
- Transition Protection [on]
- Protection Amount

Image-based detection is acceptable even if OBS transition state is available. Avoid tight coupling to one transition implementation unless it offers a clear advantage.

---

# 12. HQDN3D spatial is a special GPU problem

Classic HQDN3D spatial filtering is recursive. Each filtered sample depends on already-filtered spatial neighbors.

A naive one-thread-per-pixel port is **not equivalent**.

Claude must prototype and benchmark two approaches:

## A. Independent recursive/separable GPU implementation

From the project's written mathematical specification, design horizontal/vertical or scanline recurrences in passes/tiles that preserve the characteristic recursive spatial behavior. Do not translate FFmpeg/MPlayer implementation structure.

Potential issue:
- serial dependency along each line;
- GPU underutilization if designed badly.

## B. GPU-friendly HQDN3D-style approximation

Use nonlinear separable neighborhood passes that preserve:
- lightweight cost;
- nonlinear edge sensitivity;
- similar denoise character;
- independent luma/chroma controls.

If behavior diverges materially from classic HQDN3D, label/document it as `HQDN3D-style` rather than claiming exact equivalence.

Do not choose by aesthetic preference alone:
- compare visual result;
- compare CPU/reference behavior where meaningful;
- benchmark 4K30.

---

# 13. CPU HQDN3D benchmark

Implement or retain a CPU reference.

The i7-7700K is old, but HQDN3D is cheap enough that a CPU production backend is worth **measuring**, not assuming.

Benchmark:
1. pure CPU filter compute time;
2. GPU readback synchronization;
3. CPU->GPU upload;
4. total added OBS frame latency;
5. rendering lag.

If transfers dominate, reject CPU production mode even if the arithmetic is fast.

Never use CPU for NLMeans production.

---

# 14. NLMeans scope

NLMeans v1 is **spatial only**.

Temporal NLMeans is deferred because:
- HQDN3D already provides cheap temporal denoise;
- final-program cuts/dissolves complicate temporal NLM;
- spatial NLM is the likely GPU bottleneck and should be solved first.

---

# 15. NLMeans presets to benchmark

Initial controlled presets:

| Preset | Patch | Search | Purpose |
|---|---:|---:|---|
| Light | 3x3 | 7x7 | likely first 1650 Super candidate |
| Balanced | 5x5 | 11x11 | quality/performance midpoint |
| Strong | 5x5 | 15x15 | stress case |

These are benchmark presets, not universal quality claims.

Expose Custom only after the presets are stable.

Controls:
- Patch Size
- Search Size
- Luma Strength
- Chroma Strength
- Mix

---

# 16. NLMeans GPU design

For each target pixel:
1. compare target patch with candidate patches in search region;
2. compute patch distance in HDR comparison domain;
3. convert distance to a weight;
4. weighted-average original linear HDR candidate values.

Candidate weight:

```text
w = exp(-distance / (h*h))
```

An approximation is allowed for GPU efficiency if:
- monotonic;
- stable;
- CPU mirror matches;
- visual result is validated.

Guarantee non-zero denominator via self weight.

### Bounded memory is mandatory

Do not create one full-resolution distance texture for every search offset.

Use tiling/shared memory and scratch reuse.

4K RGBA16F is roughly 63 MiB per full-resolution texture. On a 4 GB 1650 Super, resource planning matters.

Log/debug-estimate additional allocations.

---

# 17. Alpha and graphics

Because denoise is on the final canvas it may process:
- lower thirds;
- logos;
- small text;
- overlays.

Default:
- preserve alpha;
- do not let transparent pixels contaminate opaque neighbors;
- do not clamp RGB because alpha < 1.

Required tests:
- static fine text;
- moving lower third;
- static logo;
- dissolve behind/over graphics.

If final-canvas denoise visibly harms graphics, later options:
1. protection mask;
2. denoise camera composite before graphics;
3. graphics-aware edge protection.

Do not hide this tradeoff.

---

# 18. Resource lifecycle

Correctly handle:
- plugin create/destroy;
- source/program resolution change;
- HDR/SDR color-space change;
- graphics reset/device recreation;
- algorithm switch;
- temporal enable/disable.

On any incompatible resource change:
- destroy/recreate GPU resources safely;
- reset temporal history;
- never reuse old-resolution history.

Failure behavior:
- transparent bypass/pass-through;
- log warning once;
- never output black because denoise initialization failed.

---

# 19. Performance telemetry

Add debug telemetry without rebuilding OBS properties every frame.

Useful metrics:
- algorithm;
- resolution/format;
- GPU denoise time average;
- p95/max over recent window if timestamp queries are reliable;
- dispatch count;
- duplicate callbacks skipped;
- history resets;
- estimated extra VRAM.

Do not spam the log every frame.

An optional status dock/source can come later.

Target budget:
- 4K30 frame interval = 33.33 ms;
- selected production denoise should preferably stay under roughly 8–10 ms p95, leaving room for composition/encoding;
- this is a budget, **not** a prediction for the 1650 Super.

---

# 20. Debug views

Implement early:
- Normal
- Absolute Difference x gain
- Temporal History Difference
- Scene-change metric
- NLMeans effective-neighbor/weight visualization if practical

Debug views must not unexpectedly mutate temporal history differently from normal mode.

---

# 21. Required CPU references

Create:
- HQDN3D temporal reference;
- declared HQDN3D spatial reference;
- NLMeans reference;
- HDR comparison-domain transform.

Use:
- double precision oracle where useful;
- float32 mirror matching shader order.

Fixtures:
- true black;
- near-black;
- SDR white region;
- 1000-nit equivalent;
- >1000-nit HDR values;
- negative RGB components;
- saturated colors;
- alpha;
- constant fields;
- impulses;
- hard edges;
- synthetic noise;
- cut sequence;
- dissolve sequence.

---

# 22. Validation gates

## G0 — runtime load
PASS only if:
- plugin loads;
- denoise source/module registers;
- runtime shaders/compute programs compile/create;
- no relevant `failed to compile` log.

## G1 — once-per-program-frame
Run preview + recording + streaming if available.

PASS:
```text
denoise_dispatches == unique_program_frames
history_updates == unique_program_frames   (temporal mode)
```

No preview/extra consumer double-updates history.

## G2 — HDR identity
Exact-value source -> HDR scope baseline versus identity-denoise.

Test:
- black;
- ~75% HLG;
- 100% HLG;
- >100% HLG/headroom test where synthetic path permits.

PASS:
- no 75%/88% cap;
- no 100% hard clamp;
- no hue shift;
- no alpha shift;
- no extra banding.

## G3 — zero-strength identity
HQDN3D all strengths zero and NLMeans zero:
- same waveform as baseline;
- no history drift.

## H1 — temporal constant
Constant input remains constant over time.

## H2 — temporal numerical fixture
GPU output matches CPU float32 mirror within tolerance.

## H3 — hard cut
A for >=30 frames -> instantaneous B.

PASS:
- first B frame contains no visible A ghost;
- one history reset;
- B initializes history.

## H4 — dissolve
A -> B controlled dissolve.

PASS:
- no reset every frame;
- transition protection visibly reduces lag/trailing;
- no pumping after settle.

## H5 — graphics
Fine text / moving lower third / logo remain acceptable.

## H6 — HQ spatial
GPU result matches declared project reference.
If implementation is approximate, compare to the approximate reference, not FFmpeg.

## N1 — NLM constant
Constant remains constant.

## N2 — NLM impulse
Matches CPU reference.

## N3 — NLM noisy edge
Useful noise reduction without unacceptable cross-edge bleed.

## N4 — HDR stress
Negative components + SDR white + nominal peak + above peak:
- no NaN/Inf;
- no accidental clamp.

## P1 — 4K30 performance
For:
- HQ temporal only
- HQ spatial+temporal
- NLM Light
- NLM Balanced
- NLM Strong

Record:
- average ms;
- p95;
- max;
- OBS rendering lag;
- VRAM estimate;
- visual quality.

Run after warm-up for at least several minutes.

---

# 23. Diagnostic decision tree for range problems

Before changing denoise math:

1. exact-value test pattern -> HDR scope
2. test pattern -> identity denoise -> scope
3. zero-strength algorithm -> scope
4. active denoise -> scope
5. add other filters one by one
6. test decoded camera/file source separately

Do not assume denoise caused a 75%, 88% or 100% ceiling.

---

# 24. UI

## Common
```text
Algorithm: Off / HQDN3D / NLMeans
Mix
Reset History
```

## HQDN3D
```text
Spatial Luma
Spatial Chroma
Temporal Luma
Temporal Chroma
Scene Cut Reset
Cut Sensitivity
Transition Protection
Protection Amount
```

Suggested presets:
- Light
- Balanced
- Strong
- Custom

Preset numeric values must be tuned in this HDR implementation, not copied blindly from FFmpeg.

## NLMeans
```text
Preset: Light / Balanced / Strong / Custom
Patch Size
Search Size
Luma Strength
Chroma Strength
```

## Debug
```text
Difference View
Timing Logging
```

Avoid live property-panel rebuilds.

---

# 25. Processing order relative to HDR Color / HDR Transform

Do not blindly reorder existing user scenes.

General preference:
- camera-specific geometry/correction remains where needed;
- switch/dissolve first;
- denoise final camera/program image once;
- strong program-wide grading ideally after denoise so grade does not amplify noise before filtering;
- final OBS HLG/PQ output encoding last.

If HDR Color is currently camera-specific, leave it camera-specific unless the user chooses otherwise.

The denoiser itself must remain color-management neutral: it preserves the working space.

---

# 26. Development phases

## P0 — reuse proven HDR plumbing
- inspect `OBS-HDRtools`;
- reuse native-space/high-precision patterns;
- obtain final program frame;
- GPU identity pass;
- once-only frame dispatch;
- counters;
- exact-value HLG scope gate;
- runtime shader log gate.

Do not proceed until P0 is transparent.

## P1 — HQDN3D temporal
- persistent history;
- CPU mirror;
- zero strength;
- cut reset;
- transition protection;
- timing.

## P2 — HQDN3D spatial
- faithful GPU spike;
- GPU-friendly approximation spike;
- compare quality/performance;
- choose and document.

## P3 — spatial NLMeans
- Light;
- Balanced;
- Strong;
- luma/chroma;
- bounded-memory tiled implementation;
- CPU mirror;
- 1650 Super benchmarks.

## P4 — polish
- presets;
- debug views;
- graphics protection if needed;
- resource reset robustness;
- packaging.

## P5 — release
- formal runtime gates;
- freeze defaults/schema;
- documentation;
- hardware decision.

---

# 27. Hardware decision rule

Do not require a GPU upgrade before P1–P3 are measured.

If HQDN3D quality is sufficient and runs comfortably on the 1650 Super, keep the card.

If desired NLMeans settings exceed the frame budget, then select a GPU based on:
- measured kernel scaling needs;
- compute/bandwidth;
- enough VRAM headroom.

12 GB+ VRAM is comfortable for development/headroom, not a formal minimum for this design.

---

# 28. HQDN3D clean implementation requirement

FFmpeg/MPlayer HQDN3D may be consulted to understand the published algorithm, expected behavior, parameter concepts, and as an external behavioral/output reference.

**Do not copy, translate, adapt, or port FFmpeg/MPlayer source code into this project.**

Implement the algorithm independently for the OBS HDR/GPU architecture from a written mathematical/behavioral specification. Do not preserve FFmpeg source structure, function structure, lookup-table implementation, comments, variable names, or other source-level expression.

The required workflow is:

```text
Study HQDN3D behavior, documentation and references
        ↓
Write the mathematical/behavioral specification
        ↓
Implement an independent CPU reference from that specification
        ↓
Implement the independent HDR/GPU version
        ↓
Compare behavior/output against references
```

Before writing production HQDN3D code, create `docs/HQDN3D_DESIGN.md` describing:
- the mathematical recurrence actually being implemented;
- how user strength maps to that recurrence;
- the luma/chroma representation;
- the HDR comparison domain;
- temporal-history behavior;
- the independently designed spatial GPU strategy;
- intentional differences from conventional HQDN3D.

The CPU reference implementation must also be independently written rather than ported from `vf_hqdn3d.c`.

FFmpeg output may be used as a black-box comparison target where useful, but exact numerical equivalence is not required unless the independently specified algorithm naturally produces it.

If Claude concludes that a required feature would materially benefit from copying, translating, or adapting GPL-covered FFmpeg/MPlayer source, **stop and ask the user before doing so.**

This clean implementation requirement is both a licensing decision and an engineering decision: the desired OBS implementation operates on high-precision HDR RGB/GPU resources and is structurally different from the conventional CPU/YUV implementation.

---

# 29. Explicit DON'T list

DO NOT:
- denoise every camera by default;
- update temporal history more than once per program frame;
- treat filter input as encoded HLG;
- decode/re-encode HLG inside denoise;
- clamp RGB to 0–1;
- use code 940 as internal ceiling;
- use an 8-bit intermediate;
- CPU-roundtrip NLMeans;
- assume GitHub Actions validates OBS effect shaders;
- mark unrun gates PASS;
- call a materially different algorithm HQDN3D without documenting it;
- copy GPL code casually;
- diagnose every highlight ceiling as a denoise bug;
- spend time on settings migration while denoise tuning is still changing daily.

---

# 30. Exact first instruction for Claude Code

> Read this brief, the existing `OBS-HDRtools` repository, and the existing `obs-color-monitor` HDR/HLG fork before coding. **HQDN3D is to be independently implemented: do not copy, translate, adapt, or port FFmpeg/MPlayer HQDN3D source. Before HQDN3D coding, write the mathematical/behavioral specification in `docs/HQDN3D_DESIGN.md` and implement from that specification.** Reuse the already-proven HDRtools color-space and high-precision rendering conventions rather than creating a new color pipeline. Start only with P0: obtain the completed program frame, create a GPU identity processing path that preserves its native OBS HDR graphics space/precision, and prove using the exact-value test source plus the HLG-aware scope that the frame is processed exactly once and its HDR waveform is unchanged. Add counters for callbacks, unique program frames, denoise dispatches, duplicate skips, history updates and resets. Check runtime shader compilation in the OBS log; a green CI build is not sufficient. Do not implement NLMeans yet. Once P0 passes, implement HQDN3D **temporal-only** with a persistent history texture, hard-cut reset, transition protection, and a float32 CPU mirror. For every deterministic OBS gate, give the user the expected HLG % / 10-bit code reading. Anything not actually executed must be marked NOT RUN.

---

# 31. External technical references

OBS:
- https://github.com/obsproject/obs-studio
- inspect current `libobs/obs-video.c`, `obs.c`, `obs-source.c`, graphics/effect APIs

FFmpeg HQDN3D:
- https://ffmpeg.org/doxygen/trunk/vf__hqdn3d_8c_source.html
- https://github.com/FFmpeg/FFmpeg/blob/master/libavfilter/vf_hqdn3d.h

Key FFmpeg facts to verify against current source:
- temporal path maintains previous filtered frame;
- spatial path is recursive and depends on prior filtered spatial values;
- separate luma/chroma spatial/temporal strengths;
- GPL licensing.

---

# 32. Final acceptance statement

The project is successful only when:

1. A neutral denoise stage is HDR-transparent on the exact-value source.
2. The final program frame is processed exactly once regardless of preview/stream/record consumers.
3. HQDN3D temporal resets cleanly on cuts and behaves acceptably through dissolves.
4. HQDN3D spatial provides a genuinely light denoise option.
5. NLMeans provides visibly stronger detail-preserving denoise with known 4K30 cost.
6. No algorithm introduces a hidden SDR conversion, 0–1 clamp, or 8-bit intermediate.
7. The GTX 1650 Super is benchmarked before any GPU upgrade recommendation is made.
