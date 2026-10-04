# Roadmap correction for Opus — add motion-aware temporal denoising before deep NLMeans work

**Date:** 4 Oct 2026  
**Repository:** `srl01-coding/OBS-HDRtools`  
**Status:** design correction / addendum

## 1. Executive decision

Do **not** discard the current HQDN3D work. Revise the roadmap to:

```text
P0  final-program HDR-safe plumbing
P1  HQDN3D temporal
P2  HQDN3D spatial + D3D11 compute infrastructure
P2.5  motion-aware temporal feasibility spike
P3  optimized spatial NLMeans
P4  compare/combine the best temporal + spatial methods
```

The reason is strategic:

> For a live 4K stream whose downstream quality is constrained by YouTube's inter-frame compression/transcode, the premium denoising path should probably be **motion-aware temporal denoising**, not simply a heavier spatial filter.

HQDN3D remains the inexpensive baseline. NLMeans remains valuable as a strong spatial reference/residual cleaner. But **before investing heavily in full-resolution NLMeans, test whether motion-aware temporal accumulation gives a materially better quality-per-GPU-ms result.**

---

## 2. Why the roadmap changes

The user's actual objective is to give YouTube a cleaner, more temporally stable source so constrained bitrate is spent on meaningful image structure rather than stochastic sensor noise, without destroying real texture.

A purely spatial denoiser makes individual frames cleaner but does not exploit the strongest redundancy in video: the same real scene content persists across frames.

Simple temporal IIR has a motion problem:

```text
current frame + previous history
        -> strong denoise in static areas
        -> ghosting/trails when objects or camera move
```

The modern extension is:

```text
current frame
    + previous clean frame
    + motion estimate
        -> warp history into current coordinates
        -> confidence / disocclusion / difference test
        -> temporally accumulate where safe
        -> light spatial cleanup for residual noise
```

This architecture should be tested before deciding that large spatial NLMeans is the premium mode.

---

## 3. External evidence supporting this direction

### NVIDIA Optical Flow Accelerator (NVOFA)

NVIDIA GPUs from the Turing generation onward contain a dedicated Optical Flow Accelerator. NVIDIA documents that this engine works independently of graphics/CUDA cores, leaving those resources available for other workloads.

The Optical Flow SDK exposes DirectX 11 on Windows, as well as DirectX 12, CUDA and Vulkan on supported configurations. NVIDIA documents 4x4 flow grids on Turing and finer 2x2/1x1 grids on Ampere/Ada. Runtime capability must be queried rather than assumed.

References:
- https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note/
- https://developer.nvidia.com/optical-flow-sdk

NVIDIA advertises up to 150 fps at 4K depending on hardware/preset/clock. Treat that as a vendor capability statement, **not a prediction for this plugin or this GPU**.

### NVIDIA's own temporal-noise-reduction architecture is relevant

NVIDIA VPI's TNR documentation describes implementations using bilateral spatial filtering and/or temporal IIR filtering with motion detection, and explicitly notes the tradeoff between stronger denoise and ghosting/detail loss around motion.

Reference:
- https://docs.nvidia.com/vpi/algo_tnr.html

We are not proposing to copy or necessarily integrate VPI. It is useful independent evidence that **motion-aware temporal + light spatial filtering is a sensible video-denoising architecture**.

---

## 4. Hardware implications

The user currently has a GTX 1650 Super and is willing to upgrade to roughly an RTX 3070-class GPU or similar if the result warrants it.

Do not assume NVOFA support from the marketing name alone. At runtime:

1. load/query the NVIDIA Optical Flow API;
2. confirm the installed GPU exposes the required DirectX 11 optical-flow interface;
3. report supported grid sizes/features;
4. if unavailable, skip this path cleanly.

A 3070-class Ampere GPU is especially interesting because NVIDIA's capability table exposes finer optical-flow grids on Ampere than on Turing.

Architecture:

