# Denoise placement: one core, two front-ends

Direction: `PLACEMENT_UPDATE.md` (instructing AI, 5 Oct 2026). Under it, NLMeans is
**parked** and placement is a product feature.

## Structure

```text
src/denoise/denoise-core.*    HDR DENOISE CORE
                              settings model (same obs_data keys everywhere)
                              spatial B -> frame metric -> temporal -> Output (Mix, debug views)
                              history, T(Y) noise profile, telemetry, shared effect
        |                                   |
src/denoise/denoise-filter.cpp     src/denoise/program-denoise.cpp
"HDR Denoise" source filter        "HDR Program Denoise" (Tools menu)
per camera / source                final composited program, main-rendered hook
```

There is no algorithm code in either front-end. The shader is the existing
`hdr-program-denoise.effect`, shared by both.

## HDR Denoise (source filter) `hdr_toolkit_denoise_filter_v1`, development schema

**Input.** The filter renders its own input (everything before it in the source's filter
chain) into a private texrender:
- in the source's native colour space, as RGBA16F on an HDR source;
- with the same blend and clear that libobs uses for filter inputs.

There is no decode, no clamp and no 8-bit stage.

**Processing.** The same core passes as the program processor: spatial (when on), then
temporal, then Output (Mix and debug views), into a core-owned output texture.

**Output.** The result is drawn with OBS's default effect, the same way libobs draws a
filter result: premultiplied, blend ONE / INVSRCALPHA, state pushed and popped.

**Once per frame.** OBS can render a source several times in one frame (program,
preview, multiview, projectors). Only the first render of a video frame
(`obs_get_video_frame_time`) processes. Later renders redraw the stored result, so the
history advances once per frame. The counters `unique_frames`, `reused_same_frame` and
`dispatches` prove it.

**Continuity.** If the source was not rendered for more than 2.5 frame intervals
(scene not on air and not in preview or multiview), its history is stale:
- it is reset, and the current frame passes through;
- the event is counted as `continuity_resets`;
- this is internal and not a user control.

The history also resets when the size, format or colour space changes, when the
settings become neutral, and with the *Reset history* button.

**Neutral settings.** With every strength at 0, no debug view and no forced passes, the
input is drawn unchanged with no pass at all. Identity through the shaders is proven
with Development > *Run spatial passes at strength 0* and the *Exact change* view (PL-G1).

**Defaults.** Same as the program processor, with one difference: **transition
protection is off by default**. A single camera has no program transitions. The option
remains, as "whole-frame change protection", for exposure or input changes. Scene-cut
reset stays on as a safety net (input switching, looping media).

**VRAM per instance at 3840 x 2160 RGBA16F, about 63 MiB per frame-size texture:**
- input texrender + 2 history + output = 4 frames, about 255 MiB (temporal only);
- add 2 for spatial, about 380 MiB.

The periodic log line reports it. Three sources with spatial need about 1.1 GiB of the
GTX 1650 Super's 4 GiB.

## HDR Program Denoise (unchanged behaviour)

The main-rendered hook, Identity and the compute spike are as before. HQDN3D now calls
the core: the frame is copied, the core runs, and Output writes into the main texture.
The algorithm, defaults and counters are unchanged.

## Recommended chain (user documentation)

```text
Camera source -> HDR Denoise -> HDR Color -> HDR Transform -> scene
```

- **Before Color:** a grade (especially a shadow lift) amplifies noise. Denoising first
  sees the camera's own noise-to-level relationship, which is what T(Y) is calibrated on.
- **Before Transform:** perspective and scaling resample the noise and correlate it
  spatially.
- **Before transitions:** the temporal history belongs to one physical camera.

Suggested arrangements to benchmark:
- **quality:** temporal + spatial per camera;
- **economy (priority on the GTX 1650 Super):** temporal per camera, plus one spatial pass
  in HDR Program Denoise (temporal 0, spatial on);
- **minimum:** program only.

The benchmark matrix and gates are in `docs/VALIDATION.md` (PL-G1 to PL-G7, PL-B).

## Not yet

- **NVOFA:** preferred at source level when it comes (PLACEMENT_UPDATE section 9).
- **Spatial cost.** The user measured about 20% GPU for one source. Optimising it is
  priority 3. The obvious first step is a pre-pass that computes F(Y) and the chroma
  residual once per pixel, instead of per tap (34 log2 calls per pixel today). Plugin
  GPU timings come first, so the gain can be measured.
