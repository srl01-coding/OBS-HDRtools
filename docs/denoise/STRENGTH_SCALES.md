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
