# Temporal vs spatial strength: the numbers are not comparable

Prompted by the user's observation (5 to 7 Oct, OBS 32.2.2, HDR Program Denoise):
- temporal looked much lighter than spatial at equal slider values;
- at the same debug gain, *What was removed* showed sparse speckle for temporal 2/2;
- spatial 2/2 showed dense noise plus scene structure (edges, panel seams, chair frames).

## Why

Both stages use the same threshold, T = 0.01 x S in log2 units (`kThresholdPerUnit`),
and the same response, W(x) = (1 - x^2)^2. They apply it to different evidence:

- **Temporal** has one candidate per pixel per frame: the history. On static content, the
  difference current - history has a standard deviation of about σ√(1 + r²), where r is the
  history's residual. Rejection keeps the history noisy, and a noisy history keeps the
  differences large. So at a low T the recursion settles where almost nothing is averaged.
  On static content its floor is set by the β = 0.9 cap: a residual of about 0.23σ in theory
  (0.27 to 0.28 measured below).
- **Spatial B** has 34 candidates (radius 8, H then V). Even when each tap is accepted with a
  low weight, the sum of weights is large. Accepted taps are, by construction, the ones
  whose values are close. That includes low-contrast texture and the soft edges of objects,
  which is why scene structure appears in its *What was removed* view.

## Measured (CPU reference, `tools/denoise-sim/strength_sim.cpp`)

**Setup.**
- Static flat patch, 128 x 128.
- Noise is Gaussian and independent from frame to frame. Its level comes from
  HIGH_BITRATE_ANALYSIS: 1.1% at 50 nits in the mids, 5.1% at 2.27 nits in the darks.
- Temporal: 120 frames, statistics over the last 60, g = 1.
- "Residual" is the output noise std divided by the input noise std. 1.00 means nothing was
  removed.
- "W" is the mean history weight, divided by 0.9.

**Identity noise profile.** ρ is the spatial lag-1 correlation of the noise.

| S | Mids, temporal W | Mids, temporal residual | Mids, spatial residual (ρ 0 / 0.6) | Darks, temporal residual | Darks, spatial residual (ρ 0 / 0.6) |
|---|---|---|---|---|---|
| 1 | 0.19 | 1.01 | 0.97 / 0.95 | 1.01 | 1.01 / 1.00 |
| 2 | 0.36 | 0.99 | 0.79 / 0.78 | 1.01 | 1.00 / 0.99 |
| 4 | 0.66 | 0.87 | 0.30 / 0.41 | 1.01 | 0.98 / 0.96 |
| 6 | 0.83 | 0.66 | 0.12 / 0.29 | 1.01 | 0.92 / 0.91 |
| 8 | 0.91 | 0.51 | 0.09 / 0.26 | 1.00 | 0.84 / 0.83 |
| 12 | 0.96 | 0.36 | 0.07 / 0.25 | 0.96 | 0.61 / 0.63 |
| 20 | 0.99 | 0.28 | 0.07 / 0.24 | 0.82 | 0.23 / 0.36 |

**Measured noise profile "measured 2026-10-05" (max multiplier 3), ρ = 0.**

| S | Mids, temporal | Mids, spatial | Darks, temporal | Darks, spatial |
|---|---|---|---|---|
| 2 | 0.98 | 0.73 | 1.01 | 0.92 |
| 4 | 0.81 | 0.22 | 0.96 | 0.61 |
| 6 | 0.59 | 0.10 | 0.86 | 0.30 |
| 8 | 0.46 | 0.08 | 0.72 | 0.16 |
| 12 | 0.34 | 0.07 | 0.51 | 0.09 |

## Consequences

1. **Strength 2 temporal does essentially nothing** on this camera's noise: 1-2% removed in
   the mids, nothing in the darks. To engage it on the mids, temporal needs about S 6-8. In
   the darks it needs the measured profile, or S of 12 or more.
2. **Temporal cannot match spatial on flat areas at any strength.** The β cap is a deliberate
   trail limit (design section 3). Spatial's lower residual on flat areas is paid for with
   texture and edge change, which the temporal stage does not cause on static content.
3. **Comparing the two stages at equal slider values is not an equal-strength comparison.**
   Compare them at equal residual on a flat static area, then judge texture and motion.

## Open (decision for the user / instructing AI, not implemented)

- A noise-referenced strength scale, so that S means the same residual in both stages, for
  example a temporal T of about 3x the spatial T per unit. This changes the meaning of
  existing settings: development schema only, but saved scenes would change.
- A β cap above 0.9 (0.95 gives about 0.16σ in theory). This trades a stronger static
  result for longer trails on slow motion. It belongs with the motion-compensation work
  (NV-G0), not before it.

## Spatial-only processing skips the temporal passes

When both temporal strengths are 0, no temporal debug view is selected and forced passes are
off, the core skips MetricBlocks, MetricReduce, MetricFinal and Temporal. Output then takes the
spatial result directly.

Before this change, a spatial-only instance still ran a full-frame temporal pass, two
full-frame reads and one write, at 4560 x 2160. This mattered for the hybrid's
program-level spatial instance.

The skipped frames are counted as `temporal_skipped`. History restarts when temporal is set
again. Forced passes keep the temporal identity coverage of PL-G1 and PL-G2. Gate: PL-G8.

