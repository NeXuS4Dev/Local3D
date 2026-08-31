# Render graph

`engine/rendergraph` turns "what this frame needs" into "what to record, in which
order, with which memory". Passes are declared with the resources they read and
write; the graph derives everything else.

## Why not hand written frame code

Three concrete payoffs, each of which is tested:

1. **Ordering is derived, not remembered.** A pass may be declared before the
   pass it depends on and still run after it
   (`RenderGraphTests.cpp: orders passes by their data dependencies`).
2. **Unused work disappears.** A pass whose output nothing consumes - and which
   is not an output or a side effect - is culled before any command is recorded.
   The editor can leave five debug views enabled and pay for the ones on screen.
3. **Transient memory has one owner.** The graph allocates, reuses and reports
   every intermediate target.

## The single-writer invariant

Every resource has **exactly one writer**; two passes writing one resource is a
compile error. This is what makes the graph independent of declaration order: a
reader always knows its producer, so edges are `writer -> reader` and nothing
depends on where a pass happened to be declared.

The alternative - "most recent writer in declaration order" - was implemented
first and is wrong: passes declared out of order produced *no* edges at all, so
the graph silently kept declaration order and cycle detection could not fire.
Both failures are covered by tests now.

Resources that legitimately accumulate across frames (TAA history) are declared
`ReadWrite` by one pass, which is a single writer plus a self read; no self edge
is created.

## Compilation

```
RunSetupCallbacks()   // each pass declares its reads and writes
BuildDependencies()   // validate handles, find writers/readers, build edges
CullPasses()          // backwards reachability from outputs and side effects
TopologicalSort()     // Kahn, ties broken by declaration index
ComputeLifetimes()    // firstUse / lastUse per resource, in execution order
```

Culling runs *before* the sort so dead passes cannot constrain the order of live
ones, and before lifetime computation so a culled pass does not extend a
resource's lifetime. A dependency cycle returns an error naming the passes
involved rather than producing a partial order.

Pass kinds have teeth: a raster pass that writes no color or depth attachment is
a validation error, and so is a compute pass that writes a render target.

## Execution and the transient cache

`Execute()` resolves each declared resource (external pointer, cached object, or
a fresh `IDevice::CreateTexture`), binds them into the pass's `PassContext` and
runs the pass between a debug label pair. Transients go back into the cache at
the end of the frame, keyed by their full description, so the next frame reuses
them. Two resources with identical descriptions get **two** cache slots: reuse
takes an entry out of the cache, so nothing can alias a live resource
(`two resources with the same description do not alias`).

`PassContext::Texture()` returns a pointer, not a reference. An undeclared or
unresolvable handle yields `nullptr` plus a validation error; returning a
reference there would mean inventing a dummy object or dereferencing null.

The graph does not create render passes or framebuffers: it hands the pass its
resolved attachments and the command buffer, and the pass (normally the
renderer) builds the pipeline state it needs. Keeping that out of the graph is
what lets the same graph drive a forward and a deferred path.
