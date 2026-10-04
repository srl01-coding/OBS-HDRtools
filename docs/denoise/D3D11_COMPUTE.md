# Native D3D11 compute: P2 identity spike

The decision (`P2_DECISION_RESPONSE.md`, sections 4, 5, 13 and 21) approves Windows-only
native D3D11 compute. Recursive spatial A and P3 NLMeans will both use it. This spike
proves only the plumbing, with a compute shader that copies every texel unchanged. No
denoise algorithm runs on compute until the spike passes on the user's machine.

Code: `src/denoise/d3d11-compute.cpp`. On other platforms it is a stub and the
algorithm entry is hidden. To use it, choose Algorithm *Compute identity (D3D11 spike)*.

## Facts it relies on (OBS 32.2.2 source)

- `gs_get_device_obj()` returns OBS's `ID3D11Device*`, and `gs_texture_get_obj()`
  returns the `ID3D11Texture2D*` behind a `gs_texture_t`. Neither call adds a reference.
- OBS creates render targets with `BIND_SHADER_RESOURCE | BIND_RENDER_TARGET` and never
  with `BIND_UNORDERED_ACCESS`. A compute shader can therefore read the program texture
  but cannot write it.
- On an HDR canvas the program texture is `DXGI_FORMAT_R16G16B16A16_FLOAT` (typed). The
  spike refuses any other format and falls back.
- `gs_set_render_target()` only records the target, and the D3D11 output merger is
  rebound at the next draw. Unbinding the program texture before a compute read
  therefore has to happen on the D3D11 context, not through `gs_*`.
- OBS draws nothing with compute, so the compute stage of the immediate context is
  normally empty. The spike still saves and restores it.

## Resources

All resources are plugin-owned and persistent. They are created on first use and again
only when the size, the format or the device changes, never per frame:
- `in`: RGBA16F with an SRV, holding a copy of the program (two-copy variant);
- `out`: RGBA16F with a UAV, holding the compute output;
- an SRV on OBS's program texture (one-copy and deferred variants). It holds a
  reference, keyed by the texture pointer, and is recreated when OBS replaces the
  texture.

Together that is about 127 MiB at 4K. Everything is released when another algorithm is
selected and on unload.

**Device rebuild.** The `ID3D11Device*` is compared every frame. If OBS has rebuilt its
device, every object is released and recreated. If `GetDeviceRemovedReason() != S_OK`,
the frame passes through.

**Shader.** `cs_5_0` is compiled once at runtime with `D3DCompile` from
`d3dcompiler_47.dll`, the same DLL OBS's renderer uses, loaded with `LoadLibrary`.
Nothing is linked at build time.

## Variants (Development > Compute spike variant)

| Variant | Per frame | Isolation |
|---|---|---|
| Two copies (default) | `CopyResource(in, program)`, then dispatch (in -> out), then `CopyResource(program, out)` | CS shader, SRV slot 0 and UAV slot 0 saved and restored |
| One copy | The output merger's RTVs and DSV are saved and unbound, then dispatch (program SRV -> out), then OM and CS are restored, then `CopyResource(program, out)` | explicit save and restore of OM and CS |
| Deferred | Dispatch and copy-back are recorded on a deferred context, then `ExecuteCommandList(list, TRUE)` | D3D11 restores the immediate context; the command list starts from default state, so the program is not bound as a render target inside it |

- `CopyResource` into the program texture while it is bound as the render target is
  legal in D3D11.
- The deferred variant tests the decision's hypothesis: an isolation that D3D11 itself
  guarantees, against its overhead. The log says whether the driver supports command
  lists natively or emulates them.

Expected traffic at 4K RGBA16F (each frame copy is 63 MiB read + 63 MiB written):
- two copies: about 3 x 127 MiB, roughly 2 ms on a GTX 1650 Super at ~190 GB/s;
- one copy and deferred: about 2 x 127 MiB, roughly 1.4 ms.

This is an estimate. The decision's gate (≤ 1.0 ms p95 preferred, ≤ 1.5 ms
acceptable) can probably be met only by the one-copy variants at 4K on that card.
Production compute passes will not copy for every pass anyway. For example, spatial A
would read the program SRV, run H and V in private textures, and copy back once.

## Failure behaviour

Every check runs before the program texture is touched:
- renderer is not D3D11;
- device removed;
- compile failure;
- wrong format;
- resource creation failure;
- `FinishCommandList` failure.

If any check fails, the frame passes through unchanged, `failures` counts it, and the
log gives the reason once per distinct reason. The periodic counter line then shows
`MISMATCH dispatches != unique frames`.

## Verification views

- *Exact change* (debug view): the input is copied before the round trip and compared
  with the result. Black means every component is equal; magenta means any component
  changed. This works for HQDN3D as well, for example to prove the identity of the
  spatial passes with *Run spatial passes at strength 0*.
- *What was removed*: |result - input| x gain.

The compare views add copies, so timing is read with the debug view *Normal*.

Gates: `docs/VALIDATION.md`, "Program denoise P2". The decision's compute safety list
(section 21) applies before any compute path becomes production.
