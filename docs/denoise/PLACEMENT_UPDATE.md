# OBS HDR Denoise — placement and roadmap update

**Date:** 5 Oct 2026  
**Repository:** `srl01-coding/OBS-HDRtools`  
**Status:** implementation direction update  
**Supersedes:** any roadmap language that treats denoise placement as program-level only or NLMeans as a near-term priority.

---

# 1. New measured facts from the user's machine

The user has now run early denoise builds on the actual production PC:

- Windows 10
- GTX 1650 Super
- 4K-class HDR workflow
- OBS Rec.2100 HLG

Observed:

- **One source with spatial denoise enabled used roughly 20% GPU "3D" utilization on the GTX 1650 Super.**
- **Temporal denoise appeared to have essentially no observable impact on GPU usage.**

These are runtime observations from the user's machine, not synthetic estimates.

Implication:

> Temporal denoise is cheap enough that per-source placement is especially attractive. Spatial denoise is materially more expensive and must remain configurable and benchmarked before enabling it broadly across multiple cameras.

---

# 2. Roadmap change: park NLMeans

**NLMeans is now parked.**

Do not spend further implementation time on:
- spatial NLMeans GPU optimization;
- temporal NLMeans GPU work;
- generalized NLMeans UI;
- NLMeans presets;
- heavy NLMeans benchmarking.

Keep:
- existing design notes;
- CPU/reference work already completed;
- generalized `(dx,dy,dt)` research code if it is already cleanly isolated.

Do not delete useful research.

Revisit NLMeans only if, after HQDN3D + motion-aware temporal work, there is a clearly demonstrated residual spatial-noise problem that simpler spatial filtering cannot solve.

Current priority becomes:

```text
1. Temporal HQDN3D-style denoise
2. Placement architecture
3. Spatial HQDN3D-style / recursive spatial optimization
4. Motion-aware temporal / NVOFA
5. Noise-adaptive thresholding T(Y)
6. Only then reconsider NLMeans if needed
```

---

# 3. Placement becomes a first-class product feature

The denoise core must support **both**:

1. **Source Filter placement**
2. **Program Output placement**

These are not separate algorithms. They are two front-ends around the same denoise core.

The implementation should be structured as:

```text
                   HDR DENOISE CORE
                ─────────────────────
                temporal HQ response
                spatial processing
                T(Y) noise profile
                NVOFA / motion logic
                history management
                debug views
                shared settings model
                         │
              ┌──────────┴──────────┐
              │                     │
       SOURCE FILTER          PROGRAM PROCESSOR
              │                     │
       per-camera input         final composited
                                program frame
```

Do **not** duplicate algorithm code between the two placements.

---

# 4. Recommended use of each placement

## 4.1 Source Filter mode — preferred quality path

Recommended when GPU headroom allows.

Pipeline:

```text
Camera
  ↓
HDR Denoise
  ↓
HDR Color
  ↓
HDR Transform / Corner Pin / scaling
  ↓
scene composition / transitions
```

Advantages:

- denoise before geometric resampling;
- denoise before perspective warp;
- denoise before grading amplifies noise;
- temporal history remains tied to one physical camera;
- NVOFA sees genuine within-camera motion;
- no transition contamination;
- no graphics/text denoise if graphics are added later;
- each camera can have its own noise profile/strength.

This is particularly attractive for temporal denoise because measured GPU cost is currently negligible.

---

## 4.2 Program Output mode — economy/simple path

Pipeline:

```text
all scenes / transitions / graphics
        ↓
final program
        ↓
HDR Denoise
        ↓
encoder/output
```

Advantages:

- one denoise instance;
- lowest total GPU load;
- simplest setup;
- useful fallback on weaker hardware;
- still valuable if source-level spatial cost becomes too high.

Disadvantages:

- temporal history crosses camera switches;
- transition protection required;
- NVOFA has no physically meaningful single motion field during dissolves;
- perspective/scaling have already altered the noise;
- grading may already have amplified noise;
- graphics/text may be filtered.

Program mode remains supported but is no longer the only architecture.

---

# 5. Placement recommendation by algorithm component

## Temporal HQDN3D-style

**Preferred placement: source level.**

Reason:
- measured cost appears negligible;
- persistent camera-specific history;
- no cut/dissolve contamination;
- best signal statistics;
- best future foundation for motion-aware temporal.

Recommended default for multi-camera quality mode:

```text
Camera A -> Temporal Denoise
Camera B -> Temporal Denoise
Camera C -> Temporal Denoise
```

Even if only one camera is on-air, off-air instances may still render because of preview/multiview/nested-scene behavior. Measure actual runtime behavior rather than assuming they are idle.

