# Decision response to Opus --- HQDN3D P2, compute architecture, NLMeans and YouTube quality objective

**Date:** 4 Oct 2026\
**Repository:** `srl01-coding/OBS-HDRtools`\
**Status:** design decision. Use this to update `docs/HQDN3D_DESIGN.md`
and the denoise plan.

## 1. Executive decision

Accept **Option D**, with these refinements:

1.  Build **B first** as the portable/cross-platform spatial
    implementation and fallback.
2.  Build the **native D3D11 compute infrastructure in P2 as a strategic
    spike**. P3 full-resolution NLMeans is likely to need it.
3.  If the compute spike passes OBS-state, HDR and performance gates,
    build **A as the preferred Windows recursive spatial
    implementation**.
4.  Do **not** build C now. Keep bounded-window recursion only as a
    reserve if compute fails and B is visibly inadequate.
5.  Do not preserve classic one-directional causality. The production
    recursive implementation should be **bidirectional**.
6.  Separable horizontal then vertical processing is acceptable. Do not
    pursue the fused left+upper wavefront.
7.  Use **spatial before temporal**.
8.  Continue P2 engineering now, but do not freeze P2 strengths or
    choose A versus B until P1 has actually run on real footage.
9.  The GTX 1650 Super is a **baseline benchmark, not the design
    ceiling**. The user is willing to upgrade to an RTX 3070-class GPU
    or similar if materially better denoising improves the delivered
    YouTube image.
10. Therefore, do not discard a superior scalable algorithm merely
    because its highest-quality mode misses 8--10 ms on the 1650 Super.

------------------------------------------------------------------------

## 2. Product objective: improve the picture after YouTube

The user's concern is not simply camera noise. YouTube's constrained
live-stream bitrate/transcode visibly smears the stream. The working
hypothesis is:

> A carefully denoised, temporally stable source should be more
> compressible, allowing a constrained downstream codec to spend more of
> its bitrate on meaningful edges, texture and subject detail rather
> than stochastic camera noise.

This must become an explicit quality objective.

Too little denoise can leave random high-frequency energy that is
expensive to encode. Too much denoise destroys real texture before
YouTube ever sees it. Therefore the objective is **viewer-side
rate/distortion quality**, not maximum noise removal.

The primary question is:

> At identical YouTube ingest conditions, does the denoised source
> preserve more meaningful detail after YouTube's transcode than the
> untreated source?

Current YouTube guidance is consistent with the production context:
YouTube Live HDR uses 10-bit HDR and supports HEVC/H.265; its OBS HDR
instructions specify P010 and Rec.2100 HLG/PQ and recommend HLG. Current
live guidance permits 4K30 H.265 ingest over a constrained bitrate
range. Do not hard-code those bitrate numbers into the denoiser; they
are validation context.

References: - https://support.google.com/youtube/answer/10265272 -
https://support.google.com/youtube/answer/2853702 -
https://support.google.com/youtube/answer/1722171 -
https://support.google.com/youtube/answer/7126552

------------------------------------------------------------------------

## 3. Hardware policy

### GTX 1650 Super

Continue benchmarking everything on it. It tells us the baseline and
relative costs, but do not simplify the architecture solely to fit it.

NVIDIA lists the GTX 1650 Super with 1280 CUDA cores, 4 GB GDDR6 and a
128-bit memory interface:
https://www.nvidia.com/en-us/geforce/graphics-cards/compare/

### RTX 3070-class upgrade tier

Treat an RTX 3070-class card as an explicit potential target for
higher-quality modes. NVIDIA lists the RTX 3070 with 5888 CUDA cores, 8
GB GDDR6 and a 256-bit memory interface:
https://www.nvidia.com/en-us/geforce/graphics-cards/30-series/rtx-3070-3070ti/

Do **not** infer an exact denoise speedup from core counts. The kernels
may be limited by memory bandwidth, texture throughput, occupancy,
synchronization or structure rather than FP32.

Design for scaling:

``` text
1650 Super = baseline/current-hardware presets
3070-class = legitimate higher-quality target
future faster GPU = should gain from the architecture
```

Do not optimize into a 1650-specific dead end.

------------------------------------------------------------------------

## 4. Native D3D11 compute is approved

A Windows-only native D3D11 compute path is acceptable and strategically
desirable.

