# P2.5 amendment for Opus — compare Temporal NLMeans and NVOFA motion-compensated temporal

**Date:** 4 Oct 2026  
**Repository:** `srl01-coding/OBS-HDRtools`  
**Status:** amendment to the denoise roadmap already provided.  
**Scope:** this changes only the planned P2.5/premium-temporal investigation. It does **not** change the P1/P2 HQDN3D decisions already sent.

---

# 1. Why this amendment exists

The roadmap already correctly says:

- finish/tune HQDN3D temporal;
- implement HQDN3D spatial;
- prove native D3D11 compute;
- treat the GTX 1650 Super as the baseline rather than the design ceiling;
- allow an RTX 3070-class or similar upgrade if materially better denoising improves the delivered YouTube image;
- evaluate quality after constrained encoding/YouTube, not merely in the OBS preview.

Keep all of that.

The only correction is that **NVOFA motion-compensated temporal should not be presumed to be the premium temporal architecture before we compare it with temporal NLMeans.**

HandBrake is a useful conceptual precedent: its NLMeans supports temporal frame search, demonstrating that patch matching can obtain temporal correspondence without explicit optical flow. We should include that design family in our own experiments, while implementing our own algorithm and not copying HandBrake code.

The premium-temporal decision should therefore compare:

1. simple HQDN3D temporal;
2. causal Temporal NLMeans;
3. NVOFA motion-compensated temporal.

---

# 2. Revised P2.5

Replace the previous single-track P2.5 with:

```text
P2.5 — PREMIUM TEMPORAL DENOISE COMPARISON

P2.5A
    Temporal NLMeans
    causal t-1 patch search
    no lookahead
    shared with the spatial NLMeans engine

P2.5B
    NVOFA motion-compensated temporal
    optical flow
    warped denoised history
    confidence / occlusion rejection

P2.5C
    comparative decision gate
```

Do not interrupt current P1/P2 implementation to do this immediately. This is the next architectural investigation after the D3D11 compute foundation is proven.

---

# 3. Important change to the NLMeans architecture

Do **not** architect NLMeans as a spatial-only engine that later requires a second implementation for temporal search.

Define the candidate set generally as:

```text
candidate = (dx, dy, dt)
```

where:

```text
dt = 0      current frame
dt = -1     previous frame
dt = -2     optional future experiment
```

For the initial live implementation:

```text
allowed dt = {0, -1}
```

No future frame is permitted.

This gives:

```text
Spatial NLMeans
    candidates have dt = 0

Temporal NLMeans
    candidates include dt = -1

Combined NLMeans
    candidates include dt = 0 and dt = -1
```

The same engine should therefore share:

- HDR comparison representation;
- luma/chroma distance;
- patch SSD calculation;
- tiled/shared-memory machinery;
- search-offset processing;
- weight calculation;
- RGB reconstruction/accumulation;
- debug views;
- CPU reference framework.

Do not build separate spatial and temporal NLMeans codebases.

---

# 4. Why Temporal NLMeans belongs in the comparison

Simple temporal HQDN3D effectively asks:

> Is the pixel at this coordinate sufficiently similar to my filtered history?

Temporal NLMeans asks:

> Is there a sufficiently similar patch somewhere near this coordinate in the previous frame?

That spatial search can provide **implicit motion correspondence**.

Conceptually:

```text
Current frame, target x
        ↓
target patch
        ↓
search a small region in t-1
        ↓
find similar previous patches
        ↓
use them as temporal candidates
```

For modest motion, this can follow moving scene structure without an explicit optical-flow field.

Potential advantages over NVOFA:
- no separate optical-flow SDK/backend;
- no explicit warp stage;
- patch similarity and denoising are one process;
- same compute engine as spatial NLMeans;
- naturally rejects poor matches through low weights.

Potential disadvantages:
- expensive search;
- limited motion radius;
- repeated texture/SSD work;
- may smear if multiple patches look similar;
- less explicit treatment of occlusion/disocclusion;
- larger motion requires larger search.

We should measure rather than assume which approach wins.

---

# 5. Causal/no-lookahead requirement

This is a live OBS filter.

Initial Temporal NLMeans may use:

```text
current frame t
previous frame t-1
```

but **not**:

```text
future frame t+1
```

No deliberate one-frame lookahead should be introduced merely to make temporal NLMeans symmetric.

This distinguishes our live design from offline transcoders that can freely use future frames.

If a future-frame mode is ever considered, it must be an explicit later option with its latency cost documented.

---

# 6. What frame should Temporal NLMeans search?

Initial experiment:

```text
current raw/spatial input C_t
        ↓
search previous final denoised history D_(t-1)
```

This potentially provides a cleaner reference than searching the previous noisy input.

However, recursive use of denoised history can also:
- lock texture;
- propagate mistakes;
- create temporal persistence.

Therefore compare two history policies in the CPU/small GPU experiment:

### History policy A
```text
search previous original/program frame C_(t-1)
```

### History policy B
```text
search previous denoised frame D_(t-1)
```

