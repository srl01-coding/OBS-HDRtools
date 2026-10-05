# P2.5: premium temporal denoise, design notes and CPU findings

The governing documents are `ROADMAP_CORRECTION_MOTION_AWARE.md` and `P2_5_AMENDMENT.md`;
the amendment takes precedence where they differ. There are three candidates:
- **HQDN3D temporal**, the same-coordinate baseline (P1);
- **Temporal NLMeans**, with causal (dx, dy, -1) patch candidates (`NLMEANS_DESIGN.md`);
- **NVOFA motion-compensated temporal**: optical flow, warped history, confidence.

Status: everything below was run on the CPU only. There is no GPU and no NVIDIA
hardware here.
- NVOFA is represented by **oracle flow**: the exact motion of the synthetic fixtures,
  optionally quantised to a 4x4 grid or given random error. Oracle flow bounds what
  hardware flow could achieve. It is not an optical-flow implementation, and none was
  written (roadmap section 20.8).
- The NVOF capability probe (`src/denoise/nvof-probe.cpp`, Development > *Probe NVIDIA
  optical-flow hardware*) answers question 9 of the roadmap on the user's machine. It
  has NOT been RUN yet.

## 1. Motion-compensated temporal: the design tested

```text
H_t(x) = sample(D(t-1), x + flow(x))       (bilinear / Catmull-Rom / Lanczos-3)
D_t    = HQDN3D temporal recurrence (HQDN3D_DESIGN.md 3) of C_t against H_t
         - the same comparison domain, threshold response, beta = 0.9 cap,
           luma/chroma split
         - the same frame metric / cut reset / transition protection, computed
           against H_t
samples whose source x + flow lies outside the frame: no history (identity)
```

Disocclusion therefore needs no separate detector. Revealed background differs from the
warped foreground history by more than the threshold, so its weight is 0. Optional
variant: a **zero-motion candidate**. The unwarped history is also kept, and whichever
of the two matches the current frame better over a 3x3 patch is used.

## 2. Synthetic comparison (`tools/denoise-sim`, 2026-10-05)

**Content.** The fixtures are procedural, at 192 x 144 for 36 frames (first 12
excluded). They contain:
- a "wall" of 140 nits with 2-4% texture, close to the noise level;
- 10% fabric texture;
- 250-nit graphics with 1-2 px dark strokes;
- skin-like 110-nit texture;
- a 5-nit dark band;
- small 700-nit highlights.

**Noise models.**
- **enc**: the measured post-encoder noise (`NOISE_MODEL.md`). It depends on level
  (about 0.9% at 100-300 nits, 2.3% at 23 nits), is strongly correlated (lag-1 0.84
  horizontal, 0.60 vertical), and chroma is 0.6 of luma.
- **white2**: white noise at twice that amplitude. It stands in for the unknown
  pre-encode noise.

**Matching.** Each method's strength S is the smallest value that brings the **wall**
error to 0.50 of the input noise. Error is measured against the clean frame in the
comparison domain F, so texture loss counts as error. A method marked * cannot reach
0.50 at any S, and its best S is shown instead. Full tables: run `denoise_sim --noise
enc|white2`.

### Static scene (error ratio, lower is better)

| Method | enc: S | enc: wall | enc: fabric | enc: skin | white2: S | white2: wall | white2: fabric | white2: skin |
|---|---|---|---|---|---|---|---|---|
| HQDN3D temporal | 6.3 | **0.50** | 0.79 | 0.52 | 12.3 | 0.50 | 0.80 | 0.53 |
| HQDN3D spatial B + temporal | 4.0* | 0.61 | 0.96 | 0.78 | 6.0 | 0.50 | 0.96 | 0.73 |
| NLMeans spatial 3x3/7x7 | 1.5* | 0.89 | 1.00 | 0.92 | 2.7 | 0.50 | 0.99 | 0.59 |
| Temporal NLM A 3x3, s7 t7 | 2.0* | 0.83 | 0.94 | 0.88 | 2.3 | 0.50 | 0.99 | 0.60 |
| Temporal NLM H 5x5, s7 t7 | 1.5* | 0.85 | 0.98 | 0.87 | 2.4 | 0.50 | 0.99 | 0.56 |
| Temporal NLM B 3x3, s7 t3, gain 9 | 1.5* | 0.73 | 0.91 | 0.78 | 1.6 | 0.50 | 1.00 | 0.62 |
| MC, any exact/4x4 oracle flow | 6.3 | **0.50** | 0.79 | 0.52 | 12.3 | 0.50 | 0.80 | 0.53 |
| MC, 4x4 + 0.3 px flow error | 8.0* | 0.63 | 0.99 | 0.75 | 16* | 0.53 | 0.86 | 0.61 |
| MC, 4x4 + 0.3 px error + zero-motion candidate | 6.9 | **0.50** | 0.77 | 0.55 | 12.0 | 0.50 | 0.82 | 0.54 |