Reasons: - Windows is the actual production platform. - P3 NLMeans is
likely to benefit substantially from compute shaders, UAVs and
groupshared memory. - P2 recursive HQDN3D is a smaller workload with
which to prove D3D11/OBS coexistence. - B remains a portable fallback. -
A GPU upgrade increases the value of a scalable compute architecture.

Expected split:

``` text
Windows/D3D11:
    HQDN3D temporal
    portable B spatial
    preferred recursive A compute spatial
    optimized compute NLMeans

Other platforms:
    HQDN3D temporal
    portable B spatial
    reduced/portable NLMeans only if practical
```

Do not compromise the Windows production path merely to force
algorithmic parity across platforms the user does not run.

------------------------------------------------------------------------

## 5. D3D11 state isolation

Use plugin-owned UAV-capable resources. Do not assume OBS textures are
UAV-capable.

Preferred spike: 1. obtain OBS's `ID3D11Device*`; 2. allocate persistent
private RGBA16F SRV/UAV resources; 3. copy the OBS program texture into
private compute resources where required; 4. execute compute; 5.
return/copy/render the result; 6. leave OBS graphics state functionally
unchanged.

Investigate a D3D11 **deferred context** and command list, with state
restoration on execution, as one isolation strategy. Treat this as a
hypothesis to validate, not an assumption.

References: -
https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-executecommandlist -
https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createunorderedaccessview

If deferred contexts add excessive overhead or interact poorly with OBS,
use explicit immediate-context state management and test it rigorously.

------------------------------------------------------------------------

## 6. Meaning of "faithful HQDN3D"

We are not targeting FFmpeg/MPlayer bit equivalence.

"Recursive HQDN3D" means preserving: - nonlinear difference-dependent
smoothing; - recursive/IIR propagation through similar regions; - edge
rejection when differences become large; - separate luma/chroma
strengths; - low cost relative to NLMeans; - exact identity at strength
zero.

It does **not** mean preserving: - FFmpeg/MPlayer code or code
structure; - original lookup tables; - integer/YUV arithmetic; -
left-to-right or top-to-bottom visual bias; - exact FFmpeg numerical
output.

No FFmpeg/MPlayer source is to be copied, translated, adapted or ported.

------------------------------------------------------------------------

## 7. Directionality and separability

Remove causal directional bias.

For each axis, calculate independent forward and backward recurrences
from the same axis input, then average:

``` text
Hf = recurse(left -> right, input)
Hb = recurse(right -> left, input)
H  = 0.5 * (Hf + Hb)

Vf = recurse(top -> bottom, H)
Vb = recurse(bottom -> top, H)
Spatial = 0.5 * (Vf + Vb)
```

Do not use a forward result as the input to the backward pass by
default; that effectively filters twice and changes strength.

Horizontal then vertical separability is accepted. Do not build the
fused global wavefront dependency.

------------------------------------------------------------------------

## 8. Recursive spatial definition --- A

Reuse P1's representation:

``` text
Y = 0.2126 R + 0.7152 G + 0.0722 B
c = RGB - Y*(1,1,1)

F(x) = sign(x) * log2(1 + abs(x)/K)
K = 0.1 nit initially

W(x) = (1 - x*x)^2  for 0 <= x < 1
W(x) = 0            for x >= 1

beta_s = 0.9 initially
```

For current unfiltered input `p` and previous filtered sample `qprev`:

### Luma

``` text
dL = abs(F(Yp) - F(Yprev))
TL = 0.01 * spatial_luma_strength

wL = 0                                  if TL <= 0
wL = beta_s * W(dL/TL)                  otherwise

Yout = (1-wL)*Yp + wL*Yprev
```

### Chroma

Use the same P1 chroma-distance definition already implemented/tested:

``` text
dC = chroma_distance(cp, cprev, Yp, Yprev)
TC = 0.01 * spatial_chroma_strength

wC = 0                                  if TC <= 0
wC = beta_s * W(dC/TC)                  otherwise

cout = (1-wC)*cp + wC*cprev

RGBout = Yout + cout
```

Do not invent a new P2 chroma metric unless P1 shows a real defect.

------------------------------------------------------------------------

## 9. Portable spatial fallback --- B

Call/document this **HQDN3D-style spatial**. It is not recursive HQDN3D.