```text
Generic GPU path:
    HQDN3D
    spatial filters
    fallback path

NVIDIA NVOFA path:
    hardware optical flow
    motion-compensated temporal accumulation
```

Do not make the entire denoiser NVIDIA-only. NVOFA is an optional premium backend.

---

## 5. Do not replace HQDN3D temporal

Complete and tune HQDN3D temporal first. It provides:

- extremely cheap temporal denoise;
- proven history plumbing;
- cut reset;
- transition protection;
- baseline quality;
- a reference against which motion compensation can prove its value.

The later comparison is:

```text
HQDN3D temporal
vs
motion-aware temporal at equal/no-greater GPU budget
```

If motion compensation produces no meaningful quality advantage, stop there.

---

## 6. P2.5 — motion-aware temporal feasibility spike

Insert P2.5 **after D3D11 compute plumbing is proven and after P1 has run in OBS, but before deep NLMeans optimization**.

### P2.5A — optical-flow integration

Goal: obtain current-to-previous motion vectors for the final program image.

Preferred NVIDIA experiment:

- use NVOF DirectX 11 API;
- keep resources GPU-resident;
- avoid CPU image readback;
- keep flow state/resources persistent;
- query capability at runtime;
- test 4x4 vectors on Turing-class hardware;
- test finer grids where available on Ampere+.

Do not initially write a custom optical-flow algorithm.

Questions to answer:

1. Can NVOF consume resources derived from the final-program texture without CPU readback?
2. What conversion/preprocessing is required?
3. What is incremental latency at 4K30?
4. Does NVOF coexist safely with OBS's D3D11 device/context?
5. Can forward and/or backward flow fit the budget?
6. What happens on hard cuts and dissolves?
7. Is flow quality adequate for faces, hair, hands and camera movement?
8. How limiting is 4x4 flow on the current GPU?
9. What capabilities are exposed by the actual installed GPU/driver?

Do not proceed to a production motion-aware denoiser until these are measured.

---

## 7. P2.5B — motion-compensated temporal accumulation

With current frame `C_t`, previous denoised frame `D_(t-1)`, and a flow field mapping history into current coordinates:

```text
H_t(x) = sample(D_(t-1), x + flow(x))
```

where `H_t` is motion-compensated history.

Derive confidence `q(x)` from some combination of:

- current/history luminance difference in the established HDR comparison domain;
- optical-flow cost/confidence if exposed;
- forward/backward consistency where available;
- motion magnitude;
- local gradients/edges;
- disocclusion tests.

Then:

```text
alpha(x) = temporal_strength * q(x)
D_t(x) = lerp(C_t(x), H_t(x), alpha(x))
```

Requirements:

- alpha = 0 on hard reset;
- strong rejection where warped history disagrees;
- optional luma/chroma-specific alpha later;
- cap history contribution below 1.0 so pixels cannot freeze indefinitely.

Keep the first implementation simple. Do not add neural models or elaborate motion pyramids yet.

---

## 8. Motion-aware safeguards

### Hard cuts

Optical flow between unrelated scenes is meaningless. On a cut:

```text
D_t = C_t
history = C_t
```

Cut detection remains mandatory and occurs before temporal blending.

### Dissolves

A dissolve is not a physically coherent motion field.

During a dissolve:

- strongly reduce temporal accumulation;
- optionally disable motion compensation for the transition;
- restore once the new scene stabilizes.

Reuse/extend P1 transition protection.

### Occlusion/disocclusion

Reduce confidence to zero for:

- newly revealed areas;
- inconsistent forward/backward flow;
- large warped-history/current mismatch.

### Fine texture

Do not allow history to lock skin texture, hair, fabric or text. Continue using the removed-signal view as a primary diagnostic.

---

## 9. HDR handling

Motion estimation and HDR reconstruction are separate concerns.

The denoiser continues to reconstruct/output in OBS's high-precision linear HDR working representation.

Do not:

