# Error handling

## Three tiers

**`Result<T>` (= `Expected<T, Status>`)** for operations that can legitimately
fail: creating a device or resource, parsing a file, compiling a graph. The type
is `[[nodiscard]]`, so ignoring a failure is a warning, and `Value()`/`Error()`
assert on the wrong branch so a misuse fails loudly in a debug build.

`Status` is a code plus a short inline message (`kStatusMessageCapacity` bytes).
It is trivially copyable and allocation free because it is returned from hot
paths. The code list is deliberately small - subsystems add *context* through the
message, not new codes.

**Validation errors** for misuse of a valid object: drawing outside a render
pass, writing an undeclared graph resource, uploading past the end of a buffer.
These are logged and counted (`IDevice::ValidationErrorCount()`,
`RenderGraph::ValidationErrorCount()`) but do not abort the frame. A malformed
descriptor, by contrast, comes back as an error `Result` from the call that
created it. The distinction is: *the caller can act on a `Result` now; a
validation entry is a bug report*.

**`L3D_ASSERT`** for invariants that should be impossible - a null where the
contract forbids it, an index the caller already validated. Asserts are compiled
out in shipping builds, which is why every assert sits behind a check that also
produces a `Result` or a validation entry.

## Why not exceptions

Exceptions are not disabled, but the engine neither throws nor catches. Failure
handling is explicit at every call site, an error path costs nothing until it is
taken, and the C ABI boundary used by the scripting layer cannot carry a C++
exception. Monadic combinators (`AndThen`, `Map`, `OrElse`) are provided for
chaining without nesting.