### Moving 64 x 64 textured object (object error ratio; speed in px/frame)

| Method (enc) | 0.5 | 1 | 2 | 4 | 8 | 16 |
|---|---|---|---|---|---|---|
| HQDN3D temporal | 0.97 | 1.04 | 1.05 | 1.09 | 1.02 | 0.99 |
| NLMeans spatial | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 |
| Temporal NLM A | 1.03 | 1.01 | 1.01 | 1.07 | 1.05 | 1.05 |
| Temporal NLM H 5x5 | 0.96 | 0.94 | 0.94 | 0.99 | 0.99 | 0.99 |
| MC exact flow (bicubic) | 0.77 | 0.68 | 0.70 | **0.66** | 0.75 | 0.91 |
| MC 4x4 grid | 0.77 | 0.69 | 0.71 | 0.66 | 0.75 | 0.91 |
| MC 4x4 + 0.3 px error | 0.92 | 0.91 | 0.92 | 0.91 | 0.92 | 0.92 |

| Method (white2) | 0.5 | 1 | 2 | 4 | 8 | 16 |
|---|---|---|---|---|---|---|
| HQDN3D temporal | 0.85 | 0.93 | 0.97 | 0.99 | 0.98 | 0.98 |
| NLMeans spatial | 0.85 | 0.85 | 0.85 | 0.85 | 0.86 | 0.86 |
| Temporal NLM A | 0.86 | 0.86 | 0.86 | 0.88 | 0.90 | 0.91 |
| MC exact flow (bicubic) | 0.70 | 0.68 | 0.68 | 0.68 | 0.72 | 0.88 |
| MC 4x4 + 0.3 px error | 0.78 | 0.78 | 0.78 | 0.77 | 0.80 | 0.81 |

### Disocclusion, pan, cut

- **Disocclusion trail.** This is background uncovered within the last 6 frames. Every
  method scored ≤ 1.00 (0.92-1.00) at every speed, so none ghosts. The
  threshold-response rejection handles revealed background without an occlusion
  detector, and that includes MC.
- **Pan 3 px/frame (enc), wall error.**
  - MC: 0.50, the full static performance.
  - HQDN3D temporal: 0.97.
  - Temporal NLM: 0.85-0.89.
  - Spatial NLM: 0.89.
- **Pan 0.5 px/frame.**
  - MC: 0.80 on this content. The cause is the fixture's near-Nyquist texture: Lanczos-3
    does not help (0.81), but blurring the clean world by σ = 0.7 px gives 0.51, and
    σ = 1.2 px gives 0.50.
  - Camera images with optical MTF, demosaicing and the 3840 -> 4560 resampling are
    softer than that. Sub-pixel recursion should therefore not cost real footage much.
    This needs confirming on footage.
- **Hard cut** (a distinct second scene).
  - Every temporal method gives an exact identity on the first frame (ratio 1.00).
  - Spatial-only methods keep filtering spatially (0.94-0.97).
  - No method mixes in the previous scene.

## 3. What the comparison says (CPU, synthetic, oracle flow)

1. **For static content, same-coordinate temporal is already the best tool.** MC with
   zero flow reduces exactly to HQDN3D. Nothing tested beats HQDN3D temporal on static
   content at equal texture cost.
2. **Only motion compensation keeps temporal denoise on moving content.**
   - Moving objects at 0.5-8 px/frame: MC 0.66-0.77, against 0.94-1.09 for every
     non-MC method on the measured noise.
   - A 3 px/frame pan: 0.50 against 0.85-0.97.
   - The 4x4 grid costs nothing measurable on rigid motion.

   This is the headline result in favour of P2.5B.
