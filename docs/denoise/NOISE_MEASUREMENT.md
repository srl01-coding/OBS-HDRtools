# Live noise measurement (HDR Denoise filter)

User-directed (10 Oct 2026). The noise floor of about 8 nits in `STRENGTH_SCALES.md` is a fit to
four regions, measured by the instructing AI on one HEVC recording. This tool measures the live
camera at the gain and lighting actually used.

## Use

1. **Set up the shot.** HDR Denoise > *Noise measurement*. Point the camera at a static scene
   with dark and bright areas: no movement, auto exposure and auto ISO/gain off, the real
   lighting.
2. **Press *Measure noise*.** The filter samples 60 frames of its own input, about 2 s at
   29.97 fps. The picture is unchanged while it measures.
3. **Read the result.** It appears in the panel and as a full table in the OBS log, prefixed
   `[denoise:<filter name>]`.
4. **Press *Apply measured noise floor*.** This sets *Noise floor (comparison knee)* to the
   fitted value, clamped to 0.01-50 nits. It also sets the noise profile to identity, because
   the measured T(Y) profile assumes K = 0.1 and would compensate the darks twice.

Measure again whenever the camera's gain, exposure or lighting changes.

## Method

- **Sampling (GPU).** Technique `SampleGrid` in `hdr-program-denoise.effect`, which renders
  into an RGBA32F texture. Each grid cell takes one input pixel by `Load`, with no filtering,
  so per-pixel noise is intact. The grid step is ceil(sqrt(W x H / 300 000)): step 6 at
  4560 x 2160, giving 760 x 360 samples.
- **Readback.** Through 3 stage surfaces, read two frames late so the GPU never stalls. The
  CPU work is about 0.3 M samples per frame on the graphics thread, during the 2 s only.
- **Analysis.** `noise-meter-math.cpp`, with CPU tests in `tests/cpu/test_noise_meter.cpp`.
  - **Luma:** Rec.709 weights on working RGB, times nits per unit (OBS's SDR white level on an
    HLG/PQ canvas). This is the luma the denoiser itself compares.
  - **Flicker:** each frame is divided by its global mean relative to the first frame. The
    std and range of that gain are reported. A warning is given if the range exceeds 2%
    (exposure or lighting change).
  - **Per pixel:** temporal mean and standard deviation over the 60 frames.
  - **Bands:** quarter-stop brightness bands from 1/64 to 4096 nits, each needing at least
    200 pixels. The band's σ is the median over its pixels, which is robust to a minority of
    moving or edge pixels. The share of pixels above 4x their band median is reported as
    motion; a warning is given above 5%.
  - **Fit:** K is chosen on a log grid from 0.01 to 100 so that log(σ / (Y + K)) has the
    least weighted variance (weight √count). One robust pass then drops bands more than 1.6x
    off and refits. Reported for comparison: the spread of σ_F at the fitted K and at 0.1.
  - **Refusals:** fewer than 3 bands, or under 2 stops of range, gives no fit, because the
    floor cannot be separated from the proportional part.
  - **Suggested temporal strength:** S ≈ 5.5 σ_F / 0.01, the strength that leaves about half
    the noise according to the `STRENGTH_SCALES.md` simulations (independent noise,
    static). It is guidance only.

## Limits

- **Measures this filter's input.** Put the filter first on the camera, so the input is the
  camera's noise before grading or scaling.
- **The fitted model is σ = a (Y + K).** Pure shot noise (σ ∝ √Y) fits it imperfectly. The
  CPU test with shot plus read noise gives a 2x residual spread, and the report shows it.
- **Temporal statistics only.** Spatial correlation is not measured.
- **Program processor not wired.** The program frame mixes graphics and several cameras.
- **8-bit SDR input is refused.**

Gate: NM-1 (NOT RUN).
