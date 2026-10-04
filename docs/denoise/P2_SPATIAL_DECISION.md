# Decision request: how to implement HQDN3D spatial (denoise P2)

**From:** the Claude Code instance implementing *OBS HDR Denoise brief v1.2*.
**To:** the instructing AI that wrote the brief.
**Date:** 4 Oct 2026.
**Repository:** `srl01-coding/OBS-HDRtools` (`src/denoise/`, `docs/HQDN3D_DESIGN.md`, `docs/denoise/`).

The user wants you to make this decision and do the design thinking. I have gathered the facts below. Throughout, **established** means I verified it in source or tests, **estimate** means arithmetic that hasn't been measured, and **uncertain** means exactly that.

---

## 1. The question

Brief sections 12 and 26 (P2) ask for two spatial HQDN3D prototypes:
- **(A)** a faithful recursive GPU implementation;
- **(B)** a GPU-friendly approximation, labelled "HQDN3D-style".

They are to be compared on quality and on 4K30 performance.

Doing both properly is expensive, and choosing A carries a structural cost the brief doesn't mention: **OBS's graphics API has no compute shaders**. So the real decisions are:

1. **Should the plugin take on a Windows-only native Direct3D 11 compute path**, outside libobs's graphics abstraction? This affects P2 and, probably more importantly, P3 (NLMeans).
2. **What does "faithful" have to mean for the spatial filter?** In particular, is the classic recursion's one-directional causality worth preserving?
3. **Which option or options should actually be built and benchmarked**, and against what acceptance numbers?

The output I need is listed in section 8.

---

## 2. Where the project stands (established)

| Item | State |
|---|---|
| Runtime | Windows 10, OBS 32.2.2, Rec.2100 HLG canvas, 3840×2160 at 30 fps, GTX 1650 Super (4 GB). The plugin is built against the OBS 31.1.1 SDK in GitHub Actions. CI also builds macOS and Ubuntu (OpenGL), but the user runs only Windows. |
| P0 hook | Processes the program frame once per unique frame. OBS's main-rendered callback runs once per video mix, so it is deduplicated by the main canvas texture and the frame time. The frame is RGBA16F in `GS_CS_709_EXTENDED`; processing is in place on the main texture. **Formal gates NOT RUN**; the user saw no visible change from the Identity pass. |
| P1 temporal | HQDN3D-style temporal, implemented from `docs/HQDN3D_DESIGN.md`. Five pixel-shader passes per frame. **Not yet run in OBS.** CPU float mirror tests pass. All shaders compile with DXC (pre-flight only). |
| Licensing | Independent implementation; no FFmpeg/MPlayer code. Spatial must follow the same rule (brief 28). |

### P1 design elements the spatial filter should reuse (for consistency)

- **Signal:** linear RGB in nits. Luma Y = 0.2126 R + 0.7152 G + 0.0722 B. Chroma is the luma-orthogonal residual c = RGB − Y·(1,1,1), so reconstruction is exact and neutral stays neutral.
- **Comparison domain:** F(y) = sign(y)·log2(1 + |y|/K), with K = 0.1 nit by default.
  - Luma difference: dL = |F(Ya) − F(Yb)|.
  - Chroma difference: dC = |ca − cb| / (mean|Y| + K) / ln 2.
- **Response:** w = β·W(d/T), with W(x) = (1 − x²)² on [0,1), T = 0.01 × strength (log2 units), and β = 0.9 as a cap so nothing freezes. Strength 0 is an exact identity.

---

## 3. Platform facts that drive the decision (established in OBS 32.2.2 source)

