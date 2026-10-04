# NLMeans engine: spatial and causal temporal, one design

The governing documents are `P2_DECISION_RESPONSE.md` (sections 19-20),
`P2_5_AMENDMENT.md` (sections 3-11) and the denoise brief (sections 14-16). This file was
written before any NLMeans code. The implementation is our own: no FFmpeg, HandBrake or
other NLMeans source was used.

Status: CPU references (`src/denoise/nlm-math.*`) and the synthetic comparison
(`tools/denoise-sim`). GPU kernels wait for the D3D11 compute spike to pass.

## 1. Candidates

A candidate is an offset `(dx, dy, dt)`:
- `dt = 0` searches the current frame;
- `dt = -1` searches the previous frame, which is causal. No future frame is ever used.

The engine treats both identically: same distance, same weights, same reconstruction.
The only differences are which reference image a candidate reads and the global
temporal factor (section 6).

```text
spatial NLMeans    {(dx,dy,0)  : |dx|,|dy| <= Rs}
temporal NLMeans   {(dx,dy,0)  : |dx|,|dy| <= Rs} U {(dx,dy,-1) : |dx|,|dy| <= Rt}
```

Rs = 0 together with Rt > 0 gives a purely temporal patch search, apart from the self
candidate. The code keeps `dt` as a general integer, but only {0, -1} is enabled.

## 2. Signals

The input is linear RGB in nominal nits (the shader multiplies OBS working values by
`nits_per_unit`), and alpha passes through. As in HQDN3D (`HQDN3D_DESIGN.md` 1-2):

```text
Y = 0.2126 R + 0.7152 G + 0.0722 B          c = RGB - Y
comparison image  g = F(Y) = sign(Y) log2(1 + |Y| / K),  K = 0.1 nit
```

- Patch distances use **g**, the luma in the log-like domain. Decision 19: weights come
  from luma, and the reconstruction is full-resolution RGB.
- The averaging uses the **original linear** Y and c. g is never output.
- Nothing is clamped: negative and out-of-gamut values are carried through, and F is
  odd and finite.
- An optional chroma term (section 3) handles isoluminant edges.

## 3. Distance and weights

Per candidate o and pixel x, with patch half-size P:

```text
e_o(z) = (g_t(z) - g_ref(z + o))^2 + lambda * dC(z, z + o)^2           (per pixel)
D_o(x) = (1 / (2P+1)^2) * sum_{|q| <= P} e_o(x + q)                      (box mean)
```

- `dC` is the HQDN3D chroma distance (chroma change relative to luminance, in log2
  units).
- `lambda` defaults to 0, which means luma only. It exists because an isoluminant colour
  edge is invisible to a luma-only distance. A test fixture measures whether it is
  needed.
- **Boundary:** patch samples are taken at clamped coordinates (edge replicate) for both
  images. A candidate whose centre `x + o` lies outside the frame is skipped. The naive
  and the offset-major implementations use exactly this rule, so they agree.

Weights, with strengths S in 0..20 and h = 0.01 S (log2 units, as T in HQDN3D):

```text
wL_o = exp(-D_o / hL^2)        wC_o = exp(-D_o / hC^2)        (0 if h <= 0)
wL = wC = 1 for the self candidate (0,0,0)
weights with D_o / h^2 > 12 are set to 0 (cutoff 6e-6; it keeps the support compact)
temporal candidates are multiplied by the global factor g_t (section 6)
```

- **No noise-variance subtraction.** The usual `max(D - 2 sigma^2, 0)` needs a noise
  estimate, and the noise depends on level (`NOISE_MODEL.md`). h absorbs it instead.
  This choice should be revisited after real-footage tuning.
- **Self weight 1.** The denominator is always at least 1, and S = 0 is an exact
  identity (section 7).

## 4. Reconstruction

```text
Y_out = sum wL_o Y_ref(x+o) / sum wL_o
c_out = sum wC_o c_ref(x+o) / sum wC_o           (per component)
RGB_out = Y_out + c_out ;  alpha_out = alpha_t(x)
```

Both are convex combinations, so Y and each chroma component stay inside the candidates'
range. That means no overshoot and no new extremes.

Luma and chroma strengths share one distance computation, so a separate chroma strength
costs only the extra accumulators.

## 5. Temporal reference: history policies (amendment section 6)

| Policy | Distance against | Values from | Recursive |
|---|---|---|---|
| A | previous input C(t-1) | C(t-1) | no |
| B | previous output D(t-1) | D(t-1) | yes |
| H (hybrid) | C(t-1) | D(t-1) | yes, but matching is unbiased |

Under policy B, D(t-1) is cleaner than C(t), so the expected patch distance for a true
match is σ² instead of 2σ². Temporal candidates are then systematically favoured, which
makes them stronger and also riskier.

The recursive policies (B, H) cap the temporal share so that history cannot freeze:

```text
share = sum_{dt=-1} w / sum w ;  if share > cap (0.9): temporal weights *= cap (1 - share) / ((1 - cap) share)
```

