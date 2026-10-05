# HQDN3D-style program denoise: design specification

Written before any HQDN3D code, as denoise brief v1.2 section 28 requires. The code in
`src/denoise/` is implemented from this specification. **No FFmpeg/MPlayer source was
copied, translated, adapted or ported.** Only the published *behaviour* of HQDN3D is
used as a reference:
- temporal low-pass against the previous *filtered* frame;
- nonlinear: small differences are smoothed, large differences pass;
- separate luma and chroma strengths.

The FFmpeg implementation is not used. This design differs from it structurally:
- it works on high-precision linear HDR RGB on the GPU, not 8-bit YUV planes on the CPU;
- the comparison is made in a log-like HDR domain;
- the response curve is our own closed form, with no lookup tables;
- cut detection and transition protection are added.

Status: P1 temporal (sections 1-7) and P2 spatial (section 8). P2 follows the decision
in `docs/denoise/P2_DECISION_RESPONSE.md` (Option D).

## 1. Signal

Input: the finished program frame, linear RGB in nominal nits. The OBS working value is
multiplied by the SDR white level W (for `GS_CS_709_EXTENDED`; 80 for scRGB) and
divided again on output. Alpha is passed through from the current frame and never
filtered.

Luma and chroma (brief 10), Rec.709 / D65 luminance:

```text
Y = 0.2126 R + 0.7152 G + 0.0722 B         (linear nits)
c = (R, G, B) - Y * (1, 1, 1)              (luma-orthogonal residual)
reconstruction: RGB = Y * (1,1,1) + c      (exact)
```

Neutral input has c = 0. Chroma is filtered as c, so neutral stays exactly neutral.
Negative and out-of-gamut components are carried through unchanged.

## 2. Comparison domain (brief 9)

Differences are judged in a signed log-like domain; the averaging uses the original
linear values.

```text
F(y) = sign(y) * log2(1 + |y| / K)          K = comparison knee, nits (default 0.1)
```

- F is odd, monotonic and finite for every finite y, with F(0) = 0.
- Above K, a difference of 0.01 in F is about a 0.7% change in luminance at any
  level. On an HLG camera signal that is roughly one 10-bit code near mid-grey.
- Below K, F becomes linear in nits, so near-black noise is not over-weighted.

```text
luma difference      dL = F(Y_hist) - F(Y_cur)
chroma difference    dC = |c_hist - c_cur| / ((|Y_hist| + |Y_cur|) / 2 + K) / ln 2
```

dC is the chroma change relative to the local luminance, in the same "log2 units"
as dL (for small changes, log2(1 + x) is about x / ln 2).

## 3. Temporal recurrence

Per pixel, with H the history (the previous filtered frame) and C the current frame:

```text
w_L = β * W(|dL| / T_L) * g          w_C = β * W(dC / T_C) * g
Y_out = Y_cur + w_L * (Y_hist - Y_cur)
c_out = c_cur + w_C * (c_hist - c_cur)
RGB_out = Y_out * (1,1,1) + c_out ;  alpha_out = alpha_cur
H_next = RGB_out (the filtered frame, before Grade-style Mix)
```

Response:

```text
W(x) = (1 - x²)²  for 0 <= x < 1,  0 for x >= 1
```

W is smooth, with zero slope at x = 0 and at x = 1, and a compact support. A
difference at or above the threshold T passes completely, so moving edges are not
dragged. Small differences are blended towards the history.

- **β = 0.9** is the maximum history weight. The history is therefore refreshed by
  at least 10% every frame, so a static region can never freeze, and stale content
  decays with a time constant of at most about 10 frames.
- **Strength to threshold:** `T = 0.01 * S` (log2 units) for each of luma and chroma,
  with S in 0..20 on the UI. S = 0 is an exact identity: w = 0, so RGB_out = C exactly
  and H_next = C.
- **Global factor:** g in [0, 1] comes from cut detection and transition protection
  (section 4).

