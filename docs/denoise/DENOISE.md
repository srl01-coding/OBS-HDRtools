# HDR Program Denoise - design notes

Governing spec: `docs/denoise/BRIEF_v1.2.md` (the file's own heading says v1.1; the
user supplied it as v1.2). Status: **P0 implemented (Identity: user saw no visible change; formal gates NOT RUN). P1 HQDN3D-style temporal implemented, gates NOT RUN.**

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

## P1: HQDN3D-style temporal

Specification: `docs/HQDN3D_DESIGN.md` (written first; independent of FFmpeg/MPlayer
code). Per unique frame, all on the GPU:

```text
copy main -> cur
MetricBlocks(cur, hist) -> l1 (w/16 x h/16, R32F)      skipped on reset frames
MetricReduce(l1)       -> l2 (w/256 x h/256, R32F)
MetricFinal(l2, prev)  -> metric (1x1 RGBA32F: m, static-noise floor, valid)
Temporal(cur, hist, metric) -> hist_next               the history update
Output(cur, hist_next)  -> main texture                Mix and debug views
```

Each technique sets its parameters again, because `gs_technique_end()` resets them.
Every pass binds its own render target; the last one (Output) leaves the main
texture bound, as OBS had it. VRAM: 3 x program-size textures (190 MiB at 4K
RGBA16F), released when the mode is left.

Telemetry, only while counter logging is on: GPU timer queries and a 1x1 staged
copy of the metric, read three frames later. That gives `gpu ms avg/p95/max`,
`metric avg/max` and `cut_resets` (cuts detected by the GPU). The processing path
itself never reads back.

Not for SDR canvases: on an 8-bit SDR canvas the main texture holds sRGB-encoded
values, so the luma/log maths runs on encoded values and the history is 8-bit. It
works but is not the designed case.

Rough cost (estimate, not a measurement): about 7 full-frame RGBA16F reads/writes
per frame, about 13 GB/s at 4K30 against the 1650 Super's ~190 GB/s, so a few ms.

## P2: spatial B + D3D11 compute spike

Decision: `P2_DECISION_RESPONSE.md` (Option D). Design: `docs/HQDN3D_DESIGN.md`
section 8. Quality objective: `QUALITY_OBJECTIVE.md`.

* **B (built):** a symmetric edge-aware kernel, separable as SpatialH then SpatialV in
  pixel shaders. It runs before the temporal pass:

  ```text
  copy main -> cur
  SpatialH(cur) -> sp_tmp ; SpatialV(sp_tmp) -> sp_out
  metric and Temporal read sp_out
  Output compares against cur
  ```

  The spatial passes are skipped while both spatial strengths are 0, unless
  Development > *Run spatial passes at strength 0* is on (that is the identity proof).
  VRAM is +2 program-size textures (127 MiB at 4K), released when spatial is off.
* **A (CPU reference only):** a bidirectional separable recursion. The compute shader
  for it waits until the compute spike passes on the user's machine.
* **Compute spike (built):** `D3D11_COMPUTE.md`. It is a native compute identity round
  trip with three isolation variants, a transparent fallback, and an *Exact change*
  debug view.
* **Radius:** 6, 8 or 12 (Development); 8 is provisional. On white noise all three are
  equivalent at matched noise reduction (`HQDN3D_DESIGN.md` 8.6). Real camera noise is
  spatially correlated, so footage decides.

## Roadmap (revised 4 Oct 2026)

Governing documents, in order:
- `BRIEF_v1.2.md`;
- `P2_DECISION_RESPONSE.md`;
- `ROADMAP_CORRECTION_MOTION_AWARE.md`;
- `P2_5_AMENDMENT.md`, which supersedes the P2.5 section of the roadmap correction.

```text
P0    final-program HDR-safe plumbing                         done
P1    HQDN3D temporal                                         built; OBS gates NOT RUN
P2    HQDN3D spatial B + D3D11 compute spike (+ A if safe)   B and spike built; A CPU only
P2.5  premium temporal comparison
      A  Temporal NLMeans: one engine, candidates (dx, dy, dt), dt in {0, -1}, causal
      B  NVOFA motion-compensated temporal: capability probe, flow, warp, confidence
      C  comparison gate: HQDN3D vs Temporal NLMeans vs NVOFA (synthetic, real footage,
         GPU cost, same-bitrate HEVC / YouTube)
P3    optimise the winning premium architecture; spatial NLMeans only where it adds value
P4    tune combined production modes; codec/YouTube A/B
```

What can be done before P1/P2 run in OBS (CPU only, no GPU here):
- the shared NLMeans design (`NLMEANS_DESIGN.md`);
- CPU references for spatial and temporal NLMeans;
- the synthetic motion/noise comparison of all temporal candidates, with NVOFA
  represented by ideal (oracle) flow and a 4x4-grid oracle, which bound what hardware
  flow can achieve;
- real-footage experiments on the sample clip (`NOISE_MODEL.md`);
- a same-bitrate x265 test against a clean reference.

Everything that touches the GPU waits for the compute spike (CS gates) and P1 on real
footage. That includes the NLMeans compute kernels and the NVOFA integration. The NVOFA
capability probe is the exception, because it only queries the driver.

`MOTION_AWARE_TEMPORAL.md` holds the design notes and results for P2.5.
