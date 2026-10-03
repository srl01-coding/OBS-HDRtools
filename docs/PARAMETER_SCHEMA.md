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