## Spatial HQDN3D-style

**Placement: configurable.**

Measured cost:
- roughly **20% GPU 3D usage for one source** on the GTX 1650 Super.

Therefore three simultaneously rendered source-level spatial filters could become significant.

Do not extrapolate 20% linearly as an exact prediction, but treat the result as a serious capacity signal.

Recommended options:

### Quality
```text
source-level spatial
```

Best image-domain placement, but most expensive.

### Economy
```text
source-level temporal only
        +
single program-level spatial cleanup
```

This is an especially important hybrid to support.

### Minimum load
```text
program-level temporal + spatial
```

Retain as lowest-cost fallback.

---

# 6. Add explicit placement controls

Do not create one confusing filter with a "placement" drop-down that attempts to move itself automatically through OBS.

Instead expose two installable/useable components backed by the same core:

```text
HDR Denoise            (source filter)
HDR Program Denoise    (program processor)
```

or equivalent naming.

Recommended UI/documentation language:

### HDR Denoise
> Add to an individual camera/source. Recommended for best temporal denoise quality.

### HDR Program Denoise
> Processes the final composited program once. Recommended when minimizing GPU load.

The user chooses where to deploy them.

Do not attempt to automatically instantiate per-camera filters.

---

# 7. Shared settings and presets

The core parameter model should be shared between placements.

At minimum:

```text
Temporal Luma
Temporal Chroma

Spatial Luma
Spatial Chroma

Noise Profile / T(Y)

Scene Cut Reset
Transition Protection

Mix

Debug View
```

Placement-specific UI may hide irrelevant controls.

For example:

## Source Filter
- Scene Cut Reset may be unnecessary or disabled by default.
- Transition Protection may be unnecessary because the source itself is not transitioning between unrelated cameras.
- History timeout is important.

## Program Processor
- Scene Cut Reset is required.
- Transition Protection is required.
- graphics/text protection remains relevant.

---

# 8. Source-filter history timeout

Per-source temporal history must handle cameras that stop rendering.

OBS may not continuously render an off-air camera unless multiview, preview, nested scenes, or other consumers cause it to render.

Each source instance must track elapsed frame time / source timestamp.

If the gap exceeds a continuity threshold:

```text
reset history
output current frame
history = current frame
```

Do not compare a newly rendered camera frame against history from seconds earlier.

Suggested rule:

```text
if delta_time > ~2–3 expected frame intervals:
    reset history
```

Tune from actual OBS behavior.

This should be internal by default, not a normal user control.

---

# 9. NVOFA placement decision

If/when NVOFA is implemented:

**Preferred placement is source level.**

Reason:

At source level:

```text
physical camera frame t-1
        ↓
physical camera frame t
```

Motion correspondence has a real interpretation.

At program level during a dissolve:

```text
80% Camera A + 20% Camera B
        ↓
60% Camera A + 40% Camera B
```

there is no single physically correct optical-flow field.

Program-level NVOFA may still be offered, but:
- disable/reduce temporal contribution during transitions;
- retain same-position fallback;
- use transition protection.

Do not let program-level constraints limit the quality of the source-level implementation.

---

# 10. Order relative to HDR Color and HDR Transform

Recommended source-filter chain:

```text
Camera source
    ↓
HDR Denoise
    ↓
HDR Color
    ↓
HDR Transform / perspective / scaling
    ↓
scene
```

Reasons:

### Before Color
A grade can amplify noise, especially shadow lifts.

Denoising first gives `T(Y)` access to a cleaner relationship between scene luminance and camera noise.

### Before Transform
Perspective/scaling resamples the noise and creates spatial correlation.

Denoising before geometry avoids asking the denoiser to interpret interpolation artifacts as camera noise.

### Before transitions
Temporal history remains camera-specific.

This should be the recommended documentation order.

---

# 11. Hybrid deployment is explicitly supported

A useful production arrangement may be:

```text
Camera A -> source temporal
Camera B -> source temporal
Camera C -> source temporal
                    ↓
              composition
                    ↓
          one program spatial
                    ↓
                 output
```

Given the current machine observations, this may be the best GTX 1650 Super configuration.

Another possible arrangement after a GPU upgrade:

```text
Camera A -> source temporal + spatial
Camera B -> source temporal + spatial
Camera C -> source temporal + spatial
                    ↓
              composition
                    ↓
                 output
```

Do not assume which arrangement wins before measurement.

---

# 12. Benchmark matrix

The placement feature must be benchmarked using the user's real scene collection.

Measure at minimum:

