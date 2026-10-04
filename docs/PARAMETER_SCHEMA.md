# Parameter schema

Persisted numeric parameters only (brief 10.4). Values are validated on load
and on every update; unknown enum values fall back to the default. Any change
to processing order or meaning needs a new `schema_version` and a migration.

## hdr_toolkit_transform_v1 (schema_version 1, P1)

| Key | Type | Default | Meaning |
|---|---|---|---|
| `schema_version` | int | 1 | |
| `warp_model` | int | 0 | 0 = Bilinear (StreamFX-style inverse-bilinear), 1 = Projective (homography) |
| `corner_tl_x`, `corner_tl_y` | double | 0, 0 | **Canonical** corner A, normalised to the output rectangle (x right, y down) |
| `corner_tr_x`, `corner_tr_y` | double | 1, 0 | corner B |
| `corner_br_x`, `corner_br_y` | double | 1, 1 | corner C |
| `corner_bl_x`, `corner_bl_y` | double | 0, 1 | corner D |
| `ui_tl_x` ... `ui_bl_y` | double | canonical x 100 | UI mirror in percent. Written through to the canonical keys by the property callbacks; regenerated from the canonical keys on every update. Scripts should set the canonical keys. |
| `sampling_mode` | int | 0 | 0 = bilinear, 1 = point (diagnostic) |
| `force_render_identity` | bool | false | Diagnostic: run the shader even for the identity quad |

Validity (brief 4.6): coordinates finite and within -4..5 (-400%..500%); no
coincident corners (1e-4); convex in either winding (mirroring allowed);
area >= 1e-4 x longest edge^2; bilinear Jacobian sign consistent at the
four source corners; projective additionally: corner residual <= 1e-9,
homogeneous denominator one sign over the source square, H^-1 H = I to 1e-9.
An invalid quad is persisted as entered (the user's numbers are never
rewritten) but rendering uses the last valid quad, or identity after reload.

Converting StreamFX centred percent: `canonical = (streamfx_percent + 100) / 200`.

Not yet in the schema (later packages, additive with defaults matching v1
behaviour): `transform_mode` (Corner Pin / Perspective 3D / Orthographic 3D),
3D parameters, `edge_aa`, pixel / legacy-centred unit display.

## hdr_toolkit_color_v1 (schema_version 1, P2 global stages)

| Key | Type | Default | UI range | Meaning |
|---|---|---|---|---|
| `schema_version` | int | 1 | | |
| `global_exposure_ev` | double | 0 | -6..6 | Linear gain 2^EV |
| `contrast_factor` | double | 1 | 0.5..2 | Power k on luminance around the pivot: `Y' = pivot (Y/pivot)^k`, applied as an RGB gain (hue/ratio preserving, sign-safe) |
| `pivot_nits` | double | 18 | 0.1..1000 | Contrast pivot in nominal nits |
| `gray_reference_nits` | double | 18 | 1..100 | Reference grey (brief 5.2); scales the log domain, and will be the tonal-zone reference in P3 |
| `global_saturation` | double | 1 | 0..2 | Linear-Y-preserving saturation |
| `wb_mired_shift` | double | 0 | -90..90 | Temperature: target white on the CIE daylight locus at (1e6/6504 + shift) mired, offset from D65 in CIE 1960 uv; Bradford adaptation D65 -> target (+ = warmer) |
| `wb_tint` | double | 0 | -100..100 | Tint: offset along the locus normal, 0.01 uv per 100 (+ = magenta, - = green) |
| `global_wheel_x`, `global_wheel_y` | double | 0, 0 | -1..1 | Colour-balance wheel; hue = atan2(y, x) with 0 deg = red, radius clamped to 1, sensitivity k = 0.5. `C += k r d(hue) Y`, d luminance-orthogonal |
| `grade_mix` | double | 1 | 0..1 | Linear-light mix of the original and graded colour (brief 5.4); soft clips (P3) will run after it |
| `force_render_identity` | bool | false | | Diagnostic: run the shader even when the grade is neutral |

Processing order (brief 5.3; P2 implements the stages marked *):
unpremultiply -> nominal nits -> C0 -> WB* -> exposure* -> [zone masks, zone exposure: P3] ->
contrast* -> global* [+ zone: P3] wheel -> global* [+ zone: P3] saturation -> Grade Mix* ->
[low soft clip, high soft clip, gamut containment: P3/P5] -> working units -> premultiply.

The P3 stages slot into this order with neutral defaults (zones disabled, soft
clips off), so a scene saved with schema 1 renders identically after P3
without a migration. Any change to the order, WB mapping, wheel sensitivity or
contrast law needs schema 2 and a migration (brief 10.4).

Out-of-range values (e.g. a hand-edited scene file) are clamped to the UI range
and logged; non-finite values revert to the default. If the white-balance
matrix cannot be built, the last valid grade is kept (neutral after reload).