- decode HLG;
- re-encode HLG;
- clamp to `[0,1]`;
- treat code 940 as a ceiling.

If NVOF requires a normalized or different input representation, create a **derived motion-estimation image** while preserving the original HDR frame for reconstruction:

```text
OBS RGBA16F HDR
   |-- compact/normalized luminance -> optical flow
   `-- original HDR RGB -----------> motion-compensated reconstruction
```

The motion-estimation representation must never become the output image domain.

---

## 10. Why this may beat heavy spatial NLMeans

Spatial NLMeans asks:

> Which nearby pixels in this frame are similar enough to average?

Motion-aware temporal asks:

> Where did the same scene content move between frames, and can we safely average that actual content over time?

For video, the latter can be much more powerful.

Potential advantages:

- stronger denoise without broad spatial blur;
- better preservation of thin edges;
- large gains in static/slow-moving areas;
- lower residual temporal noise;
- potentially better compressibility;
- allows lighter residual spatial filtering.

Potential failures:

- inaccurate flow;
- disocclusion;
- ghosting;
- coarse motion grids;
- transition handling.

That is why P2.5 is a feasibility phase rather than a commitment.

---

## 11. Revised roles of the algorithms

### HQDN3D temporal

Role: cheapest temporal baseline and fallback.

### HQDN3D / HQDN3D-style spatial

Role: low-cost residual spatial cleanup and portable fallback.

### Motion-aware temporal

Role: candidate **premium video denoiser** and likely best match to the codec-quality objective.

### NLMeans spatial

Role: high-quality spatial reference/residual cleaner. It may be most useful **after** motion-aware temporal at lower strength/search than originally envisioned.

Possible eventual chain:

```text
final program
    -> motion-aware temporal
    -> light HQDN3D/NLMeans residual spatial cleanup
    -> encoder
```

rather than automatically:

```text
final program
    -> heavy spatial NLMeans
    -> encoder
