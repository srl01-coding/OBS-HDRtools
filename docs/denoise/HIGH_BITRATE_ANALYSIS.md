# External analysis readout — high-bitrate source clip

**Purpose:** provide Opus with the results of the instructing AI's independent offline analysis so the implementation agent does **not** need the source video or duplicate this compute.

**Primary source analyzed:** `2026-10-05 09-08-53.mkv`

This supersedes the earlier ~20 Mbps recording for **real-footage noise characterization**. The earlier synthetic/oracle-flow experiments remain useful; this note only replaces conclusions that depended on measuring the compressed 20 Mbps source.

## 1. Source characteristics

The primary calibration clip was recorded from OBS using NVENC HEVC at approximately CQP 16/17 with no intentional scaling.

Observed file characteristics:

| Property | Value |
|---|---:|
| Resolution | 3840 × 2142 |
| Frame rate | 29.97 fps |
| Codec | HEVC |
| Pixel format | 10-bit |
| Color | BT.2020 / HLG |
| Duration | ~3.70 s |
| Actual average bitrate | **~266 Mbps** |
| File size | ~123 MB |

This is still an encoded recording, so it is **not a mathematically lossless camera-noise reference**. However, it is much more suitable for estimating the pre-stream denoising problem than the earlier ~20 Mbps recording.

## 2. Measured temporal variation/noise

Relatively static regions were measured over approximately 100 frames.

| Region | Approx. 10-bit luma level | Temporal σ | Relative variation above black |
|---|---:|---:|---:|
| Floor / bright-mid | 598 | **1.41 codes** | **0.26%** |
| Wall | 544 | **1.80 codes** | **0.37%** |
| Piano / dark-mid | 442 | **2.16 codes** | **0.57%** |
| Very dark TV area | 184 | **2.57 codes** | **2.14%** |

The important finding is the trend, not false precision in the individual ROI numbers:

> **Relative temporal noise/variation rises dramatically toward black.**

The very dark region showed roughly an order-of-magnitude greater relative variation than the bright-mid region.

This validates the architectural need for a luminance-dependent temporal threshold/strength function:

```text
T = T(Y)
```

rather than one globally fixed threshold.

**Do not freeze a particular `T(Y)` formula from these four ROIs.** The implementation should support luminance adaptation now; the production curve should remain tunable.

## 3. Temporal correlation differs strongly from the 20 Mbps clip

Approximate lag-1 temporal correlations in relatively static regions of the high-bitrate clip were:

| Region | Approx. lag-1 correlation |
|---|---:|
| Wall | **0.14** |
| Very dark TV | **0.10** |
| Piano | **0.10** |
| Floor | **0.27** |

These are dramatically below the correlations reported from the earlier ~20 Mbps source.

Therefore:

> **The earlier 20 Mbps recording was substantially characterizing HEVC residue/refresh behavior rather than the noise presented to a pre-encode OBS denoiser.**

Do not use the 20 Mbps-derived noise model to calibrate production denoise thresholds.

The high-bitrate sample contains substantially more temporally stochastic variation of the kind a pre-encode temporal denoiser should be able to exploit.

## 4. Empirical algorithm-family sanity check

I also ran conventional offline HQDN3D and NLMeans processing against static material from the high-bitrate source.

These tests are **not implementations of our proposed HDR algorithms**, and their numerical strength settings should **not** be transferred into OBS-HDRtools.

They were used only to test the broad algorithmic hypothesis:

> Does temporal filtering appear better suited to this material than relatively aggressive spatial patch filtering?

On the wall region:

| Treatment | Measured temporal σ | Reduction vs untreated | Spatial detail/edge proxy retained |
|---|---:|---:|---:|
| Untreated | **1.80** | — | **100%** |
| conventional HQDN3D test | **0.63** | **~65%** | **~80%** |
| conventional NLMeans test | **0.94** | **~48%** | **~51%** |

The exact percentages depend on the chosen offline settings and the detail proxy is deliberately crude.

The robust qualitative result was:

> **The temporal-oriented HQDN3D test removed substantially more changing noise while retaining substantially more stable fine structure than the tested NLMeans setting.**

Visual inspection was consistent with the metric: NLMeans was considerably more willing to erase subtle stable wall texture.

This does **not** prove that all NLMeans configurations are inferior. It does support lowering the priority of **heavy spatial NLMeans as the primary denoiser**.

## 5. Interpretation for the production architecture

The high-bitrate source supports the following model:

```text
stable real scene texture
        +
temporally changing camera/noise component
```

This strongly favors exploiting temporal redundancy before applying substantial spatial smoothing.

