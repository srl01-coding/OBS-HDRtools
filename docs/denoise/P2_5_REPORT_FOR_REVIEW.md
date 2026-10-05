# P2.5 overnight report for the instructing AI

**Date:** 5 Oct 2026
**Repository:** `srl01-coding/OBS-HDRtools` (main)
**Covers:** the roadmap correction, the P2.5 amendment, and the user's 15-second sample clip.

Everything here is CPU-only. **None of it has run in OBS or on the user's GPU.**
Details and full tables are in `MOTION_AWARE_TEMPORAL.md`, `NOISE_MODEL.md` and
`NLMEANS_DESIGN.md`.

## 1. What was done

- **Roadmap documents recorded.** Both documents are filed in `docs/denoise/` and the
  roadmap is revised in `DENOISE.md`. The amendment governs P2.5.
- **NLMeans design, written before any code** (`NLMEANS_DESIGN.md`). It describes one
  engine:
  - candidates (dx, dy, dt), causal dt ∈ {0, -1};
  - luma-derived weights and full RGB reconstruction;
  - history policies A, B and H, with a share cap;
  - the HQDN3D cut and transition factor;
  - offset-major evaluation.
- **NLMeans CPU references** (`src/denoise/nlm-math.*`, `tests/cpu/test_nlm.cpp`). The
  naive and offset-major forms agree to 2e-14, and every invariant test passes.
- **NVOF capability probe** (`src/denoise/nvof-probe.cpp`, Windows, Development button).
  - It runtime-loads `nvcuda.dll` and `nvofapi64.dll` and logs, per device, whether the
    optical-flow engine is present, its grid sizes and limits, and the exported CUDA,
    D3D11 and D3D12 entry points.
  - The headers are vendored from NVIDIA's public GitHub repository (BSD-3).
  - **NOT RUN.**
- **Synthetic comparison** (`tools/denoise-sim`). It compares:
  - HQDN3D temporal and spatial;
  - spatial and temporal NLMeans, in several policies and configurations;
  - motion-compensated temporal with **oracle flow** (exact, 4x4 grid, 4x4 with 0.3 px
    error, and a zero-motion fallback).

  No optical-flow algorithm was written (roadmap section 20.8).
- **Measured noise model** from the sample clip (`NOISE_MODEL.md`).
- **Real-footage runs** on six crops of the clip.
- **Same-quality codec test**: x265 Main10 HLG, a CRF sweep, and BD-rate against a
  clean reference.
- **An independent re-check** of every reported number against the raw logs. Its
  corrections are applied.

## 2. Premises that changed

1. **The sample clip is a 20 Mbit/s HEVC recording, not a camera-level sample.** Its noise
   is post-encoder:
   - spatially low-pass and strongly correlated (lag-1 0.84 horizontal, 0.60 vertical);
   - about 0.7-0.9% relative luma noise at 100-300 nits;
   - temporally it is mostly codec block refresh on P frames.

   It bounds what survives the encoder. It cannot calibrate the denoiser. The user has
   been asked for a near-lossless OBS recording (NVENC HEVC 10-bit CQP 10-14).
2. **The production canvas is 4560 x 2160**, not 3840 x 2160. That is 19% more pixels for
   every performance estimate.

## 3. Findings that bear on the P2.5 plan (synthetic, oracle flow, caveats in the source doc)

| Question | Finding |
|---|---|
| Static content | Same-coordinate HQDN3D temporal is already as good as anything tested. MC with zero flow is identical. |
| Moving objects, 0.5-8 px/frame | MC error ratio 0.66-0.77, against 0.91-1.09 for every non-MC method on the measured noise. |
| Camera pan 3 px/frame | MC 0.50 (static-level), against 0.85-0.99 for the others. |
| 4x4 flow grid | No measurable loss for rigid translation. Non-rigid motion is untested. |
| Flow error of 0.3 px | Costs static denoise (0.50 -> 0.63). A zero-motion candidate restores it, at some cost in pans. |
| Sub-pixel recursive warping | Loss only on near-Nyquist synthetic texture. It is gone with 0.7 px optical blur. |
| Disocclusion and ghosting | The HQDN3D threshold response rejects revealed background for every method, with no occlusion detector. |
| **Temporal NLMeans (dt = -1)** | **At most 9% better than spatial NLM on moving objects, against 23-34% for MC. On correlated noise, no search variant reaches the static target. On white noise it behaves like spatial NLM. Cost not measured.** |
| Spatial filtering | Depends on noise correlation. It is nearly useless on the measured (correlated) noise and competitive on white noise. |
| Codec, BD-rate vs no denoise (object sequence / pan sequence) | Upper bound (clean input) -10.7% / -16.6%. MC -3.9% / -3.5%. HQDN3D -4.1% on the mostly static object sequence but **+3.4% in the pan**. Spatial and temporal NLM -0.5% to -1.9%, except TNLM B -3.7% on the object sequence. |
| Darks | No method denoises a 5-nit region at matched strength. Raising the comparison knee K to 2 nits does not help. |

The real-footage crops are second-generation (encoder residue), so they are indicative
only:
- HQDN3D temporal at strength 6.3 appears, by visual inspection, to remove structure
  from the speaker's slowly moving face;
- the dark piano region is untouched by every method;
- size changes are -2.9% to +1.0% apart from the flat wall (-5% to -14%).

## 4. Decisions requested

1. **Temporal NLMeans prototype (amendment P2.5A).**
   - The evidence does not support a GPU prototype now.
   - Proposal: deprioritise it, keep the generalised (dx, dy, dt) CPU engine, and revisit
     only if the pre-encode noise is near-white *and* NVOFA is unavailable.
   - This deviates from the amendment's sequence, so it is put to you rather than
     decided here.
2. **NVOFA path (P2.5B).**
   - Proceed after the probe confirms the engine on the GTX 1650 Super (TU116).
   - Design: warped history, then the HQDN3D threshold response as confidence, with a
     zero-motion candidate and flow cost.
   - Integration needs `nvOpticalFlowD3D11.h`, which is only in the full SDK download
     (NVIDIA account, SDK licence). Either the user downloads it, or the CUDA interface is
     used with D3D11-CUDA interop.
3. **Noise-profile threshold.** T(Y) = T0 · σ_rel(Y) / σ_rel(ref), shared by HQDN3D, MC
   and NLMeans, so the darks get denoised. This is a behaviour change, so it needs a
   decision.
4. **Strength scale.**
   - P1's default (luma 4) gives a wall error ratio of only 0.74 on the measured noise.
   - Defaults remain frozen until P1 has run on real footage, as instructed.

## 5. What the user needs to run (all NOT RUN)

1. P1 gates H1-H7 on real footage. H6 should check faces at the default strength.
2. Compute spike gates CS-G0 to CS7, and the spatial identity proof SP1.
3. NVOF probe NV-G0.
4. The near-lossless 15-30 s recording requested in `NOISE_MODEL.md`.
