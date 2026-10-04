# Measured noise: the 4 Oct 2026 sample clip

Source: `clip_01-29-00_15s_3.mkv`, supplied by the user. It is a church-service shot with
a locked-off camera, a speaker at a lectern, audience heads in the foreground and a
slide overlay. The clip itself is **not** in the repository, and no frames or crops of
it are either (it shows real people). Only aggregate numbers are recorded here. The
analysis scripts live outside the repository; their method is described below.

## The file

| Property | Value |
|---|---|
| Video | HEVC Main 10, 4560 x 2160, 29.97 fps, 479 frames (16 s), yuv420p10le, narrow range |
| Colour | BT.2020, ARIB STD-B67 (HLG), mastering metadata 1000 nits |
| GOP | IBBP..., an I frame every 59 frames (about 2 s) |
| Rate | 20.2 Mbit/s average |
| Frame sizes | I 506 kB, P 195 kB, B 13.7 kB on average |
| Content | Frames 0-14 are the end of a dissolve (a slide overlay fades out); the rest is a static wide shot with a moving speaker and audience |

The canvas is **4560 x 2160**, not 3840 x 2160. That is 19% more pixels than the 4K
figures used in the performance estimates so far.

## Critical premise: this is post-encoder noise

This file is an encoded output, recorded at 20 Mbit/s. What it contains is camera noise
*after* the encoder has quantised it. It is not the noise the denoiser will see at the
program texture, and two things in the data show this:

- **B frames average 13.7 kB at 4.5K (0.003 bit/pixel).** The encoder codes almost
  nothing in them, so noise in B frames is largely copied or interpolated from the
  reference frames. Frame-to-frame differences on the static wall alternate with the
  P-frame cadence: B-frame row profiles change by about 0.03 codes, P frames by about
  0.1.
- **The noise spectrum is strongly low-pass.** Power falls by 100x between 0 and 0.4
  cycles/pixel. Noise that survived DCT quantisation is low-frequency by construction.

The measurements are therefore:
- an **upper bound on correlation** and a **lower bound on amplitude**, compared with
  the pre-encode noise;
- direct evidence of what the encoder does with the noise, which is the quality
  objective.

A pre-encode sample is still needed to calibrate the denoiser. Its OBS recording
settings are listed at the end of this file.

## Method

1. Only I frames 1-8 are used. Frame 0 is mid-dissolve, and I frames are the
   best-coded frames, with an independent noise realisation every 2 s.
2. **Static blocks.** The frame is split into 40 x 40 blocks. A block is static if its
   mean never departs from the 8-frame median by more than 1 code; 49% of blocks
   qualify.
3. **Flat blocks.** Of the static blocks, a block is flat if the mean |I_mean - blur(I_mean)|
   is below 0.75 codes; 7.8% of all blocks qualify, mostly the wall and floor.
4. **Noise.** For consecutive I-frame pairs, σ = std(I_k - I_{k+1}) / √2, after
   removing each 40 x 40 block's mean difference (this removes exposure drift).
5. **Conversion to light.** Codes become display luminance through the HLG inverse
   OETF and a 1.2 system gamma at 1000 nits, which is OBS's working scale (75% = 203
   nits). σ in nits is σ_code x dY/dcode. σ_F is in the denoiser's comparison domain
   (F = log2(1 + Y/K), K = 0.1).

## Results

### Luma noise against level (flat static regions, I frames)

| Code (mean) | Luminance | σ (codes) | σ (nits) | σ / Y | σ_F | Pixels |
|---|---|---|---|---|---|---|
| 382 | 23 nits | 3.07 | 0.54 | 2.3% | 0.033 | 4.7 k |
| 635 | 114 nits | 1.36 | 1.01 | 0.89% | 0.013 | 52 k |
| 666 | 139 nits | 1.29 | 1.21 | 0.87% | 0.013 | 108 k |
| 726 | 211 nits | 1.25 | 1.85 | 0.88% | 0.013 | 5 k |
| 767 | 280 nits | 1.01 | 2.01 | 0.72% | 0.010 | 588 k |
| 808 | 378 nits | 0.70 | 1.93 | 0.51% | 0.007 | 8 k |

- Relative noise is about 0.7-0.9% from 100 to 300 nits, and rises to about 2.3% at
  23 nits.
- In the comparison domain that is σ_F ≈ 0.010-0.013, rising to 0.033 in the darks.
  The synthetic gates used 2% per channel (σ_F ≈ 0.022 on luma), so the encoded noise is
  about half of that at mid levels.
- Only one flat region is darker than 100 nits (23 nits). The dark piano and clothing
  are not flat, so dark-level statistics are thin.

### Chroma

| Plane | σ (codes) |
|---|---|
| Cb (4:2:0) | 0.81 |
| Cr (4:2:0) | 0.59 |
| Luma, same regions | 1.13 |

The luma/chroma correlation of the noise is -0.04, effectively independent.

### Spatial correlation (luma noise autocorrelation)

| Lag (px) | Horizontal | Vertical | Diagonal |
|---|---|---|---|
| 1 | 0.84 | 0.60 | 0.49 |
| 2 | 0.65 | 0.26 | 0.08 |
| 3 | 0.50 | 0.21 | 0.04 |
| 4 | 0.43 | 0.15 | 0.02 |
| 8 | 0.32 | 0.04 | -0.02 |

Chroma (Cb, half resolution): 0.83/0.79 at lag 1 and 0.25/0.17 at lag 4. In luma pixels
that is about twice the reach.

The noise is **strongly horizontally correlated**. About 27% of the variance of each
40 x 40 block's difference sits in row means, against 2.5% for white noise. This is
**not lighting flicker**: the wall's row profile changes by only about 0.1 code from
frame to frame. The likely causes are horizontal resampling (a 3840-wide camera image on
a 4560-wide canvas, if that is how the scene is built) combined with encoder
quantisation. The data cannot separate the two.

## Consequences for the denoise work

1. **Synthetic noise must be correlated.** White noise at 2% made the spatial radius
   (6/8/12) and the recursive A filter look irrelevant. With correlation reaching 3-8 px
   horizontally, a larger support and A's IIR propagation should matter. The synthetic
   harness therefore uses both white noise and a correlated model fitted to the table
   above.
2. **Spatial filtering alone cannot remove correlated noise efficiently.** Low-frequency
   noise blobs look like texture to an edge-aware filter. This favours temporal methods,
   which see a fresh realisation every frame, consistent with the P2.5 direction.
3. **The encoder already spends bits on noise.** P frames are 14x the size of B frames
   on a static shot, and I frames 37x. On a static scene those bits largely encode
   noise. That is the measurable target for the codec test: bits saved at equal quality
   against a clean reference.
4. **The canvas is 4560 x 2160.** Performance budgets should be scaled by 1.19 relative
   to 3840 x 2160.

## Requested: a pre-encode sample

To calibrate against the noise the denoiser actually sees, record 15-30 s of the same
scene through OBS at near-lossless quality:
- Settings > Output > Recording, Advanced mode;
- encoder NVIDIA NVENC HEVC, 10-bit (P010), Rec.2100 HLG as in production;
- rate control **CQP**, CQ level **10-14**, keyframe interval 1 s.

Include a static wide shot (a wall or grey card filling part of the frame), some
movement, and if possible a dark area. A file of a few hundred MB is expected. With
denoise off, this is the input-side counterpart of this clip.