For symmetric offset `k`:

``` text
decay(k) = beta_s ^ abs(k)

dL(k) = abs(F(Y[x+k]) - F(Y[x]))
edgeL(k) = W(dL(k)/TL)
weightL(k) = decay(k) * edgeL(k)
weightL(0) = 1

Yout = sum(weightL(k)*Y[x+k]) / sum(weightL(k))
```

Apply the equivalent chroma operation using the existing P1
chroma-distance metric.

Test: - R=6 - R=8 - R=12

Do not expose radius in the normal UI initially. Choose it from
measurement.

------------------------------------------------------------------------

## 10. Do not build C yet

Keep bounded-window recursion in reserve only if:

``` text
A compute path fails or is unacceptable
AND
B quality is insufficient
```

It otherwise consumes engineering effort without advancing P3 compute
infrastructure.

------------------------------------------------------------------------

## 11. Spatial/temporal order

Use:

``` text
current
  -> spatial
  -> temporal
  -> output
  -> temporal history
```

Temporal history stores the final temporal result.

This gives clean composition: - spatial=0 -\> P1 temporal - temporal=0
-\> spatial only - both=0 -\> identity

Do not spatially re-filter stored history every frame.

------------------------------------------------------------------------

## 12. Performance policy --- revised because hardware can be upgraded

The 8--10 ms goal remains desirable, but **do not use it as a universal
rejection threshold on the 1650 Super**.

Classify modes:

### Tier 0 --- current card comfortable

``` text
combined denoise <= 8 ms p95
```

### Tier 1 --- current card marginal

``` text
8–15 ms p95
```

Retain if quality is materially better. Offer a lighter preset for
current hardware.

### Tier 2 --- upgrade-target

``` text
>15 ms p95 on 1650 Super
```

Still retain if: - quality advantage is meaningful; - algorithm is well
parallelized/scalable; - there is no pathological
serialization/synchronization bottleneck; - memory usage is
reasonable; - an RTX 3070-class GPU should materially benefit.

Do not spend disproportionate engineering effort micro-optimizing for
the 1650 if it compromises quality or architecture.

------------------------------------------------------------------------

## 13. Compute infrastructure gate

For the private-resource compute round trip on the 1650 Super:

Preferred:

``` text
<= 1.0 ms p95
```

Acceptable for continued investigation:

``` text
<= 1.5 ms p95
```

If materially above 1.5 ms, investigate copies, synchronization,
resource creation and state restoration before abandoning compute.
Resources must be persistent, not created per frame.

------------------------------------------------------------------------

## 14. P2 synthetic quality gate

Compare A and B at **matched noise reduction**, not equal slider values.

Use deterministic flat-field noise at: - 18 nits - 203 nits

Tune each candidate to:

``` text
sigma_out / sigma_in <= 0.70
```

(at least 30% reduction in RMS noise standard deviation).

Then test noisy step edges: - 18 -\> 203 nits - 203 -\> 1000 nits

Initial engineering acceptance at the matched-noise point:

``` text
10–90% edge-width increase <= 1.5 px
overshoot/undershoot <= 2% of step amplitude
```

No visible directional edge asymmetry is acceptable.

These are comparison gates, not definitions of ideal photographic
quality.

------------------------------------------------------------------------

## 15. Real-footage gate

Use actual a6400/a6700 footage.

Primary diagnostic:

``` text
removed = input - output
```

with adjustable display gain.

Desired removed signal: - predominantly stochastic grain/noise.

Bad removed signal: - facial structure; - hair strands; - fabric
texture; - text edges; - architectural edges; - meaningful highlight
texture.

Inspect skin, hair, dark clothing, walls/backgrounds, fine text,
specular detail, movement, cuts and dissolves.

Synthetic tests establish repeatability; real footage decides
usefulness.

------------------------------------------------------------------------

## 16. Add a codec-oriented quality gate

Because the actual objective is improved YouTube clarity, evaluate
denoise **after constrained encoding**, not only before it.

For representative identical source clips:

``` text
A = no denoise
B = HQDN3D light/balanced
C = best HQDN3D spatial+temporal
D = NLMeans candidate
```

At minimum, encode each with identical HEVC Main10 settings
approximating the real YouTube ingest constraint.