Noise reduction this gives: for a static region with noise well below T, w ≈ β and
the recursion is a first-order IIR. Its noise variance factor is (1 - β) / (1 + β) =
0.053, so noise standard deviation falls about 4.4×. As noise approaches T, w falls
and the filter backs off.

Properties (tested on the CPU, section 6):
- exact identity at S = 0;
- constant input stays constant;
- black stays black;
- neutral stays neutral;
- finite for negative components and for luminance from 0 to above 10,000 nits;
- the output always lies between C and H on luma and on each chroma component
  (no overshoot).

## 4. Frame change: cut reset and transition protection (brief 11)

The metric is computed on the GPU in the same frame, so a cut is caught on its first
frame without any CPU round trip:

```text
m = mean over a sample grid of |F(Y_cur) - F(Y_hist)|        (log2 units)
```

- **Grid:** pass 1 averages a 4×4 sub-grid of each 16×16 block, at offsets
  2, 6, 10, 14, into a w/16 × h/16 texture. Pass 2 averages 16×16 blocks of pass 1.
  Pass 3 averages all of pass 2 into one value. At 4K that is 240×135, then 15×9,
  then 1×1, for about 0.5 M samples per frame. Partial edge blocks average only
  their valid samples, and every block counts equally.
- **Static-noise floor:** pass 3 also tracks `f` in a ping-ponged 1×1 texture:
  f_next = min(m, f * 1.005 + 0.0001), and f = m on a history reset. The floor follows the
  quietest recent level downward at once and recovers upward slowly (0.5% per
  frame, so about 12 s to rise sixfold). A 2-second dissolve therefore cannot drag
  the floor up with it; the CPU test first used 2% and lost protection mid-dissolve.
- **Hard cut:** `m >= m_cut`, with `m_cut = 1.2 * 2^(-s/25)` from the Cut Sensitivity
  s (0..100; s = 50 gives 0.3, s = 100 gives 0.075). With *Scene Cut Reset* on, a cut
  frame uses g = 0, so the output is the current frame and the history becomes the
  current frame: no ghost of the previous shot.
- **Transition protection:**
  `p = smoothstep(0, 0.02, m - 1.5 f)` and `g = 1 - A * p`, with A the Protection
  Amount (0..1).
  - A dissolve or a camera move raises the frame change above the static noise
    floor, so temporal strength drops smoothly and the image does not lag or trail.
  - Once the picture settles, m returns to the floor and g returns to 1.
  - The frame is never reset in the middle of a dissolve unless m reaches m_cut.

## 5. History lifecycle (brief 8, 18)

The history is reinitialised (H = C, output = C) when:
- the plugin starts;
- the program resolution or format changes;
- the algorithm changes into HQDN3D;
- *Reset History* is pressed;
- a cut is detected.

History textures are RGBA16F at program size, ping-ponged: two of them, plus the
current-frame copy, about 190 MiB at 4K. The metric textures add 240×135 and 15×9 R32F
plus two 1×1 RGBA32F. Only once-per-frame processing updates the history (P0 hook).
Debug views change only the final draw, never the history.

## 6. Verification

`tests/cpu/test_hqdn3d.cpp` checks the double-precision reference of sections 1–4
against a float32 mirror of the shader, which follows the shader's operation order.
It covers:
- identity at S = 0;
- constant fields and black;
- neutral stays neutral;
- negative and >10,000-nit values;
- the no-overshoot bound;
- noise reduction on a static noisy field (measured against the IIR prediction);
- a moving edge passing;
- the cut sequence (first B frame equals B exactly);
- a dissolve sequence (protection reduces lag);
- the metric grid on odd sizes.

## 7. Intentional differences from conventional HQDN3D

- Linear HDR RGB with a log-like comparison domain, instead of 8-bit Y'CbCr planes
  with a code-value difference.