| Configuration | Required |
|---|---|
| One source: temporal only | yes |
| One source: spatial only | yes |
| One source: temporal + spatial | yes |
| Three sources: temporal only | yes |
| Three sources: spatial only | yes |
| Three sources: temporal + spatial | yes |
| Program: temporal only | yes |
| Program: spatial only | yes |
| Program: temporal + spatial | yes |
| Three source temporal + one program spatial | **yes — priority hybrid** |

For each record:
- GPU 3D usage;
- actual GPU timer ms if available;
- p95;
- OBS render lag;
- VRAM;
- whether all off-air sources were actively executing;
- multiview on/off;
- transition peak.

Do not rely only on Windows Task Manager percentage. Keep it because it is a useful user-visible signal, but pair it with plugin GPU timing where possible.

---

# 13. Current hardware interpretation

Observed:

```text
1 source spatial ≈ 20% GPU 3D
1 source temporal ≈ no observable GPU usage increase
```

Do not infer:

```text
3 source spatial = exactly 60%
```

because utilization may not scale linearly and other OBS work competes for the same GPU.

But the result clearly says:

> Spatial processing is the component that currently determines whether per-source denoise is practical on the GTX 1650 Super.

Temporal processing does not presently appear to be the capacity problem.

---

# 14. GPU-upgrade policy

The user remains willing to upgrade to approximately an RTX 3070-class or similar GPU if that materially improves quality.

Therefore:

- optimize correctness and algorithm architecture first;
- retain source-level quality modes even if the 1650 Super cannot run three spatial instances comfortably;
- classify modes as current-hardware vs upgrade-target;
- do not degrade the source-level algorithm merely to fit the 1650 Super.

Likely categories:

```text
1650 Super:
    source temporal
    + program spatial
    or selective source spatial

3070-class:
    potentially source temporal + spatial on all cameras
    + future NVOFA
```

Actual classification waits for measurements.

---

# 15. NLMeans status

**Parked.**

Do not let existing NLMeans design work drive placement architecture.

Do not implement:
- NLMeans GPU kernels;
- temporal NLMeans;
- NLMeans source/program variants

until explicitly reactivated.

If future residual-noise analysis shows a real need, NLMeans can use the same placement-capable core/frontend architecture.

---

# 16. Updated implementation priority

Proceed:

```text
1. Complete source-filter frontend
   using the existing denoise core

2. Keep existing program processor

3. Prove HDR identity in both placements

4. Add source history continuity/reset logic

5. Benchmark temporal and spatial independently
   in both placements

6. Benchmark:
   three source temporal
   + one program spatial

7. Complete T(Y) / noise profile abstraction

8. Continue NVOFA feasibility work
   with source-level placement as preferred architecture

9. Tune actual production presets

10. Revisit NLMeans only if later evidence requires it
```

---

# 17. Formal acceptance gates for placement

## PL-G1 — source HDR identity

Neutral source denoise filter:
- preserves exact-value HDR pattern;
- no 75/88/100% ceiling;
- same color space;
- no accidental SDR conversion.

## PL-G2 — program HDR identity

Existing program processor must continue passing the same gate.

## PL-G3 — source continuity

Camera source stops rendering for several frame intervals, then resumes.

PASS:
- history resets;
- no stale ghost frame.

## PL-G4 — three-source temporal

Three camera filters active.

PASS:
- histories independent;
- no cross-camera contamination;
- usable GPU load;
- no render lag.

## PL-G5 — source + transform/color

Chain:

```text
Denoise -> HDR Color -> HDR Transform
```

PASS:
- HDR preserved;
- no ordering artifacts;
- expected denoise result.

## PL-G6 — source versus program visual comparison

At matched algorithm/strength where meaningful:

```text
source-level
vs
program-level
```

Compare:
- noise reduction;
- texture;
- transitions;
- graphics;
- perspective-scaled areas.

## PL-G7 — hybrid configuration

```text
3 × source temporal
+
1 × program spatial
```

Benchmark.

This is a priority production candidate on the current GPU.

---

# 18. Documentation update

Update the user-facing documentation to say:

> **Placement**
>
> For highest denoise quality, add HDR Denoise directly to each camera source before HDR Color, perspective correction and scaling. This gives temporal processing a continuous camera-specific history and avoids denoising already-resampled noise.
>
> If GPU load is a concern, use HDR Program Denoise to process the final composited output once.
>
> A useful hybrid is source-level temporal denoise on each camera with one spatial cleanup pass at program level.

---

# 19. Final product principle

Placement is now part of the denoise design:

> **Temporal intelligence belongs as close to the physical camera as practical; expensive spatial cleanup can be placed either per source or once at program level depending on available GPU headroom.**

The user should not have to choose between quality and efficiency permanently. The plugin should expose both deployment models around one shared HDR-safe denoise core.