That is the same "history never above 90%" rule as HQDN3D's β. The synthetic comparison
decides which policy to keep; there is no UI for it.

The reference frame the engine stores:
- policy A: the previous input, after the spatial pre-filter if one runs;
- B and H: the previous output (H needs both).

Each is one program-size RGBA16F texture: 75 MiB at 4560 x 2160.

## 6. Cuts, dissolves, reset (amendment sections 10-11)

The global factor g_t from HQDN3D (`HQDN3D_DESIGN.md` 4) is reused unchanged:
- the same frame metric, against the stored reference;
- the same static-noise floor;
- the same cut threshold and protection ramp.

Temporal candidate weights are multiplied by g_t:
- **cut:** g_t = 0, so the temporal candidate set is effectively empty and spatial
  candidates still act;
- **dissolve:** g_t falls, which reduces temporal candidates while spatial ones remain;
- **history reset** (start, resize, mode change, button): g_t = 0, and the stored
  reference becomes the current frame.

No second transition detector is introduced.

## 7. Identity and bounds

- S_L = S_C = 0 gives an exact identity, because only the self weight is non-zero. The
  implementation returns the input sample itself, since Y + (RGB - Y) is not bit-exact.
- If every non-self weight is 0 (all candidates far), the output is again exactly the
  input.
- Constant input gives constant output; black stays black; neutral stays neutral (c = 0
  in, c = 0 out).
- Finite for negative components and values above 10,000 nits.

## 8. Offset-major evaluation (decision 19, amendment 8)

The naive form reads the whole patch for every (pixel, candidate). It is the reference
only:

```text
for x: for o: D = mean_q e_o(x+q) ...                                 O(N * |O| * (2P+1)^2)
```

The engine is offset-major:

```text
for o in candidates:
    e_o over the tile + patch apron           (one difference per pixel)
    D_o = separable box mean of e_o           (running sums: O(1) per pixel per axis)
    accumulate wL_o, wC_o, wL_o Y, wC_o c     (registers / per-pixel accumulators)
```

The cost is O(N |O|), independent of the patch area except for the apron. Patch size
becomes nearly free, and search area (|O|) is the cost driver. That changes which
presets make sense (section 10).

On the CPU, the offset-major reference must equal the naive reference to rounding
(tested to 1e-12 on random images, odd sizes, both dt).

**GPU mapping (compute, after the spike).**
- One thread group per 16 x 16 output tile. Groupshared memory holds:
  - g_t for the tile + P apron;
  - g_ref and RGB_ref for the tile + P + R apron;
  - e for the tile + P apron.

  At P = 2 and R = 7 that is a 34 x 34 reference block, about 23 KB.
- Each thread keeps its accumulators (wL, wC, Y, c x 3, sum wL, sum wC) in registers.
- Memory: no per-offset full-resolution images (brief 16, bounded memory). Extra VRAM is
  the reference frame (section 5) plus nothing per offset.

## 9. Cost estimate (not a measurement)

Assume about 25 ALU operations per (pixel, candidate) and 4560 x 2160 x 30 fps
(9.85 M px).

| Preset | Patch | Spatial window | Temporal window | Candidates | G candidate-evals/s |
|---|---|---|---|---|---|
| Light (spatial) | 3x3 | 7x7 | - | 49 | 14.5 |
| Light (spatial + temporal) | 3x3 | 7x7 | 7x7 | 98 | 29 |
| Balanced (spatial) | 5x5 | 11x11 | - | 121 | 36 |
| Strong (spatial) | 5x5 | 15x15 | - | 225 | 66 |

The GTX 1650 Super's FP32 peak is about 4.4 TFLOP/s. Light spatial + temporal is about
0.7 TFLOP/s, so roughly 5 ms at perfect efficiency, and realistically 2-3x that. Patch
size barely enters. Measurements on the user's machine replace this table.

## 10. Consequences for presets

The brief's presets (Light 3x3/7x7, Balanced 5x5/11x11, Strong 5x5/15x15) were sized for
a naive cost model.
- With offset-major evaluation, **patch 5x5 costs about the same as 3x3**.
- The measured noise is correlated over 3-8 px horizontally (`NOISE_MODEL.md`). 3x3
  patches would match noise blobs as if they were texture, which favours larger patches.

Proposal for measurement, not a default: keep the search sizes as the cost tiers and test
patch 5x5 at every tier.

## 11. Verification (CPU)

`tests/cpu/test_nlm.cpp`:
- naive = offset-major;
- exact identity at S = 0;
- constant, black and neutral fields;
- convex bounds;
- impulse response (brief N2);
- HDR stress with negative components, SDR white, nominal peak and above (N4);
- after a cut, no previous-frame contribution;
- the temporal share never above the cap (B, H);
- mirror symmetry of the spatial part.

The comparison with HQDN3D and motion-compensated temporal is in `MOTION_AWARE_TEMPORAL.md`.