## Darks: the comparison knee is the noise floor (10 Oct)

The user reported (10 Oct) that temporal acts on the lighter areas but barely touches dark
ones, such as the black piano. Two causes.

**1. The comparison domain over-weights dark noise.** F = log2(1 + Y/K) with K = 0.1 nits
treats noise as purely proportional to Y. The camera's noise is not. In nits, the measured σ
(NOISE_PROFILE.md) is:

| Y (nits) | 2.27 | 35.6 | 64.4 | 89.4 |
|---|---|---|---|---|
| σ (nits) | 0.117 | 0.488 | 0.683 | 0.796 |

That fits σ ≈ a(Y + K) with K ≈ 8 nits, an additive floor plus a proportional part. For that
noise law, log2(Y + K) is the domain in which noise has the same size at every level.

σ_F relative to the wall:
- K = 0.1: 4.65 / 1.29 / 1.00 / 0.84;
- K = 8: 1.20 / 1.19 / 1.00 / 0.87.

The measured T(Y) profile compensates only down to its first anchor, and is clamped to max 3.
Below 2.27 nits it is flat, while the true requirement keeps rising.

**Simulated temporal residual** (`tools/denoise-sim/knee_sim.cpp`):
- static, noise independent from frame to frame, σ as measured;
- the 0.5-nit σ is extrapolated from the fit, so treat that column as an estimate;
- identity profile.

| K | S | 0.5 n | 2.27 n | 35.6 n | 64.4 n | 89.4 n |
|---|---|---|---|---|---|---|
| 0.1 | 8 | 0.99 | 0.98 | 0.60 | 0.48 | 0.41 |
| 0.1 | 12 | 0.99 | 0.94 | 0.42 | 0.35 | 0.31 |
| 4 | 8 | 0.83 | 0.76 | 0.55 | 0.45 | 0.40 |
| 8 | 8 | 0.51 | 0.51 | 0.50 | 0.43 | 0.39 |
| 8 | 12 | 0.37 | 0.37 | 0.36 | 0.32 | 0.30 |

With K = 8 the result is level-independent. Above about 30 nits the mids change little, since
for Y >> K differences are unaffected.

**Cost.** Below K the threshold is effectively absolute: about T x K x ln 2, which is 0.44 nits
at S = 8. Dark detail or motion with less contrast than that is averaged, so a dark sleeve
moving across the black piano can trail. That is inherent: the dark noise itself is about
0.12 nits (5% at 2.27 nits), and no per-pixel test separates detail from noise of the same
size. Motion compensation (NV-G0 path) is the real answer for dark motion.

**Changes.**
- The knee range is now 0.01-20 nits (UI) and 0.001-20 (sanitize). The default stays 0.1:
  existing instances do not change.
- K is shared by temporal and spatial.
- Do **not** combine K ≈ 8 with the measured profile: that profile was derived at K = 0.1 and
  would compensate twice. Use the identity profile with K ≈ 8.
- Debug view *What was removed, noise-normalised*: |F(out) - F(in)| on luma x gain. The
  linear *What was removed* view under-shows the darks, because dark noise is small in nits
  (0.12 vs 0.68). At K ≈ 8, equal noise removal reads equally bright at every level.

The fit comes from four ROIs on one clip and one camera, so it is not frozen. Gate KN-1.

## Extended range 0-100 (10 Oct, user-directed)

The user asked for strengths reaching "totally plasticized". All four strengths now go from
0 to 100. Settings from 0 to 20 behave exactly as before, so no migration is needed.

- **Threshold.** T = 0.01 x S continues linearly to 1.0 log2 at S = 100. At that point,
  differences within a factor of about 2 are treated as noise, and texture and soft detail
  are flattened. Spatial reaches its plastic look from the threshold alone. The radius (up
  to 12) sets the scale of the smoothing.
- **Temporal history cap.** The threshold alone cannot make temporal plastic, because the
  0.9 history cap limits a static scene to about 0.23σ at any strength. Above S = 20 the cap
  rises: 1 - β falls geometrically from 0.1 at S = 20 to 0.01 at S = 100 (`history_beta`).

  | S | ≤ 20 | 40 | 60 | 80 | 100 |
  |---|---|---|---|---|---|
  | β | 0.900 | 0.944 | 0.968 | 0.982 | 0.990 |
  | Memory time constant 1/(1-β), frames | 10 | 18 | 32 | 56 | 100 |
  | Static residual √((1-β)/(1+β)) | 0.23 | 0.17 | 0.13 | 0.09 | 0.07 |

  Expect long trails and ghosting on motion at high S. Scene-cut reset and protection
  still apply.
- **Precision.** With 1 - β = 0.01, a half-float history cannot move on differences below
  about 2.4% (half an ulp divided by 1 - β) and would stall. Above S = 20 the history is
  therefore kept in RGBA32F:
  - 2 x 127 MiB at 3840 x 2160, instead of 2 x 63 MiB;
  - switching across 20 restarts the history;
  - VRAM reporting includes it.
- **Tests (CPU).** Mirror equivalence with random S over 0-100; β monotone; static noise at
  S = 100 measured 0.069 against the IIR limit of 0.071. Gate SR-1.