- A closed-form response curve (β · (1 - x²)²) instead of a precomputed coefficient
  table.
- A history-weight cap (β = 0.9) so that nothing freezes.
- Chroma difference measured relative to luminance.
- Cut reset and transition protection added; they are not part of HQDN3D.
- The spatial part (section 8) is bidirectional: there is no left-to-right or
  top-to-bottom bias.

Because of these differences the UI calls the mode **HQDN3D-style**.

## 8. Spatial (P2)

Decision: `docs/denoise/P2_DECISION_RESPONSE.md`. There are two spatial filters:
- **B, "HQDN3D-style spatial"**: a symmetric kernel, portable, built first.
- **A, recursive**: a bidirectional separable recursion. It runs as a D3D11 compute
  shader, and only after the compute identity spike passes on the user's machine.

The CPU reference of A exists now. Neither is the default yet: the P2 strengths and the
A/B choice wait until P1 has run on real footage.

"Faithful HQDN3D" (decision section 6) means keeping these properties:
- smoothing that depends nonlinearly on the difference;
- IIR propagation through similar regions (A only);
- edge rejection;
- separate luma and chroma strengths;
- exact identity at zero strength.

It does not mean keeping FFmpeg's code structure, tables, integer YUV arithmetic,
one-way causality or numerical output.

### 8.1 Order and composition

```text
current -> spatial -> temporal -> output
                        |-> temporal history (stores the temporal result)
```

- The frame metric (section 4) and the temporal recurrence both read the spatially
  filtered current frame, so history and current are compared like with like.
- Stored history is never spatially re-filtered.
- Mix and the Difference view still compare against the unfiltered input, so the
  Difference view shows `input - output` for spatial and temporal together.

Composition:
- spatial = 0 is exactly P1;
- temporal = 0 is spatial only (the temporal pass is then an exact identity);
- both = 0 is an exact identity.

### 8.2 Shared quantities

Same representation as P1 (sections 1-2): Y, c = RGB - Y, F with knee K, and W(x) =
(1 - x²)². Also:

```text
T_L = 0.01 * S_L     T_C = 0.01 * S_C      spatial strengths S in 0..20
beta_s = 0.9
dL(a, b) = |F(Y_a) - F(Y_b)|
dC(a, b) = |c_a - c_b| / ((|Y_a| + |Y_b|) / 2 + K) / ln 2      (the P1 chroma distance)
```

Both filters are separable: horizontal first, then vertical. The vertical pass
compares and averages the output of the horizontal pass. Samples outside the frame do
not exist: they get weight 0 and are not clamped or mirrored. Alpha is passed through
from the input pixel.

### 8.3 B - HQDN3D-style spatial (symmetric kernel)

Along one axis, for the centre sample x and offsets k = -R..R:

```text
wL(0) = wC(0) = 1
wL(k) = beta_s^|k| * W(dL(x+k, x) / T_L)        (0 if T_L <= 0)
wC(k) = beta_s^|k| * W(dC(x+k, x) / T_C)        (0 if T_C <= 0)
Y_out = sum wL(k) Y[x+k] / sum wL(k)
c_out = sum wC(k) c[x+k] / sum wC(k)            (per component)
RGB_out = Y_out + c_out
```

- **Exact identity.** If every off-centre weight is 0 for both luma and chroma, the
  output is the input sample unchanged. Y + (RGB - Y) is not bit-exact in floating
  point, so the shader returns the sample directly. This covers S = 0, and also pixels
  whose neighbours all differ by more than T.
- **Radius.** R is chosen by measurement from 6, 8 and 12 (decision section 9). It is a
  development option, not normal UI. The provisional value is 8.
- **Bounds.**
  - Y_out and each component of c_out are convex combinations of the input, so they
    have no overshoot.
  - The kernel is symmetric, so a mirrored input gives a mirrored output. Floating-point
    summation order is the only difference.
- **Cost.** At R = 8 there are 17 taps per axis, so 34 texel loads per pixel.