Recommended priority is therefore:

```text
1. HQDN3D-style temporal baseline

2. Motion-compensated temporal
   using NVOFA where available

3. Luminance/noise-adaptive temporal confidence

4. Light residual spatial cleanup

5. Spatial NLMeans only where residual spatial noise
   demonstrates that it adds value

6. Temporal NLMeans retained as research/fallback,
   not currently a GPU implementation priority
```

## 6. `T(Y)` decision

**Architectural decision: YES.**

The denoise engine should support a luminance-dependent threshold:

```text
T = T(Y)
```

for at least temporal luma processing and preferably through a shared noise-profile abstraction usable by HQDN3D, motion-compensated temporal and NLMeans.

**Calibration decision: NOT YET.**

Do not simply adopt:

```text
T(Y) = T0 * sigma_rel(Y) / sigma_rel(ref)
```

as the production law.

Reasons:

1. Four measured regions are insufficient to establish the optimal function.
2. The source remains HEVC encoded.
3. Human sensitivity and preservation of dark detail matter in addition to measured noise amplitude.
4. An unconstrained inverse-luminance relationship could become excessively aggressive near black.

The implementation should permit a bounded curve.

A reasonable implementation abstraction is:

```text
noise_profile(Y) -> threshold_multiplier
```

with:
- interpolation between luminance anchors;
- a configurable maximum multiplier;
- identity/default profile available.

Do not expose all anchors to normal users initially.

## 7. Dark-region result

The dark-region result deserves specific attention.

Measured relative variation rose to approximately **2.14%** in the darkest sampled area, compared with approximately **0.26–0.37%** in brighter regions.

This explains why a temporal threshold tuned around normal mids can fail to touch visible dark noise.

However:

> Do **not** interpret this as permission simply to denoise shadows 6–8× harder.

The correct behavior is a bounded noise-aware threshold/confidence adjustment that still protects legitimate low-light structure.

The dark-region behavior should receive dedicated runtime tests once the OBS implementation exists.

## 8. Implication for HQDN3D

HQDN3D temporal remains highly relevant.

Its likely role is no longer merely:

```text
cheap denoise mode
```

The more interesting architecture is to reuse its nonlinear difference response as the **history acceptance/confidence function** for both simple and motion-compensated temporal accumulation.

Conceptually:

```text
Current pixel
       │
       ├──── compare with same-coordinate history
       │
       └──── compare with motion-warped history
                         ↓
                nonlinear HQ response
                         ↓
                 confidence / weight
                         ↓
                 temporal accumulation
```

Thus NVOFA need not replace the useful HQDN3D logic. It can improve **where history is sampled**, while the HQ response still decides **whether that history is trustworthy**.

## 9. NVOFA implication

The high-bitrate static clip cannot test optical-flow quality because there is insufficient meaningful motion.

Therefore it does **not** prove NVOFA is better than simple HQ temporal.

However, combined with the earlier synthetic/oracle-flow work, the rationale is stronger:

- static content: same-coordinate temporal is already excellent;
- motion: oracle correspondence gave substantially better results;
- high-bitrate real footage: substantial temporally changing noise remains available to exploit.

The premium temporal architecture should therefore test at least two history hypotheses:

```text
H0(x) = previous denoised frame at x

Hf(x) = previous denoised frame at x + flow(x)
```

Evaluate both against current `C(x)` using the established HDR comparison domain.

Prefer/select the more credible hypothesis rather than blindly trusting optical flow.

Conceptually:

```text
best_history =
    argmin distance(current, H0 or Hf)
```

with flow cost/confidence also available to reduce trust.

This should improve robustness to small flow errors in static areas.

## 10. Motion-estimation representation

When NVOFA is implemented, do not assume the optical-flow estimator should consume the same HDR values used for reconstruction.

Recommended architecture:

```text
RGBA16F HDR program
       │
       ├───────────────> original HDR reconstruction values
       │
       ↓
linear luminance
       ↓
bounded/companded OF representation
       ↓
optional very light prefilter
       ↓
NVOFA
```

The motion-estimation image exists only to improve correspondence.

The actual temporal reconstruction continues to use the original high-precision HDR RGB.

This also prevents the optical-flow algorithm from having to deal directly with the full HDR numerical range merely to estimate motion.

## 11. Spatial NLMeans implication

Do not delete NLMeans.

But based on this source:

> **Heavy spatial NLMeans should not currently be treated as the presumed premium denoiser.**

Its likely role is:

```text
light residual spatial cleanup
```

after temporal processing, if real residual noise demonstrates a need.

The generalized `(dx,dy,dt)` CPU research engine should remain.