1. **libobs `gs_*` has no compute stage.** Effects are vertex + pixel shaders only. There are no unordered-access views, structured buffers or groupshared memory.
2. **Native objects are reachable.** `gs_get_device_obj()` returns the `ID3D11Device*` on Windows, and `gs_texture_get_obj(tex)` returns the `ID3D11Texture2D*`. A compute path is therefore possible through raw D3D11 calls on the device's immediate context.
3. **OBS never creates UAV-capable textures** (no `D3D11_BIND_UNORDERED_ACCESS` anywhere in `libobs-d3d11`). A compute path needs its own D3D11 textures, plus `CopyResource` between those and OBS's textures (same RGBA16F format).
4. **Shared device context, cached state.** OBS's D3D11 renderer keeps its own record of what is bound (render targets, shader resource views, shaders). Compute dispatches on the same immediate context must not leave graphics-stage bindings changed behind OBS's back.
   - D3D11 automatically unbinds a resource from shader-resource slots when it is bound as an output elsewhere.
   - Using only private textures for UAVs and restoring compute-stage state after each dispatch should avoid this. **Uncertain until tested on the user's machine.**
5. **Compute shaders need their own compile step.** Either runtime `D3DCompile` (d3dcompiler_47 ships with OBS on Windows), or precompiled bytecode from `fxc` on the Windows CI runner. The `.effect` system can't express compute shaders.
6. **Platform split.** On the OpenGL / Metal builds the compute path would be absent, so a non-compute fallback (or "spatial unavailable") is needed there. That's acceptable for this user, but it is a code-maintenance cost.
7. **Effect-system pitfalls learned so far** (they apply to any pixel-shader option):
   - `gs_technique_end()` resets all parameters.
   - No exponent literals.
   - Use `Load` rather than `Sample` inside dynamic loops.
   - Effects compile only at plugin load.

### GPU budget (estimate, from published GTX 1650 Super specifications)

| Figure | Value |
|---|---|
| Memory bandwidth | about 192 GB/s |
| Texture rate | about 120–140 GTexel/s |
| FP32 compute | about 4.4 TFLOPS |
| One full 4K RGBA16F frame | about 66 MB |
| Rule of thumb | each extra full-resolution read+write pass at 4K costs roughly **1 ms** on this card |
| P1 temporal (estimate) | about 2–3 ms |
| Brief's total denoise budget | 8–10 ms at p95 |

---

## 4. The spatial problem

As the brief describes it (from published behaviour), classic HQDN3D spatial filtering is **recursive**: each output sample is a nonlinear low-pass of the input against **already-filtered neighbours**, effectively the previous pixel in the row and the corresponding pixel in the previous row. Because the response is nonlinear (it depends on the difference between input and filtered neighbour), the recurrence **cannot be turned into a parallel prefix sum**. It is inherently sequential along its direction.

Two properties of that recursion matter:
- **Long reach:** in flat areas the smoothing propagates arbitrarily far (an IIR filter), which is part of its cheap-but-strong character.
- **Directional bias:** causal left→right and top→bottom filtering is asymmetric. Smoothing flows from one side only, so edges are treated differently on their leading and trailing sides. This is a by-product of a 1990s CPU design rather than a desirable feature.

---

## 5. Options

### Option A: native D3D11 compute, sequential scan recursion (the brief's "A")

- **Structure:**
  - A horizontal pass: one GPU thread per row walks the 3840 pixels in order.
  - A vertical pass: one thread per column walks the 2160 rows.
  - Tiles are staged in groupshared memory for coalesced access.
  - Optional **forward and backward** passes in each direction remove the directional bias, at twice the cost.
- **Fidelity:** a true causal recursion, separable (H then V). The fused classic form, where each pixel depends on both its left and upper filtered neighbours at once, is a wavefront dependency needing about 6,000 globally synchronised steps per frame. **I consider the fused form impractical on this GPU and would implement the separable form.** That is already a documented deviation from the classic form.
- **Cost (estimate, uncertain):**
  - Only 2,160 or 3,840 threads exist per pass, so the 1650 Super (about 20 SMs) is barely occupied and memory latency per step is poorly hidden.
  - My range is **about 0.3–1.5 ms per directional pass**, so **about 1–3 ms** one-directional and **2–6 ms** bidirectional.
  - The real figure depends heavily on how well the row data are prefetched into shared memory, and only a spike will tell.
- **Risks:**
  - Interaction with OBS's cached D3D11 state (section 3, item 4).
  - Windows-only.
  - Runtime shader compilation and device-loss handling to build.
  - None of the existing `.effect` tooling or the DXC pre-flight applies as-is (DXC can still check compute shaders).
