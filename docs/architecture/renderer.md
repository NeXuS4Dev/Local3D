# Renderer

`engine/renderer` turns a view plus a list of draw items into recorded GPU work.
It owns no scene: the scene system, an editor viewport and a headless test all
hand it the same `FrameView` / `DrawItem` data, which is why the whole frame can
be exercised in CI against the null RHI.

## What the renderer is responsible for

Culling, lod selection, batching, per-frame uniform upload, frame graph
construction and command recording. What it deliberately does **not** own:

* **Shaders.** `ShaderLibrary` carries the bytecode for every pass. A pass whose
  shader is missing fails `Initialize()` rather than being silently skipped, so a
  half-built shader pipeline cannot look like a working renderer.
* **Geometry ownership.** `MeshData` is uploaded and owned by the renderer, but
  produced elsewhere; the asset pipeline fills the same struct.
* **Scene state.** No entity, transform or material graph lives here.

## Frame structure

```
shadow-csm      depth array, one pass per cascade
depth-prepass   depth only, feeds SSAO
forward-opaque  HDR color, depth tested (not written) against the prepass
ssao            compute, reads depth, writes half-res AO
bloom-down-N    compute downsample chain
bloom-up-N      compute upsample chain, each mip adds the next coarser blur
tonemap         HDR -> output (or the imported present target)
```

The prepass exists because of a real dependency, not for speed: SSAO needs depth
*before* shading, and if the forward pass produced that depth the graph would
contain `forward -> ssao -> forward`. When SSAO is off the prepass is not built
and the forward pass writes depth itself. The forward render pass then loads
depth (`LoadOp::Load`) instead of clearing it - otherwise the prepass would be
thrown away.

Every pass has exactly one writer per resource, which is what the render graph
requires. Bloom therefore uses two chains (downsample targets and upsample
targets) rather than ping-ponging into one texture.

## Batching and instancing

Visible items are grouped by mesh into consecutive runs, and each run becomes one
instanced draw: `DrawIndexed(indexCount, instanceCount, 0, 0, firstInstance)`.
Grouping is by consecutive run rather than by a hash map so the instance order is
stable frame to frame - which keeps GPU-side instance caching and any future
deterministic replay working.

Vertex binding 0 is the mesh (float3 position, float3 normal, float2 uv, 32 byte
stride) and binding 1 is per-instance data (`GpuInstanceData`, 80 bytes: four rows
of the world matrix plus a tint). The instance buffer grows only, never shrinks.

## Culling and lods

Culling is a world-space AABB test against the view frustum, in input order, so
the output order - and therefore batching - is deterministic.

Lod selection is two steps, and the second one matters: the coverage thresholds
pick the *wanted* level, then the selection falls back to the nearest finer lod
and only then to the nearest coarser one. The first implementation returned the
wanted level unconditionally, so an item whose chain was only partially authored
(a two-lod prop still has three empty slots) selected an empty slot and silently
disappeared from the frame.

## Cascaded shadows

Splits blend logarithmic and linear by `ShadowSettings::lambda`, clamped into the
camera range. Each cascade builds a light-space basis over the camera
sub-frustum's eight corners and fits a square ortho box around them.

Two details that are easy to get wrong and are covered by tests:

* **The light's up vector.** A sun pointing straight down is parallel to world
  up, which leaves `LookAt` with a degenerate basis and produces NaNs in every
  shadow matrix - the most common sun direction there is. The basis switches to
  the Z axis when `|lightDir.y| > 0.99`.
* **Texel snapping.** Snapping the box *centre* to the texel grid and rounding
  the radius up to whole texels keeps the box a whole number of texels wide while
  still containing every corner. Snapping the min *and* max bounds independently
  shrinks the box, which pushes slice corners outside clip space and clips
  shadows at the edge of every cascade.

## The camera's up vector, and the light's, are the same bug

`FrameView::Update()` has the same trap as the cascade basis: `LookAtRH` starts
from `Cross(up, forward)`, which is the zero vector when a camera looks exactly
along its up vector. Because `Normalize` returns zero rather than NaN for
degenerate input, the failure is not a crash but a rank deficient view matrix and
a black screen with nothing in the log. `Update()` substitutes the world axis
least aligned with the view direction when the two are within ~2.5 degrees of
parallel, and looks down -Z when the camera sits exactly on its target. The
substitute depends only on the view direction, so it is stable frame to frame.

A camera driven by a transform cannot produce this case - a rotation keeps its
axes orthogonal - which is why it took an editor style free-look camera to
surface. See [scene.md](scene.md) for the cases that do.

## Frame resources are created once

The graph's transient resources are created when the settings, the size, or the
present target changes, and reused for every frame afterwards; only the pass list
is rebuilt per frame (`RenderGraph::ClearPasses`). Creating resources per frame
looked harmless - the graph's transient cache absorbed the allocations, so the
device's texture count stayed flat - while the graph's resource table, and with
it the reported transient memory, grew without bound.
`RenderGraph::Reset()` is the escape hatch when the resource set itself changes.
