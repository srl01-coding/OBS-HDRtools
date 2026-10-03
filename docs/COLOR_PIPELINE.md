# Colour pipeline and render contract

Applies to every filter in this package. Brief sections 3 and 5.1.

## Five separate meanings of "HDR" (brief 3.1)

1. Camera transfer/profile (HLG, Cine2, S-Log...). **Not visible to these filters.**
2. Capture representation (bit depth, matrix, range). Decoded by OBS before us.
3. OBS graphics colour space + working RGB. **This is what the filters see.**
4. Intermediate storage (`GS_RGBA` / `GS_RGBA16F`) and shader arithmetic (float32).
5. Output encoding (HLG/PQ code values, 64-940 etc.). Done by OBS after us.

No filter decodes HLG/PQ, multiplies by 4, or treats code 940 as a maximum.

## Working spaces

| OBS space | Working values | 1.0 means |
|---|---|---|
| `GS_CS_SRGB` | 8-bit sRGB storage; OBS's sRGB-aware sampling gives linear RGB | SDR white W (reference, not absolute) |
| `GS_CS_SRGB_16F` | float linear SDR | W (reference) |
| `GS_CS_709_EXTENDED` | float linear Rec.709 primaries, >1 and <0 allowed | W nits |
| `GS_CS_709_SCRGB` | float linear Rec.709 primaries | 80 nits |

`W = obs_get_video_sdr_white_level()`, read live; never hard-coded.
`gs_get_format_from_space()` gives `GS_RGBA16F` for every non-SRGB space
(verified in `graphics.h`, OBS 32.2.2).

Grading maths (from P2) runs on straight linear RGB in **nominal nits**
(`C_nits = C * nits_per_unit(space)`), converted back once at output.

## Space negotiation: "preserve native" policy (brief 3.3)

```text
input_space    = obs_source_get_color_space(target, {SRGB, SRGB_16F, 709_EXTENDED, 709_SCRGB})
staging_space  = input_space
output_space   = input_space            (video_get_color_space ignores preferences)
consumer_space = gs_get_color_space()   at render entry; logged, not overridden
```

Why this is sufficient (verified by reading OBS 32.2.2 `libobs/obs-source.c`):
`source_render()` asks every source/filter for its space with the consumer's
space as the only preference. If the answer differs, libobs renders the
filter into a `color_space_texrender` in the reported space and converts with
the matching `default.effect` technique (`Draw`, `DrawMultiply`, `DrawTonemap`,
`DrawMultiplyTonemap`). So an SDR preview gets a legitimate HDR->SDR tone map
at the boundary, while the HDR chain itself never takes an HDR->SDR->HDR
detour. This must still be proven in the user's build (VALIDATION gates N3-N5).

## Render path (brief 3.4)

1. Validate target/parent; zero size -> `obs_source_skip_video_filter`.
2. `capture_context()` -> input space, consumer space, staging format, size, W, peak.
   Logged once on change (not per frame).
3. `obs_source_process_filter_begin_with_color_space(format, input_space, OBS_NO_DIRECT_RENDERING)`.
   If it returns false, return without rendering again.
4. Bind cached effect parameters.
5. `obs_source_process_filter_tech_end(..., tech)` with blend `ONE / INVSRCALPHA`
   (output is premultiplied), blend state pushed/popped.

`OBS_SOURCE_SRGB` is declared, so libobs enables linear sRGB sampling and
sRGB framebuffer writes for 8-bit SDR inputs; float textures are unaffected.
Filters never toggle framebuffer sRGB themselves.

## Alpha (brief 3.6)

The filter texrender produced by `process_filter_begin` is **premultiplied**
(target drawn with `SRCALPHA, INVSRCALPHA` colour blend onto cleared transparent
black, alpha blend `ONE, INVSRCALPHA`). Per pixel:

```text
if a <= 1e-6: return (rgb * linear_gain, a)        // no division; exact for gains
straight = rgb / a  ->  grade in nits  ->  return (graded * a, a)
```

## Forbidden in any HDR path

`saturate(rgb)`, `max(rgb,0)`, `GS_RGBA` intermediates for non-SRGB spaces,
manual sRGB decode of float textures, a 940/1.0 ceiling, decoding HLG again,
advertising a space whose numerical meaning is not implemented.

## Known OBS behaviour relevant to testing (measured 27 Sep 2026, scope fork)

* OBS's input decoder (`format_conversion.effect`, `YUV_to_RGB`) clamps
  limited-range Y'CbCr to its legal range **before** any filter: a 10-bit HLG
  file's code 1019 arrives identical to 940. Super-whites/footroom from video
  sources cannot be tested through filters; use `hdr_toolkit_pattern_dev`.
  Media Source and Video Capture Device both expose a Color Range override
  (Auto/Partial/Full) that changes this clamp.
* Color Correction and Color Grade skip `GS_CS_709_EXTENDED` entirely.
* obs-shaderfilter 2.6.0 on an HDR Media Source: a x4.17 linear gain collapsed
  every bar >= ~50% HLG to one level at SDR white (clipping, not tone mapping).
  Mechanism not identified.