- **Side benefit:** it builds the compute infrastructure that **P3 NLMeans very probably needs** (section 6).

### Option B: pixel-shader nonlinear separable approximation ("HQDN3D-style")

- **Structure:**
  - Horizontal then vertical passes, each a symmetric neighbourhood of radius R (for example 6–8).
  - Each tap's weight is an exponential distance falloff a^|k| (mimicking the recursion's decay) times the same response W(d/T) used in P1, with d measured against the centre pixel in the comparison domain.
  - The normalised weighted mean is taken of the original linear values, with luma and chroma separate.
- **Fidelity:**
  - Not recursive and not causal: no directional bias, and a reach limited to R per pass.
  - It is an edge-preserving cross between a bilateral and an exponential filter, so it must be labelled "HQDN3D-style".
  - Its behaviour can be tuned to look similar, but the long-range IIR smoothing of flat areas is shorter.
- **Cost (estimate):**
  - At R = 8 there are 17 taps × 2 passes ≈ 34 loads per pixel, about 0.3 G loads per frame or 8.5 G/s at 4K30.
  - **About 1–2 ms.**
- **Risks:** low. It reuses all the existing infrastructure, works on every platform, and the CPU mirror and DXC pre-flight apply directly.
- **Downside:** it does nothing to prepare for NLMeans.

### Option C: bounded-window ("truncated") recursion in pixel shaders

- **Idea:** with the history weight capped at β_s < 1, the recursion's memory decays geometrically. A filtered pixel depends on the input k steps back with weight at most β_s^k, so evaluating the recursion over only the last L inputs is **exact to within ε ≤ β_s^L** of the full recursion.
  - With β_s = 0.9 and L = 64, ε ≈ 0.001 of the local difference.
  - With β_s = 0.95, L ≈ 135 for the same ε.
- **Structure:** each pixel re-runs the recursion over its own L-pixel window in a pixel shader. That is fully parallel, cross-platform, and keeps the recursion's character, including its long reach up to L and, optionally, its causality. Running it forward and backward and averaging removes the bias.
- **Fidelity:** recursive behaviour with a documented, bounded error and a self-weight cap. That cap is the same design choice as P1's β.
- **Cost (estimate):** L loads per pixel per pass, so at L = 64 with two passes, 128 loads per pixel is about 1 G loads per frame, about 32 G/s. That is around 25% of the card's texture rate, **roughly 3–5 ms**. Halving L halves the cost, at the price of a larger ε or a lower β_s.
- **Risks:** low to medium. Shader loop length and compile time need care (the DXC pre-flight helps), and the cost scales with the window.

### Option D: hybrid

- Build B, or C, as the always-available, cross-platform implementation.
- Build the D3D11 compute infrastructure as a separate, contained spike (an identity copy plus the scan recursion of A), which serves as a P3 foundation.
- Pick the production spatial path by measurement.

### Comparison

| | A: compute scan | B: separable approximation | C: windowed recursion | D: hybrid |
|---|---|---|---|---|
| Recursive | yes (separable) | no | yes, within ε | per choice |
| Directional bias | yes, unless bidirectional | none | optional | per choice |
| Cost on 1650S (estimate) | 1–3 ms one-way, 2–6 ms both ways | 1–2 ms | 3–5 ms at L = 64 | B/C, plus the spike |
| Platforms | Windows only | all | all | all, plus Windows extra |
| Fits existing tooling | no (new compute path) | yes | yes | partly |
| Prepares P3 NLMeans | **yes** | no | no | **yes** |
| Main risk | OBS D3D11 state interplay; occupancy | quality difference from classic | cost scales with L | scope |

---

## 6. The P3 connection (why this is not just a P2 choice)

**The NLMeans cost problem (estimate):**
- The brief's "Light" preset (3×3 patch, 7×7 search) at 4K30 needs about 49 × 9 × 8.3 M × 30 ≈ 110 G samples/s computed naively. That is at or beyond the 1650 Super's texture rate.
- The standard efficient GPU formulation computes, per search offset, a squared-difference image and then box-filters it to patch sums, ideally in **groupshared memory** so each loaded texel is reused across neighbouring pixels and offsets.
- The brief also requires **bounded memory**: no full-resolution texture per offset.

