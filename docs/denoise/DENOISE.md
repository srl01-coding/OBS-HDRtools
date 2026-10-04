# HDR Program Denoise - design notes

Governing spec: `docs/denoise/BRIEF_v1.2.md` (the file's own heading says v1.1; the
user supplied it as v1.2). Status: **P0 implemented, runtime gates NOT RUN.**

## P0: where the program frame comes from (verified in OBS 32.2.2 source)

`libobs/obs-video.c`, `render_main_texture()`:

1. sets the mix's `render_texture` as render target (in the mix's `render_space`,
   RGBA16F / `GS_CS_709_EXTENDED` on an HDR canvas) and draws the main view, or
   copies another mix's texture when it can reuse it (`draw_mix_texture`);
2. sets `texture_rendered = true`;
3. calls every `obs_add_main_rendered_callback` callback **with that mix's
   texture still bound as render target**.

So the callback runs **once per video mix, not once per frame**. The hook acts only when

* `gs_get_render_target() == obs_get_main_texture()` (the main canvas mix);
  secondary mixes - including ones that reused the main texture - are counted as
  `other_mix_skipped`;
* `obs_get_video_frame_time()` differs from the last processed frame; a repeat is
  counted as `duplicate_callbacks_skipped`.

Mixes are rendered in creation order with the main mix first, so a rescaled
stream/record mix that reuses the main texture receives the processed frame.

Who sees the processed frame: streaming, recording, the Program monitor (Studio
Mode) and the normal-mode preview all draw the main texture. Who does **not** run
the hook: previews, projectors, multiview (they draw the texture, they don't
render a mix). Studio Mode's *Preview* pane renders the preview scene directly and is
not denoised - correct, it is not the program.

Not covered by design: outputs on a separate view/canvas that cannot reuse the
main texture (e.g. a virtual camera set to a single scene, additional canvases) -
they render their own picture and are not processed.

## P0 processing

`gs_copy_texture(scratch, main)` then a forced shader pass (`Identity` technique,
`image.Load`, exact texel) drawn back into the main texture with blending off and
framebuffer sRGB off. Same size and format as the main texture; native space kept;
no decode, encode or clamp. A size/format change recreates the scratch texture and
counts a history reset. Missing resources = untouched pass-through, warned once.

## Controls

Tools > *HDR Program Denoise...* opens the properties of a private controller
source (`hdr_toolkit_program_denoise_v1`, hidden from the Add Source list). Settings
persist in the plugin config folder, `program-denoise.json` (development schema 1,
no migrations - brief 2.7). Default: Off.

## Counters (brief 4)

| Counter | Meaning |
|---|---|
| `callbacks` | every main-rendered callback, all mixes |
| `other_mix_skipped` | callback while a non-main mix texture was bound |
| `unique_program_frames` | distinct main-mix frames (brief: program_frames_seen / unique_program_frames) |
| `duplicate_callbacks_skipped` | main texture bound again for an already processed frame time |
| `denoise_dispatches` | passes drawn into the main texture |
| `history_updates`, `history_resets` | temporal history (P1); resets also counted on resource recreation |
| `failures` | frames passed through because resources were missing |
| `obs_total_frames`, `obs_lagged_frames` | OBS's own counters since the last reset, for cross-checking |

Logged on request, or every 10 s when enabled (graphics thread, one line). P0
acceptance: `denoise_dispatches == unique_program_frames` (Identity), and
`unique_program_frames` tracks `obs_total_frames`.

## Measurement

Use the obs-color-monitor `hdr-hlg` scopes with target **Program** (it draws
`obs_render_main_texture()`, i.e. the processed frame). Target *Main view* renders
the program scene itself and would bypass the hook.

## Later packages (from the brief, not started)

* P1 HQDN3D temporal: ping-pong history textures, pixel shader; CPU float mirror.
* P2 HQDN3D spatial: a faithful recursive pass needs a D3D11 compute shader (OBS's
  graphics API has no compute; `gs_get_device_obj()` / `gs_texture_get_obj()` give
  the native objects) - Windows-only; the GPU-friendly approximation can stay in
  pixel shaders. `docs/HQDN3D_DESIGN.md` must exist before any HQDN3D code (brief 28).
* P3 NLMeans: rough budget, not a measurement: Light (3x3 patch, 7x7 search) at
  3840x2160x30 is about 49 x 9 x 8.3 M x 30 = 110 G samples/s, around the 1650
  Super's texture rate. Expect to need half-resolution or luma-only weights.
