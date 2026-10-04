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

Status: **P1 = temporal only.** The spatial part (P2) gets its own section when it is
designed.

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
- P1 is temporal only. The spatial part (P2) is a separate design step: either a
  recursive GPU pass (D3D11 compute) or a labelled "HQDN3D-style" separable
  approximation.

Because of these differences the UI calls the mode **HQDN3D-style**.