Do not build an elaborate UI for this. Determine which policy behaves better and document the choice.

A hybrid is also permissible:

```text
use previous original for patch-distance decisions
use previous denoised value for reconstruction
```

if tests show a clear advantage.

---

# 7. Initial Temporal NLMeans search design

Do not start with a huge 3D search.

Suggested first experiment:

```text
current-frame spatial candidates:
    modest spatial search

previous-frame candidates:
    modest search around same coordinate
```

For example, use the existing Light/Balanced spatial search sizes as starting points, but measure.

The previous-frame search radius is effectively the maximum implicit motion displacement the algorithm can follow.

At 4K30:
- small subject movement may fit;
- rapid pans/hands may not;
- this is exactly where NVOFA may have an advantage.

Do not enlarge the temporal search until the optimized patch-SSD engine is measured.

---

# 8. NLMeans patch-distance reuse remains mandatory

Do not implement:

```text
for every output pixel
    for every candidate
        reread every pixel of the patch
```

and use that result to judge feasibility.

For each `(dx,dy,dt)` candidate offset, conceptually:

```text
difference image
    D(x) = distance(I_t(x), I_(t+dt)(x + offset))

        ↓

reuse neighboring D values
to calculate patch SSD

        ↓

weight

        ↓

accumulate candidate
```

Use:
- groupshared-memory tiling;
- rolling/separable box sums;
- integral-like reuse;
- or an equivalent efficient compute formulation.

Patch area should not naively multiply global memory reads by patch area.

This applies equally to spatial and temporal candidates.

---

# 9. HDR domain for Temporal NLMeans

Use the same HDR comparison-domain policy already specified for denoise.

Patch matching may use:
- companded luminance;
- normalized chroma;
- other tested comparison representations.

Reconstruction/averaging uses the original high-precision linear HDR values.

Do not:
- compare encoded HLG merely because output is HLG;
- reconstruct in HLG code space;
- clamp to `[0,1]`;
- use 940 as an internal ceiling.

Negative/out-of-gamut working RGB must continue to be handled safely.

---

# 10. Hard cuts

Temporal candidates from the previous shot must be completely invalid after a hard cut.

On cut:

```text
temporal candidate set = empty
history = current frame
```

Spatial candidates may continue normally.

Reuse the cut detection/history reset developed for HQDN3D.

No previous-camera patch is allowed to contribute to the first frame after the cut.

---

# 11. Dissolves

A dissolve creates mixtures of two scenes, so temporal patch matching can find misleading correspondences.

During transition protection:

- reduce or disable temporal NLMeans candidates;
- retain spatial candidates;
- restore temporal candidates as the new scene stabilizes.

Use the same transition-change signal as HQDN3D where practical.

Do not let temporal NLMeans invent a separate unrelated cut/transition detector unless evidence requires it.

---

# 12. P2.5B — NVOFA challenger

Keep the NVOFA feasibility spike from the previous roadmap concept, but change its status:

> **NVOFA is a premium-temporal candidate, not the presumed winner.**

The experiment remains:

```text
current frame
        +
previous denoised history
        +
NVOFA optical flow
        ↓
warp history into current coordinates
        ↓
confidence / difference / occlusion rejection
        ↓
temporal accumulation
```

NVIDIA's Optical Flow Accelerator is attractive because supported Turing/Ampere/Ada GPUs expose dedicated optical-flow hardware and a DirectX 11 API, potentially leaving normal shader/compute resources available for denoise.

References:
- https://developer.nvidia.com/optical-flow-sdk
- https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note/
- https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-programming-guide/

At runtime, query capabilities. Do not assume the current card supports every grid size/feature.

---

# 13. Why both candidates are worth testing

## Temporal NLMeans

Correspondence mechanism:
```text
patch search
```

Strength:
- denoising and correspondence are unified.

Weakness:
- search cost rises with motion radius.

## NVOFA temporal

Correspondence mechanism:
```text
explicit optical flow
```

Strength:
- can follow larger/coherent motion without searching every displacement in the denoise kernel.

Weakness:
- flow errors, occlusions, SDK/backend complexity.

There is no reason to prejudge the winner.

---

# 14. Shared comparison against HQDN3D

HQDN3D temporal remains the baseline.

The P2.5 decision table should eventually contain:

| Mode | Temporal correspondence | GPU cost | Static denoise | Motion detail | Ghosting | Encoded/YouTube result |
|---|---|---:|---:|---:|---:|---|
| HQDN3D temporal | same coordinate | | | | | |
| Temporal NLMeans | local patch search | | | | | |
| NVOFA temporal | optical flow + warp | | | | | |

Compare at matched visual/detail conditions, not arbitrary slider values.

---

# 15. Quality fixtures

## Static noisy field

All temporal methods should substantially reduce changing noise.

Measure:
- temporal standard deviation;
- spatial residual;
- texture preservation.

## Translating textured patch

Move a textured object by known pixels/frame.

Temporal NLMeans:
- should follow it while displacement is inside search radius.