Where practical, run an **unlisted/private YouTube A/B** under identical
ingest settings and inspect YouTube's resulting playback/transcode.

Evaluate: 1. facial detail; 2. hair; 3. fabric; 4. background texture;
5. motion detail; 6. gradients; 7. blockiness; 8. mosquito/ringing
artifacts; 9. temporal smear; 10. meaningful detail retained after
YouTube.

Do not tune solely against the pre-encode picture.

A source that is slightly smoother locally can still win after a
constrained codec if it stops the codec spending bits on stochastic
noise. Conversely, detail destroyed before encoding cannot be recovered
by YouTube.

------------------------------------------------------------------------

## 17. Objective codec test with a known clean reference

Do not blindly score a denoised image against the noisy camera image;
the noisy image is not necessarily the desired ground truth.

For synthetic testing:

``` text
clean reference
    -> add deterministic camera-like noise
    -> denoise
    -> constrained encode/decode
    -> compare decoded output to clean reference
```

Use as available: - PSNR; - SSIM; - VMAF; - edge width/MTF; - residual
noise.

For real camera footage, rely more on matched visual A/B, removed-signal
view and downstream encoded result.

------------------------------------------------------------------------

## 18. Temporal denoise may be disproportionately valuable to YouTube

Random noise changing every frame is costly for an inter-frame codec.

Therefore HQDN3D temporal may provide a large compression benefit for
very little GPU cost.

P1 real-footage testing must include: - static scene; - slow subject
motion; - camera movement; - hard cuts; - dissolves.

We want temporal stability without ghosting, trails, stuck texture or
delayed transitions.

This is another reason to run P1 before freezing P2 defaults.

------------------------------------------------------------------------

## 19. P3 NLMeans: do not use the naive cost as the design

The \~110 G samples/s calculation correctly describes a naive
patch×search implementation. It should **not** become the expected
production architecture.

Before reducing resolution, implement/research patch-distance reuse:

``` text
for each search offset d:
    per-pixel squared difference
        -> reusable patch/box sum
        -> NLMeans weight
        -> accumulate candidate
```

Use groupshared-memory tiling, rolling/separable box sums, integral-like
reuse, or an equivalent efficient formulation.

Requirement:

> Patch area must not multiply global texture fetches naively for every
> candidate and output pixel.

Try **full-resolution luma-derived weights with full-resolution RGB
reconstruction first**.

Only after that should we consider: - half-resolution weights; - reduced
search; - reduced patch; - other approximations.

Because the user is willing to upgrade, retain a high-quality
full-resolution mode even if it is too expensive for the 1650 Super.

------------------------------------------------------------------------

## 20. NLMeans quality tiers

Design scalable presets:

``` text
Light
    intended to fit current 1650 Super if possible

Balanced
    quality/performance midpoint

High
    full-resolution stronger mode;
    explicitly allowed to target RTX 3070-class hardware

Custom
    advanced/testing
```

Do not deliberately cripple `High` to make it run on the 1650 Super.

------------------------------------------------------------------------

## 21. Compute safety gate

Before native compute becomes production:

1.  exact-value HDR identity unchanged;
2.  no \[0,1\] clamp;
3.  HDR Transform remains correct;
4.  HDR Color remains correct;
5.  preview/multiview correct;
6.  stream correct;
7.  recording correct;
8.  simultaneous stream+record does not duplicate denoise/history;
9.  resolution changes recover;
10. device/resource recreation survives;
11. no stale frame;
12. no corrupted OBS graphics state;
13. no D3D11 debug-layer errors where testable;
14. failure falls back transparently rather than black.

A fast compute backend that destabilizes OBS is rejected.

------------------------------------------------------------------------

## 22. P1/P2 scheduling

Proceed now with: - B CPU reference; - B float mirror; - B shader; -
D3D11 compute identity spike; - recursive A CPU reference; - recursive A
compute implementation after the spike passes.

Do **not** wait for P1 tuning to begin that engineering.

But do not freeze P2 defaults or make the final A/B production choice
until P1 temporal has run in OBS on real footage.

------------------------------------------------------------------------

## 23. Selection rule

Do not select:

> whichever is fastest on the 1650 Super.

Select:

> the implementation that gives the best meaningful-detail retention and
> downstream YouTube clarity at an acceptable hardware cost.

