# Noise profile: luminance-dependent threshold T(Y)

The decision is in `HIGH_BITRATE_ANALYSIS.md`, section 6 and decision C:
- the abstraction is to be implemented now;
- the production curve is **not** frozen;
- the identity profile must be available;
- the curve must be bounded;
- the anchors are not to be exposed to normal users.

Code:
- `src/denoise/noise-profile.*` holds the double reference and the float mirror;
- `profile_mult()` in `data/effects/hdr-program-denoise.effect` is the shader version;
- `tests/cpu/test_profile.cpp` holds the tests.

## 1. Definition

```text
T_eff = T * m(Y)          T = 0.01 * S (log2 units), as before
m(Y) = clamp(interp(log2 |Y|), min_mult, max_mult)
```

The interpolation is piecewise-linear in log2(nits) between up to 6 anchors (log2 Y_i,
m_i), and constant outside the anchors. |Y| below 0.001 nit uses the first anchor.

- **Identity profile:** no anchors (n = 0), so m = 1 exactly, and every filter is
  bit-identical to the profile-free code. That is the default.
- **Bounds:** min_mult is in [0.1, 1] (default 0.5); max_mult is in [1, 16] (default 3,
  set by a development slider from 1 to 8). This keeps the threshold from running away
  near black (analysis section 6, reason 4).

Which luminance Y each filter uses:

| Filter | Y |
|---|---|
| HQDN3D temporal (and motion-compensated temporal) | (\|Y_cur\| + \|Y_hist\|) / 2, the same normaliser as the chroma distance |
| Spatial B | \|Y\| of the centre sample |
| Spatial A (CPU reference) | (\|Y_p\| + \|Y_qprev\|) / 2 |
| NLMeans (CPU reference) | \|Y\| of the centre pixel; scales h for every candidate |

The multiplier scales both the luma and the chroma thresholds. A chroma-only curve can be
added if footage shows that chroma noise follows a different law.

## 2. Data behind the built-in development profile

The four static regions measured by the instructing AI on the 5 Oct 2026 high-bitrate
clip (NVENC CQP 16/17, about 266 Mbit/s, 3840 x 2142) were converted here to the
denoiser's domains. Conversion: HLG inverse OETF, then a 1.2 system gamma at 1000 nits
(OBS working scale), then σ in nits = σ_code x dY/dcode, then σ_F = σ / ((Y + K) ln 2)
with K = 0.1.

| Region | Code | Luminance | σ (codes) | σ / Y (linear light) | σ_F | m = σ_F / σ_F(wall) |
|---|---|---|---|---|---|---|
| very dark TV | 184 | 2.27 nits | 2.57 | 5.14% | 0.0710 | 4.67 |
| piano | 442 | 35.6 nits | 2.16 | 1.37% | 0.0197 | 1.30 |
| wall (reference) | 544 | 64.4 nits | 1.80 | 1.06% | 0.0152 | 1.00 |
| floor | 598 | 89.4 nits | 1.41 | 0.89% | 0.0128 | 0.84 |

Two notes on reading the analysis:

1. **"Relative variation above black" is in code units.** The analysis reports σ / (code -
   64): 0.26%, 0.37%, 0.57%, 2.14%. In linear light, which is what the comparison domain F
   approximates above K, the same σ means 0.9-1.4% in the mids and 5.1% in the dark
   region. The rise towards black is about 4.7x in F, not the roughly 8x the code-domain
   figures suggest. The profile uses the F-domain ratio.
2. **The two correlation figures are different quantities.** The analysis's lag-1
   correlations (0.10-0.27) are **temporal**: frame-to-frame correlation at one pixel.
   The 0.84 / 0.60 figures from the 20 Mbit/s clip (`NOISE_MODEL.md`) are **spatial**:
   neighbouring pixels within one frame. They are not like-for-like. The conclusion still
   stands, because the 20 Mbit/s clip's noise was encoder residue, as `NOISE_MODEL.md`
   already said. What is still unmeasured is the **spatial** correlation of the
   high-bitrate clip. The simulator's `hb` noise model therefore assumes white noise.

Built-in profile 1, "Measured 5 Oct 2026 clip (development curve)":
- anchors (2.27, 4.67), (35.6, 1.30), (64.4, 1.00), (89.4, 0.84), in nits and multiplier;
- default max 3, so the dark anchor is clamped from 4.67 to 3;
- min 0.5.

**Above 89 nits nothing was measured.** The curve holds 0.84 constant there. That is an
extrapolation and a reason the curve is a development option, not a default.

The strength S now means "strength at the wall level (64 nits)". With the identity
profile it means the same at every level, as before.

## 3. Not frozen

These remain open (analysis section 6):
- the anchor set;
- the maximum multiplier;
- whether highlights should go below 1;
- whether chroma needs its own curve;
- perceptual protection of low-light structure.

The OBS-side tests (VALIDATION NP-*) and motion footage decide them. The development UI
offers only identity or the measured curve, plus the maximum; the anchors are not
exposed.

## 4. Verification

`test_profile.cpp`:
- identity is exactly 1, including a profile whose multipliers are all 1;
- the anchors are reproduced and clamped;
- constant outside the anchors;
- log-linear interpolation (the midpoint gives 1.15);
- monotone for this profile;
- the float mirror matches the double reference within 4.6e-7;
- sanitize sorts and spaces the anchors, and falls back to identity on NaN.

Effect on the filters:
- **Temporal at 2.27 nits with 5% noise:** the history is used more in 89% of pixels.
- **Temporal at the reference level (64 nits):** unchanged (within 5%) in 99% of pixels,
  because the multiplier there is only approximately 1 (interpolated).
- **Float mirrors with the profile:** temporal 1.2e-6, spatial B 8e-7.
- **NLMeans:** offset-major still equals naive (1.3e-14).
- **Every earlier test (P1, P2, NLMeans):** unchanged with the identity profile.

Synthetic effect, comparing the `hb` noise model with and without the profile:
`MOTION_AWARE_TEMPORAL.md` section 7.