B is not a recursive filter. Its reach is limited to R, and its averaging weight comes
from each sample's difference to the centre, not to a running estimate.

### 8.4 A - bidirectional separable recursion

The recursion along one line, with input p[0..n-1]:

```text
q[0] = p[0]
q[i] = (1 - w) p[i] + w q[i-1]     separately for Y (w = wL) and for c (w = wC)
wL = beta_s * W(dL(p[i], q[i-1]) / T_L)      wC = beta_s * W(dC(p[i], q[i-1]) / T_C)
```

The forward (left to right) and backward (right to left) recursions both run from the
**same** input. Their results are averaged, so the backward pass never filters the
forward result a second time:

```text
H = 0.5 (Hf + Hb)          then on H:          Spatial = 0.5 (Vf + Vb)
```

- Weights are compared against the running filtered value q[i-1], as in classic
  HQDN3D. This lets smoothing propagate through a flat region.
- A step larger than T gives w = 0, which restarts the recursion. Edges pass.
- Each direction is a convex combination of the input, so the average is too: no
  overshoot on Y or on c.
- A mirrored line swaps Hf and Hb, so the result is mirror-symmetric by construction.
- The same identity rule as B applies: with S = 0, or with every w = 0, the output
  equals the input.

GPU mapping (P2 compute, after the spike):
- One thread per line and direction. At 4K that is 2 x 2160 threads for the horizontal
  pass and 2 x 3840 for the vertical pass, each serial over the line length.
- Parallelism is therefore low, and memory latency may dominate. Only measurement on
  the user's machine decides whether this is acceptable (decision section 12 tiers).
- Splitting lines into overlapping segments would be option C, which is not to be built
  (decision section 10).

### 8.5 Synthetic gates (decision section 14), CPU

Noise model:
- deterministic Gaussian noise, independent per channel;
- σ = 2% of the level on R, G and B; the gates use this σ;
- flat fields at 18 and 203 nits.

Matched noise reduction:
- σ is measured on Y (luma) and on c (chroma, RMS over the 3 components).
- S_L is the smallest strength with σ_out/σ_in ≤ 0.70 on luma.
- S_C is found the same way on chroma.
- This is done for each filter (B at R = 6, 8, 12, and A) and each level.

At the matched point, noisy steps are tested: 18 -> 203 and 203 -> 1000 nits, rising and
falling, as vertical and horizontal edges. Rows are averaged along the edge to give a
profile.
- **Edge width:** the increase in 10-90% width is ≤ 1.5 px.
- **Overshoot and undershoot:** ≤ 2% of the step.
- **Direction:** rising/falling and horizontal/vertical agree. The CPU reference also
  checks mirror symmetry directly.

Note on edge strength: both gated steps are 2.3 to 3.5 stops, far above any T at the
matched point. Pixels across them therefore get weight 0, and the gates are expected to
pass with a large margin. The informative case is a **low-contrast** step, at 18 -> 21
and 203 -> 240 nits (about 0.22 and 0.24 stops). That is where an edge-aware filter
actually blurs. It is reported (not gated) so that A and B can be compared there.

These are comparison gates, not definitions of image quality. Real footage decides,
using the removed-signal view (decision sections 15-17 and `docs/denoise/QUALITY_OBJECTIVE.md`).

### 8.6 Synthetic results (CPU, `tests/cpu/test_spatial.cpp`, 2026-10-04)

Measured on the double-precision references. The B shader is mirrored in float and
agrees within 3e-5 relative.

Columns:
- *S_L / S_C*: the strength at which σ_out/σ_in reaches the target (luma and chroma
  solved separately).
- *dW*: worst 10-90% width increase over rising/falling and vertical/horizontal edges.
- *+2%, +5%, +10%*: dW on low-contrast steps (informative, not gated).