3. **Temporal NLMeans (dt = -1 patch search) did not track motion in a useful way.**
   - With a 7x7 temporal window it averages many approximate matches rather than the one
     true correspondence.
   - On content whose texture is near the noise level, it blurs that texture before it
     removes correlated noise. It never reached the static target on the measured noise,
     and on moving objects it gained at most 6%.
   - On white noise it behaves like spatial NLMeans plus extra candidates.
   - A temporal-only, same-position variant ("t1 only, gain 9") reduces to a patch-weighted
     HQDN3D, with no motion benefit.

   On this evidence Temporal NLMeans is not the premium-temporal candidate. That holds
   unless real pre-encode footage contradicts it (section 5).
4. **Noise correlation decides whether spatial filtering is worth anything.**
   - On the measured (encoded, correlated) noise, no spatial method could reach a 0.50
     wall error without destroying texture first (best 0.61-0.89).
   - On white noise, spatial NLMeans reaches it, and in pans it matches MC (0.49-0.51).
   - The pre-encode noise character (`NOISE_MODEL.md`, requested sample) therefore
     decides the spatial stage. NLMeans should stay a **residual spatial cleaner for moving
     regions** rather than the premium mode.
5. **Flow error needs a zero-motion fallback.**
   - Random 0.3 px flow error cost static denoise (0.50 -> 0.63).
   - Keeping the unwarped history as a second candidate restores static (0.50), but
     costs part of the pan gain (0.67 -> 0.78).
   - A production MC design should carry both candidates and use flow confidence
     (NVOF cost output) to choose between them.
6. **Darks are not denoised at any matched strength.** Dark-zone error ratios were
   0.98-1.00. With K = 0.1 nit, noise at 5 nits is larger in F than the thresholds
   (σ_F ≈ 0.06), and the real footage's piano region shows the same. Raising the
   comparison knee K (for example to 0.5-1 nit) is a tuning question for P1/P2 on real
   footage.

## 4. Real footage (sample clip, CPU, second generation)

`tools/denoise-sim/real_eval.py` ran the CPU references on six crops of the sample clip:
wall with cross, slide text, dark piano, drum kit / music stand, audience (backs of
heads) and the speaker. The strengths were the matched values above.

The clip is a 20 Mbit/s HEVC recording, so its "noise" is largely encoder residue:
- the temporal component is HEVC block structure, refreshed on P frames;
- the removed-signal images of the temporal filters show exactly those 8-32 px blocks.

These results therefore show how the filters treat **encoder artefacts**. They do not
show camera noise.

Results are in section 6.

## 5. Open questions that only the user's machine or new footage can answer

1. **NVOF capability on the GTX 1650 Super (TU116).** Run the probe. The log answers:
   - whether the optical-flow engine is present;
   - the grid sizes;
   - the maximum API version;
   - whether a D3D11 entry point is exported.
2. **Pre-encode noise.** The near-lossless recording requested in `NOISE_MODEL.md`
   decides whether spatial filtering has any value (finding 4).
3. **Flow quality on real motion** (faces, hands, hair) and its cost at 4560 x 2160.
   This needs the D3D11 integration. That in turn needs `nvOpticalFlowD3D11.h` from the
   full SDK, which is not in NVIDIA's public GitHub repository (see
   `third_party/nvof/README.md`), and the compute spike to have passed.

## 6. Real-footage and codec results

### 6.1 Sample clip crops (CPU references, strengths as matched in section 2)

Columns:
- **CRF 18 size**: the change in libx265 Main10 HLG file size against the input crop,
  120 frames.
- **flat t-std**: the temporal standard deviation of F(Y) on flat pixels, output / input.
- **edge/flat**: removed-signal rms on edge pixels divided by that on flat pixels.
  Values well above 1 mean structure is being removed.

| Crop | HQ temporal | HQ spatial + temporal | NLM spatial | TNLM A | TNLM B s7 t3 g9 | TNLM t1 only g9 |
|---|---|---|---|---|---|---|
| wall + cross: size / flat t-std | -5.5% / 0.84 | -11.1% / 0.70 | -7.4% / 0.80 | -11.8% / 0.72 | -14.2% / 0.67 | -8.1% / 0.81 |
| slide text: size | 0.0% | -0.6% | -0.7% | -0.6% | -0.2% | +0.3% |
| dark piano: size / flat t-std | -0.2% / 1.00 | +0.8% / 1.00 | +1.0% / 1.00 | -0.9% / 0.99 | +0.2% / 0.99 | +0.8% / 1.00 |
| drums / music stand: size | +0.1% | -2.2% | -1.2% | -2.9% | -2.0% | -0.4% |
| audience: size | -0.2% | -0.3% | -1.1% | -1.1% | -1.5% | -0.5% |
| speaker: size / edge-flat | -0.2% / **1.24** | -0.6% / 0.83 | -0.9% / 0.69 | -2.0% / 0.75 | -1.2% / 0.77 | +0.4% / 1.09 |

