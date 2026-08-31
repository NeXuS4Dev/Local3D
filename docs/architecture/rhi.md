# RHI

`engine/rhi` is the seam between the renderer and the graphics API. It exists so
that the renderer, the render graph and the asset pipeline never mention Vulkan,
and so that a validating CPU backend can run the whole frame in CI.

## What the abstraction is allowed to look like

The API maps 1:1 onto explicit APIs (Vulkan today, D3D12/Metal reserved). There
are no convenience calls that would not survive a port: descriptor sets, render
passes, framebuffers, pipeline layout ids, push constant ranges and explicit
command buffer recording are all part of the interface. Anything "friendlier"
would either leak a backend's model or force the slowest common path.

Descriptors (`BufferDesc`, `TextureDesc`, `PipelineDesc`, ...) are plain
copyable data. They are validated by the backend that consumes them, so a
descriptor can be stored, hashed, serialised and diffed - which is what the
asset pipeline and the editor's pipeline inspector need.

`BackendType` and `ShaderFormat` are the only places a backend name appears in a
public header, and they appear as *data* (for reporting and asset cooking), never
as types.

## Ownership and deferred destruction

The engine holds `ResourcePtr<T>`, a `std::unique_ptr<T, GpuResourceDeleter>`.
The deleter does **not** delete: it calls `GpuResource::Release()`, which hands
the object to its device (`RhiResources.cpp`). The device keeps it in a deferred
list until

```
frameNumber - releaseFrame >= frameCount
```

because frames that are still in flight may read the object. `WaitIdle()` drains
the list immediately. `MemoryReport` exposes both live and deferred counts, so a
leak and a frame-latency backlog are distinguishable.

This is the one place where the "no raw owning pointers" rule needs a documented
exception: the device owns a `GpuResource*` in its deferred list. It is safe
because the pointer is only reachable from that list and is deleted exactly once,
by `ProcessDeferredDeletions`.

`GpuResource` itself carries the id, debug name and owning device pointer. That
keeps `Release()` non-virtual and removes three boilerplate overrides from every
backend resource class - the earlier design had each backend class re-implement
them and it was pure noise.

## Frame model

```
BeginFrame()   // retire deferred deletions, advance FrameIndex()
  ... record, Submit(finishedCommandBuffer) ...
EndFrame()     // advance FrameNumber()
```

`Submit()` requires a *finished* recording (`Begin` ... `End`). A buffer that is
still recording, was never begun, or was already submitted is a validation
error; re-record before resubmitting. This mirrors explicit APIs, where a
command buffer is reset before reuse, and it caught a real bug: the first
implementation validated *and* re-entered `End()`, which double-reported every
error.

## The null backend is the specification

`src/null/NullDevice.cpp` implements the whole interface in memory. It is not a
stub: it validates and it accounts.

* usage flags on every bind and copy (`Vertex`, `Index`, `Indirect`,
  `CopySource`, `CopyDestination`),
* render pass / framebuffer attachment count, format and extent,
* pipeline vs descriptor set layout agreement, and descriptor writes against the
  layout's bindings,
* push constant ranges against `DeviceInfo::maxPushConstantSize`,
* mip counts, cube layer multiples and per-mip upload sizes,
* command buffer state: begun, inside a render pass, pipeline bound, viewport
  set, every declared vertex binding bound,
* `ICommandBuffer::Stats` counts draws, dispatches, copies, binds and timestamps
  so renderer and graph tests can assert on recorded work rather than pixels.

The contract for failures is explicit and mirrored by every backend:

* a **malformed descriptor** returns an error `Result` from the creation call;
* **misuse of a valid object** (drawing outside a render pass, uploading past the
  end of a buffer) is recorded through `ReportValidationError` - logged under
  `LogCategory::Rhi` and counted by `ValidationErrorCount()`.

The split matters: the first is a programming error the caller learns about
immediately, the second must not abort a frame in a shipping build.

## Backend registration

`CreateDevice()` looks the preferred backend up in a registry. The null backend
registers itself through `null::RegisterNullBackend()`, which `CreateDevice`
calls explicitly on every request.

A static initialiser would not work: when the RHI is a static library, the linker
drops an object file that defines no referenced symbol, and the registration
silently never runs. The test suite caught exactly that (24 of 26 cases failed to
create a device) before the explicit call replaced it.