NVOFA:
- should follow it if flow is correct.

HQDN3D:
- expected to reduce temporal contribution as same-coordinate difference rises.

Measure:
- retained texture;
- trailing energy;
- residual noise.

## Increasing motion speed

Sweep displacement:

```text
0, 1, 2, 4, 8, 16 ... pixels/frame
```

This is particularly useful.

It should reveal:
- where temporal NLMeans exhausts its search radius;
- where NVOFA becomes advantageous;
- where both should reject history.

## Disocclusion

Reveal previously hidden background.

Temporal methods must reject stale foreground history.

## Pan

Controlled pan tests camera-motion behavior.

## Hard cut and dissolve

Use the established program-transition gates.

---

# 16. Codec/YouTube comparison

The user's ultimate objective remains downstream clarity.

For representative sequences compare:

```text
No denoise
HQDN3D
Temporal NLMeans
NVOFA temporal
best temporal + light spatial cleanup
```

Use identical constrained HEVC Main10 encode settings.

Where practical, test actual private/unlisted YouTube output under identical ingest conditions.

Judge:
- faces;
- hair;
- fabric;
- text;
- background texture;
- motion;
- gradients;
- mosquito noise;
- blockiness;
- temporal smear;
- retained meaningful detail.

A method that wins in the raw OBS preview but loses after encoding is not the production winner.

---

# 17. Performance policy

Continue using the 1650 Super as baseline.

Do not reject a high-quality candidate solely because it needs an RTX 3070-class upgrade.

For each temporal method separately record:

```text
correspondence cost
denoise/accumulation cost
total incremental p95
VRAM
```

For Temporal NLMeans, also report cost by:
- search radius;
- patch size;
- number of temporal frames/candidate offsets.

For NVOFA, separate:
- flow acquisition;
- warp;
- confidence;
- accumulation.

This tells us which method will scale with a GPU upgrade.

---

# 18. NLMeans product architecture

If Temporal NLMeans works well, expose NLMeans conceptually as one algorithm with temporal depth rather than separate unrelated algorithms.

Possible eventual UI:

```text
NLMeans

Spatial Search:
    Off / Light / Balanced / High

Temporal Search:
    Off / Previous Frame

Temporal Radius:
    ...

Luma Strength
Chroma Strength
Mix
```

Do not expose all experimental parameters until defaults are understood.

Internally, keep `(dx,dy,dt)` general enough that future `dt=-2` experimentation does not require redesign, but do not enable it initially.

---

# 19. Revised development sequence

Keep current work:

```text
P1
    HQDN3D temporal runtime validation/tuning

P2
    HQDN3D portable spatial B
    D3D11 compute foundation
    recursive spatial A
```

Then:

```text
P2.5A
    generalized NLMeans candidate architecture
    optimized spatial patch SSD
    causal dt=-1 temporal candidates
    Temporal NLMeans prototype

P2.5B
    NVOFA capability probe
    optical-flow visualization
    history warp
    confidence/disocclusion rejection
    motion-aware temporal prototype

P2.5C
    HQDN3D vs Temporal NLMeans vs NVOFA comparison
    same-bitrate encode / YouTube evaluation
```

Then:

```text
P3
    optimize the premium architecture(s)
    retain spatial NLMeans where it adds value
```

Do not commit extensive effort to a very large spatial-only NLMeans mode before the P2.5 comparison.

---

# 20. What this amendment does NOT change

It does not change:

- final-program-only denoise;
- once-per-unique-frame requirement;
- HLG/HDR working-space policy;
- P1 HQDN3D temporal;
- P2 B + D3D11 compute + recursive A decision;
- bidirectional recursive spatial decision;
- spatial-before-temporal for HQDN3D;
- clean independent HQDN3D implementation requirement;
- 1650 Super baseline / 3070-class upgrade willingness;
- YouTube post-encode quality objective;
- formal PASS/FAIL/NOT RUN discipline.

---

# 21. Immediate instruction to Opus

> Continue the P1/P2 work already planned. Amend the future P2.5 roadmap so NVOFA motion-compensated temporal is **one challenger**, not the presumed premium solution. Before deep spatial-only NLMeans optimization, design the NLMeans engine around generalized candidates `(dx,dy,dt)` and prototype causal `dt=-1` Temporal NLMeans using the same optimized patch-SSD/tiling machinery as spatial NLMeans. No future-frame lookahead. Then compare HQDN3D temporal, Temporal NLMeans and NVOFA motion-compensated temporal on synthetic motion/noise, real Sony footage, GPU cost, and identical constrained HEVC/YouTube output. Let those measurements determine the premium architecture.

---

# 22. Decision in one sentence

> **Keep the roadmap already sent, but broaden P2.5 from an NVOFA feasibility phase into a premium-temporal comparison: HQDN3D is the cheap baseline, causal Temporal NLMeans is the patch-correspondence candidate, and NVOFA is the explicit-motion candidate; choose among them based on real motion/detail preservation, GPU cost and downstream YouTube quality.**