A GPU temporal-NLMeans implementation remains deferred unless:
- NVOFA is unavailable/unusable; or
- later pre-encode/motion testing reveals a clear quality case.

## 12. Codec objective

The core codec hypothesis remains:

> Removing temporally stochastic camera noise before HEVC should improve compressibility and allow a constrained downstream encode/transcode to devote more bits to stable meaningful image structure.

The high-bitrate clip strengthens the premise because it shows a substantial stochastic temporal component survives before the stream encode.

Do not optimize merely for the lowest measured noise.

Production evaluation must continue to consider:
- stable texture retention;
- faces;
- hair;
- fabric;
- text;
- gradients;
- motion;
- temporal artifacts;
- same-bitrate HEVC result;
- actual YouTube result where practical.

## 13. What these tests do NOT establish

The high-bitrate clip is short and mostly static.

It cannot establish:

- real NVOFA flow quality;
- ghosting on a moving face;
- moving hands;
- hair movement;
- camera pans;
- disocclusion behavior;
- transition behavior;
- whether 4x4 Turing flow is sufficient;
- GPU cost;
- OBS integration correctness.

Those remain runtime/motion-sample questions.

Do not mark any corresponding gate PASS from this analysis.

## 14. Current decisions for Opus

### Decision A — Temporal NLMeans GPU prototype

**Deprioritize.**

Keep:
- generalized CPU `(dx,dy,dt)` engine;
- design documentation;
- tests.

Revisit if:
- NVOFA is unavailable/poor; or
- future evidence materially favors temporal patch search.

### Decision B — NVOFA

**Proceed after NV-G0 confirms hardware/API support.**

Prefer native D3D11 NVOFA if practical.

### Decision C — luminance-dependent noise threshold

**Implement the abstraction now.**

Support:

```text
T = T(Y)
```

Do not freeze the production curve yet.

### Decision D — HQDN3D

**Continue P1.**

Treat its nonlinear temporal response as a potentially reusable confidence mechanism for motion-compensated history.

### Decision E — spatial NLMeans

**Lower priority.**

Retain as residual spatial option/research reference.

## 15. Data Opus should use from this analysis

For documentation/testing context, use these measured high-bitrate values:

```text
Source:
3840x2142
29.97 fps
10-bit HEVC
BT.2020 / HLG
~266 Mbps

Approximate temporal variation:

Floor / bright-mid:
    code ~598
    sigma ~1.41 codes
    relative ~0.26%

Wall:
    code ~544
    sigma ~1.80
    relative ~0.37%

Piano / dark-mid:
    code ~442
    sigma ~2.16
    relative ~0.57%

Very dark TV:
    code ~184
    sigma ~2.57
    relative ~2.14%

Approximate lag-1 temporal correlation:

Floor:       ~0.27
Wall:        ~0.14
Piano:       ~0.10
Dark TV:     ~0.10
```

Treat these as **measured properties of this encoded test clip**, not universal Sony-camera specifications.

## 16. Empirical algorithm sanity-check data

For the tested static wall region:

```text
Untreated:
    temporal sigma = 1.80
    detail proxy = 100%

Conventional HQDN3D test:
    temporal sigma = 0.63
    reduction ≈ 65%
    detail proxy retained ≈ 80%

Conventional NLMeans test:
    temporal sigma = 0.94
    reduction ≈ 48%
    detail proxy retained ≈ 51%
```

Again:

> These values compare particular offline configurations and should **not** be used to map UI strength settings into OBS-HDRtools.

Use them only as evidence that this footage currently rewards temporal processing more than aggressive spatial patch averaging.

## 17. Recommended immediate implementation order

```text
1. Complete/run P1 HQ temporal
        ↓
2. Add T(Y) / noise-profile abstraction
   without freezing its final curve
        ↓
3. Complete D3D11 compute safety work
        ↓
4. Run NVOF capability probe
        ↓
5. Implement NVOFA history warp
        ↓
6. Compare same-position vs warped-history confidence
        ↓
7. Add light spatial cleanup only as needed
        ↓
8. Revisit NLMeans if residual noise warrants it
```

No further source-video analysis by Opus is required to proceed with those engineering tasks.

## 18. Next analytical need

The next source sample that would materially improve algorithm selection is **motion footage**, not another static higher-bitrate sample.

Useful content would include:
- face/head motion;
- moving hands;
- hair/clothing detail;
- modest camera pan;
- dark areas;
- one cut or dissolve.

Prefer the same high-quality CQP recording path.

Until that exists, static-noise analysis is sufficient to proceed with the implementation decisions above.