Observations. These are second-generation material, so read them as direction only.
- **The removed signal of the temporal filters on static areas is HEVC block structure.**
  This confirms that the clip's temporal "noise" is encoder residue, refreshed on P
  frames.
- **On the speaker, HQDN3D temporal at the matched strength (6.3) removes facial
  structure.** The removed-signal image shows the moving face and shirt (edge/flat 1.24).
  Slow, low-contrast motion stays below the threshold and is averaged with history. That
  is the smear motion compensation exists to prevent.
  - Spatial NLM's removed signal on the same frame is noise-like (0.69).
  - P1's default temporal luma strength is 4, not 6.3. The real-footage gate H6 should
    check faces at the default strength.
- **Darks (piano) are untouched by every method.** This is finding 6 in section 3: at
  K = 0.1 nit, dark noise exceeds the thresholds.
- **Bit savings are small (0-3%)** except on the flat wall (5-14%). The encoder in the
  original recording has already removed most noise.

### 6.2 Same-quality codec test against a clean reference (decision section 17)

`codec_test.py`:
- 384 x 256, 60 frames;
- libx265 Main10 HLG at CRF 14-34;
- PSNR of the 10-bit HLG luma against the **clean** sequence;
- BD-rate against no denoise, meaning the bitrate change at equal PSNR to clean.

| Method | Object 2 px/frame, measured (encoded) noise | Pan 1 px/frame, white noise at 2x |
|---|---|---|
| perfect denoise (clean input), upper bound | -10.7% | -16.6% |
| HQDN3D temporal | **-4.1%** | **+3.4%** (worse than none: lag in the pan) |
| HQDN3D spatial + temporal | -2.8% | -0.5% |
| NLMeans spatial | -1.2% | -1.2% |
| Temporal NLM A | -1.9% | -1.6% |
| Temporal NLM B s7 t3 g9 | -3.7% | -0.8% |
| MC, exact or 4x4 oracle flow | -3.9% | **-3.5%** |
| MC 4x4 + 0.3 px error | -3.0% | -3.1% |
| MC 4x4 + 0.3 px error + zero-motion candidate | -3.6% | -2.2% |

Reading:
- **MC is the only method that is good in both sequences.** HQDN3D temporal is the best
  on a mostly static scene, and harmful in a pan at the same strength.
- **The real filters capture a quarter to a third of the available gain.** Perfect
  denoising would save 11-17% here, and they save 3-4%.
- **Scale caveat.** The noise levels here are small next to the coding error (pre-encode
  PSNR 54 dB). Real 4K camera noise before encoding (requested sample) is likely larger,
  and so is the available gain.
- **Limits of the test.** Small frames, procedural content, x265 and PSNR stand in for
  YouTube's own transcode and a viewer. This is directional evidence for the decision
  section 23 ordering, not a measurement of YouTube quality.

### 6.3 Recommendation for the P2.5 decision gate (to be confirmed on the user's machine)

1. **Pursue P2.5B (NVOFA motion-compensated temporal) as the premium-temporal
   candidate.** Run the capability probe first (NV-G0).
   - Design: warped history, the HQDN3D threshold response for confidence, the frame
     metric against the warped history, a zero-motion candidate, and flow cost used to
     pick between the candidates.
   - The 4x4 grid was not a limitation on rigid motion in these tests.
2. **Do not build a GPU Temporal NLMeans prototype on this evidence.** It showed no motion
   benefit over spatial NLMeans at a higher cost. Revisit only if the pre-encode sample
   shows near-white noise *and* no NVOFA is available.
3. **Keep spatial NLMeans as the residual cleaner for moving regions**, sized by the
   pre-encode noise. If that noise is as correlated as the encoded sample, spatial
   filtering of any kind is of little value.
4. **Before choosing P1 defaults,** check faces at the default temporal strength (H6) and
   test a comparison knee K of 0.5-1 nit for the darks.