```

Do not assume either until measured.

---

## 12. Revised P3 decision gate

Before substantial engineering effort on large full-resolution NLMeans, compare:

- Candidate A: HQDN3D temporal + best P2 spatial
- Candidate B: motion-aware temporal only
- Candidate C: motion-aware temporal + light P2 spatial
- Candidate D: optimized spatial NLMeans

At matched downstream encode bitrate, compare:

- residual noise;
- edge/detail retention;
- skin/hair/fabric texture;
- temporal stability;
- ghosting;
- encoded result;
- actual YouTube result where practical;
- GPU time.

Only then decide how far to push NLMeans.

---

## 13. NVOFA implementation direction

Use NVIDIA's Optical Flow API rather than writing CUDA optical-flow kernels.

Prefer DirectX 11 on this Windows/OBS system because NVOF exposes a D3D11 interface and OBS already uses D3D11.

References:
- https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-programming-guide/
- https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note/

Follow the current SDK's runtime-loading and resource guidance. Gracefully fall back when API/driver/GPU support is unavailable.

---

## 14. Dependency/licensing policy

NVIDIA states that the Optical Flow SDK is free to use within applications, subject to its SDK license.

Reference:
- https://developer.nvidia.com/opticalflow/download

Before distributing binaries that depend on it:

- document the dependency;
- document driver/hardware requirements;
- comply with NVIDIA's license terms;
- do not redistribute SDK components unless allowed.

This is separate from the FFmpeg GPL discussion.

---

## 15. Performance gates for P2.5

Measure separately:

```text
optical-flow acquisition
history warp
confidence calculation
temporal accumulation
```

On the 1650 Super classify the total incremental motion-aware temporal cost as:

### Excellent

```text
<= 4 ms p95
```

### Useful

```text
4–8 ms p95
```

### Upgrade-target

```text
> 8 ms p95
```

Do not reject an >8 ms mode if:

- quality improvement is large;
- most cost should scale well on Ampere/3070-class hardware;
- NVOFA itself is not the pathological bottleneck.

The user explicitly accepts a GPU upgrade if it yields meaningfully better YouTube output.

---

## 16. Quality acceptance for motion-aware temporal

Use the existing clean-reference/noisy-reference framework.

### Static region

At matched detail retention, motion-aware temporal should remove more temporal noise than simple HQDN3D temporal, or achieve equivalent denoise with less texture loss.

### Moving edge

Use deterministic moving objects/edges. Measure:

- trailing energy;
- edge width;
- position error;
- residual noise.

No visible multi-frame ghost trail at normal viewing speed.

### Disocclusion

Reveal previously hidden background. Newly revealed pixels must use current-frame content immediately rather than old foreground history.

### Pan

Use a controlled pan. Motion-aware accumulation should preserve useful temporal denoise through the pan where simple IIR must reduce strength significantly.

### Real footage

Test:

- faces;
- moving hands;
- hair;
- clothing;
- camera pan/tilt;
- static backgrounds;
- dissolves;
- hard cuts.

Use both:

```text
removed = input - output
```

and a temporal/ghost diagnostic view.

---

## 17. Codec/YouTube acceptance remains primary

Compare under identical constrained HEVC Main10 conditions:

```text
No denoise
HQDN3D
Motion-aware temporal
NLMeans
Motion-aware + light spatial
```

Where practical, run private/unlisted YouTube comparisons with identical ingest conditions.

Judge the decoded viewer result, not merely the OBS preview.

Success is **not** "least grain before encoding". Success is "most meaningful detail and least objectionable artifacts after constrained delivery."

YouTube Live HDR currently requires HEVC/H.265 and 10-bit HDR, supporting HLG/PQ.

References:
- https://support.google.com/youtube/answer/10265272
- https://support.google.com/youtube/answer/2853702

---

## 18. Do not add ML or BM3D yet

Do not add a neural denoiser in this phase. Model/runtime/HDR complexity is too high before classical motion-aware temporal has been tested.

Do not prioritize BM3D before P2.5. BM3D is a credible spatial benchmark but is less directly aligned with temporal stability and codec efficiency.

---

## 19. Updated sequence

```text
P1
  HQDN3D temporal runtime validation/tuning

P2
  portable spatial B
  D3D11 compute spike
  recursive spatial A if safe

P2.5
  NVOF capability probe
  D3D11 NVOF integration
  flow visualization
  motion-compensated history warp
  confidence/disocclusion handling
  simple temporal accumulation
  benchmark + real-footage tests

Decision Gate
  compare HQDN3D vs motion-aware vs light spatial combinations

P3
  optimized NLMeans
  scope/depth determined by Decision Gate

P4
  tune combined production modes
  codec/YouTube A/B
```

---

## 20. Immediate instructions to Opus

1. Do **not** interrupt P1/P2 work already underway.
2. Add this roadmap correction to the documentation now.
3. Create `docs/denoise/MOTION_AWARE_TEMPORAL.md` as a design placeholder.
4. During D3D11 compute work, keep resource/state abstractions reusable for P2.5.
5. Before deep P3 NLMeans implementation, perform the P2.5 NVOF capability/integration spike.
6. Query NVOF capability at runtime rather than assuming support.
7. If NVOF is unavailable, report that and retain HQDN3D/NLMeans paths.
8. Do not write a custom optical-flow algorithm unless the hardware/API route fails and a later decision explicitly authorizes it.
9. Do not use the motion-estimation representation as the HDR reconstruction domain.
10. Report measurements and visual findings before choosing the premium production algorithm.

---

## 21. One-sentence decision

> **Keep HQDN3D as the cheap baseline and NLMeans as the spatial high-quality candidate, but insert a hardware-assisted motion-aware temporal denoising feasibility phase before deep NLMeans optimization, because that approach is more directly aligned with live-video quality and the goal of improving YouTube's use of limited bitrate.**