| Filter | Nits | Target | S_L | S_C | Gated step dW (px) | Overshoot | +2% | +5% | +10% |
|---|---|---|---|---|---|---|---|---|---|
| B R=6 | 18 | 0.70 | 3.24 | 6.28 | 0.00 | 0.19% | 0.11 | 0.02 | 0.00 |
| B R=8 | 18 | 0.70 | 3.11 | 6.02 | 0.00 | 0.19% | 0.11 | 0.02 | 0.00 |
| B R=12 | 18 | 0.70 | 2.98 | 5.75 | 0.00 | 0.19% | 0.11 | 0.02 | 0.00 |
| A | 18 | 0.70 | 4.85 | 9.26 | 0.00 | 0.20% | 0.18 | 0.02 | 0.01 |
| B R=6 | 18 | 0.50 | 4.28 | 8.01 | 0.00 | 0.17% | 2.14 | 0.03 | 0.00 |
| B R=8 | 18 | 0.50 | 4.11 | 7.68 | 0.00 | 0.17% | 2.27 | 0.03 | 0.00 |
| B R=12 | 18 | 0.50 | 3.94 | 7.34 | 0.00 | 0.16% | 2.39 | 0.03 | 0.00 |
| A | 18 | 0.50 | 6.30 | 11.54 | 0.00 | 0.16% | 2.48 | 0.04 | 0.01 |

At 203 nits the strengths and widths are the same within 0.1 and 0.01 px, because
the comparison is in the log domain and the noise is a fixed percentage. Overshoot
equals the input profile's own noise (0.2%), and the filters add none.

Reading:
- **The gated steps do not discriminate.** At the matched point, T (about 0.03 log2
  units) is about 100 times smaller than either step, so no filter averages across
  them. The gates pass by construction. This is the design note in 8.5, now confirmed.
- **Where the filters do blur.**
  - At 0.70 noise reduction, only a step at the noise amplitude (+2%) is touched, and
    by at most 0.2 px.
  - At 0.50, that step is widened by 2.1 to 2.5 px. A +5% step is still essentially
    untouched (≤ 0.04 px).
  - The filters trade noise for blur only for detail that is comparable with the noise
    itself. That is the expected edge-aware behaviour, not a defect.
- **Radius.** R = 6, 8 and 12 differ by at most about 0.25 px on the +2% step at the
  0.50 point (larger R blurs slightly more and needs slightly lower S). On white noise
  the edge function, not the radius, limits the kernel, so R = 6 is enough. Camera noise
  is spatially correlated (demosaic, in-camera processing, codec), and correlated noise
  benefits from a larger support. R therefore stays provisional at 8 until real footage
  has been measured. R = 6 is the cheaper choice if footage shows no difference.
- **A against B.** At matched noise reduction, A is not better than B on these
  synthetic tests: it is marginally softer on the +2% step at 0.70 (0.18 against 0.11
  px). It needs higher strength values for the same reduction, because its weights
  compare against a smoothed running value. Its expected advantage is unbounded
  propagation through large flat or correlated-noise areas, and white-noise flat fields
  do not test that. The A/B choice therefore rests on real footage and on the codec test
  (decision sections 15-17), as planned.

## 9. Luminance-dependent threshold (noise profile)

Decision C of `docs/denoise/HIGH_BITRATE_ANALYSIS.md` changes the thresholds of sections
3 and 8:

```text
T_L,eff = T_L * m(Y)    T_C,eff = T_C * m(Y)
```

- **Temporal (section 3):** Y = (|Y_hist| + |Y_cur|) / 2.
- **Spatial B (8.3):** Y = |Y| of the centre sample.
- **Spatial A (8.4):** Y = (|Y_p| + |Y_qprev|) / 2.

m(Y) is defined in `docs/denoise/NOISE_PROFILE.md`. The default is the identity profile
(m = 1 exactly), so sections 3-8 are unchanged unless a profile is selected. The
curve is a development option and is not frozen.