Priority order:

1.  OBS/HDR correctness
2.  absence of temporal/edge artifacts
3.  downstream encoded/YouTube image quality
4.  useful denoise strength
5.  GPU cost
6.  portability

Windows is the production platform.

------------------------------------------------------------------------

## 24. Likely product arrangement to test toward

Do not assume this outcome, but it is a useful architecture:

``` text
HQDN3D Light
    temporal + portable B
    current 1650 Super

HQDN3D High
    temporal + bidirectional recursive A
    D3D11 compute
    current card if it fits, otherwise upgrade tier

NLMeans Light
    optimized compute
    modest search

NLMeans High
    optimized full-resolution compute
    allowed to target RTX 3070-class or better
```

------------------------------------------------------------------------

## 25. Hardware-upgrade decision after measurements

After P2/P3 report:

  ---------------------------------------------------------------------------------
  Mode           1650S p95         Noise   Edge/detail Encoded/YouTube   Class
                               reduction        result result            
  ---------- ------------- ------------- ------------- ----------------- ----------
  HQ                                                                     
  temporal                                                               

  HQ B                                                                   

  HQ A                                                                   

  NLM Light                                                              

  NLM                                                                    
  Balanced                                                               

  NLM High                                                               
  ---------------------------------------------------------------------------------

Then choose hardware from actual bottlenecks.

RTX 3070 is an example target, not yet a fixed purchase recommendation.

NVIDIA specifies 8 GB GDDR6, 5888 CUDA cores, a 256-bit memory
interface, 220 W graphics-card power and 650 W reference system-power
requirement for the RTX 3070. A later hardware recommendation must
therefore also check PSU, connectors and case clearance.

Reference:
https://www.nvidia.com/en-us/geforce/graphics-cards/30-series/rtx-3070-3070ti/

------------------------------------------------------------------------

## 26. Direct answers to the decision request

### 1. Windows-only native D3D11 compute?

**Yes. Approved and strategically desirable.** Keep B as the portable
fallback.

### 2. Which P2 algorithms?

**Option D: B + compute spike + A if the spike passes.** Do not build C
now.

### 3. What does faithful mean?

Preserve nonlinear recursive/IIR behavior, edge-aware response and
luma/chroma separation. Do not preserve CPU implementation structure,
one-way causality, directional bias or exact FFmpeg output.
Bidirectional separable recursion is the preferred recursive design.

### 4. Acceptance numbers?

Use the matched-noise/edge gates above. For performance, \<=8 ms
combined p95 on the 1650 Super is excellent; 8--15 ms is marginal but
retain if quality justifies it; \>15 ms may remain an upgrade-target
mode if scalable and materially better.

### 5. Wait for P1?

**No for engineering; yes for final tuning/selection.**

------------------------------------------------------------------------

## 27. New explicit quality objective

Create `docs/denoise/QUALITY_OBJECTIVE.md` with this statement:

> The denoiser is intended not merely to make the local OBS preview
> cleaner, but to give a constrained downstream live encoder and YouTube
> transcode a temporally cleaner, more compressible 4K HDR source. The
> desired result is that bitrate is spent on meaningful scene structure
> rather than stochastic camera noise, while preserving enough real
> texture that the denoiser itself does not become the source of
> softness. Denoise quality is therefore evaluated both before encoding
> and after a controlled same-bitrate encode, with actual YouTube A/B
> testing where practical.

Do not let "noise removed" become the sole quality metric.

------------------------------------------------------------------------

## 28. Immediate next actions

1.  Run P1 temporal in actual OBS as soon as practical and collect real
    timing/footage observations.
2.  In parallel implement B CPU reference + float mirror + shader.
3.  Build the D3D11 private-resource compute identity spike.
4.  Run compute safety/HDR gates.
5.  If passed, implement bidirectional separable recursive A.
6.  Compare A/B at matched synthetic noise reduction.
7.  Compare A/B on real Sony footage using the removed-signal view.
8.  Add controlled same-bitrate HEVC A/B.
9.  Where practical, perform an actual YouTube A/B.
10. Only then select the P2 default.
11. Carry the proven compute infrastructure into optimized P3 NLMeans.

From this point onward, replace cost estimates with measurements from
the user's machine as soon as they exist.
