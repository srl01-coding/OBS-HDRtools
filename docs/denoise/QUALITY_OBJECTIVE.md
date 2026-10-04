# Denoise quality objective

Source: `P2_DECISION_RESPONSE.md`, sections 2 and 27.

> The denoiser is intended not merely to make the local OBS preview cleaner, but to
> give a constrained downstream live encoder and YouTube transcode a temporally
> cleaner, more compressible 4K HDR source. The desired result is that bitrate is
> spent on meaningful scene structure rather than stochastic camera noise, while
> preserving enough real texture that the denoiser itself does not become the source
> of softness. Denoise quality is therefore evaluated both before encoding and after a
> controlled same-bitrate encode, with actual YouTube A/B testing where practical.

"Noise removed" is never the only quality metric.

## How it is evaluated

1. **Synthetic gates** (`docs/HQDN3D_DESIGN.md` section 8.5). Their job is
   repeatability, and they compare filters at matched noise reduction, not at equal
   slider values.
2. **Real footage** from the a6400/a6700, using the Difference view (`removed = input -
   output`, with gain).
   - The removed signal should be stochastic grain.
   - Faces, hair, fabric, text, architectural edges and highlight texture in the
     removed signal mean failure.
   - Check skin, hair, dark clothing, walls, fine text, specular detail, motion, cuts
     and dissolves.
3. **Codec A/B.** The same clips are encoded with identical HEVC Main10 settings close
   to the YouTube ingest constraint:
   - A: off;
   - B: HQDN3D light;
   - C: best HQDN3D spatial + temporal;
   - D: NLMeans (P3).

   Judge them on facial detail, hair, fabric, background texture, motion detail,
   gradients, blockiness, ringing, temporal smear, and the detail that survives.
4. **Clean-reference synthetic codec test.** The chain is: clean reference, then added
   noise, then denoise, then constrained encode/decode. The decoded result is compared
   with the *clean* reference (PSNR, SSIM, VMAF where available, edge width, residual
   noise). It is never compared with the noisy camera image.
5. **YouTube A/B** (unlisted or private, identical ingest settings) where practical.

## Selection rule (decision section 23)

Priority order:
1. OBS/HDR correctness;
2. no temporal or edge artefacts;
3. encoded/YouTube image quality;
4. useful denoise strength;
5. GPU cost;
6. portability.

The fastest mode on the GTX 1650 Super is not chosen for being the fastest. An RTX
3070-class card is a legitimate target for higher-quality modes.

Bitrate figures from YouTube's guidance are validation context only and are never
hard-coded in the denoiser.