**Without compute** (pixel shaders only), NLMeans becomes multi-pass:
- For each offset: a difference pass, two box-filter passes, and an accumulation pass into running weight/sum textures.
- That means several full-resolution passes per offset, about 49 × 4 ≈ 200 passes per frame at 4K for "Light". At roughly 1 ms per full-resolution pass (section 3), that is far over budget.
- Pixel-shader NLMeans would therefore have to run at reduced resolution or with a reduced search.

**With compute**, the tiled shared-memory version is the realistic route to full-resolution Light, and possibly Balanced, on this card. **I'm uncertain whether even that fits the 8–10 ms budget** on a 1650 Super.

So **the infrastructure decision (compute: yes or no) is really a P3 decision**, and P2 is the cheapest place to prove it safe inside OBS.

---

## 7. What has to be measured, whatever is chosen

1. **GPU ms** (average, p95, max) at 4K30 on the 1650 Super, for P1 temporal alone and with each spatial candidate. P1 already logs GPU timer data when counter logging is on.
2. **Quality:**
   - *Synthetic:* noise standard deviation removed on flat patches against edge blur (step-edge width or MTF) at matched noise reduction.
   - *Real footage:* the a6400/a6700 cameras, using the "what was removed" view (it should show grain only, no edges).
3. **For A or D:**
   - Proof that compute dispatch plus `CopyResource` leaves OBS's rendering intact: no corrupted previews, other filters still correct, no D3D11 debug-layer errors if obtainable.
   - Device-reset survival.
4. **Equivalence:** whichever option is chosen, the GPU output must match the CPU reference of the *declared* algorithm, not FFmpeg.

---

## 8. What I need back from you

Please decide, and write it as an addendum to `docs/HQDN3D_DESIGN.md` (section 8, "Spatial"), covering:

1. **Infrastructure:** whether a Windows-only native D3D11 compute path is acceptable in this plugin, given:
   - the P3 implications (section 6);
   - the requirement for a cross-platform fallback;
   - the risk of state interplay with OBS's renderer.
2. **Which spatial algorithm(s) to build in P2** (A, B, C or D, or something better), and **what "faithful" means**:
   - Is causality or directional bias to be preserved or removed?
   - Is separable (H then V) acceptable in place of the fused form?
   - Is a self-weight cap with bounded error acceptable?
3. **The written recurrence or kernel**, at the level of `docs/HQDN3D_DESIGN.md` section 3, using the P1 comparison domain and response (section 2 above) or replacing them with reasons. Include:
   - how spatial luma/chroma strength maps to parameters;
   - the order relative to temporal: spatial before temporal, after it, or on the temporal output. Classic HQDN3D applies spatial and temporal together; state your choice and why.
4. **Acceptance numbers for P2:** for example, spatial at most X ms p95 on the 1650 Super at 4K30; noise reduction of at least Y on a synthetic flat patch with edge widening of at most Z px.
5. **Whether P2 should wait** until P1 temporal has been tuned on real footage (P1 hasn't run in OBS yet), or proceed in parallel.

Constraints you can rely on:
- I will implement exactly what is written.
- The CPU reference comes first, then the float mirror, then the shader.
- I will report measured numbers honestly and mark anything not run as NOT RUN.
- No FFmpeg/MPlayer code will be used.

---

## 9. My own recommendation (for you to accept or overrule)

**Option D.**
- Build **B** as the immediate cross-platform spatial mode, so the user gets a usable light spatial denoise quickly.
- Run a contained **D3D11 compute spike** in P2: identity copy, then the A scan recursion, with timing and OBS-state checks. P3 needs that foundation anyway, and P2's algorithm is cheap enough to debug the infrastructure on.
- Keep **C** in reserve, in case B's character is too far from what the user expects of HQDN3D and the compute path proves fragile.
- Remove the directional bias in any recursive variant (bidirectional), and document it as an intentional difference.

**Confidence:**
- Moderate on the structure.
- Low on the cost estimates; they are arithmetic, not measurement.
- High on the platform facts in section 3.
