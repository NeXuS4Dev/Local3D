# Dependencies

The default answer to "should we depend on this?" is no. Everything the engine
can reasonably own, it owns: math, containers, ECS, reflection, serialization,
the render graph, the physics reference backend and the audio mixer are all
in-tree. That keeps the auditable surface small and avoids inheriting somebody
else's build system, licence and release schedule.

Vendored (checked into `third_party/`, licences alongside):

| Dependency | Justification | Scope |
| --- | --- | --- |
| [doctest](https://github.com/doctest/doctest) | Single header test framework; compiles far faster than Catch2 and needs no build step. | tests only |
| [stb](https://github.com/nothings/stb) | `stb_image` decodes PNG/JPEG/TGA/HDR for texture import. Writing a correct PNG decoder is not a good use of engine time. | asset import only |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | Khronos C headers, so the Vulkan backend type checks on a machine with no SDK. | Vulkan backend only |
| [Dear ImGui](https://github.com/ocornut/imgui) (docking) | Editor UI. Building a docking editor UI from scratch would be a project of its own. | editor only |

Deliberately **not** dependencies:

* **GLM** - `engine/math` owns the conventions that matter (column-major,
  `clip = P * V * v`, Vulkan RH projection with z in [0,1], Gribb-Hartmann
  frustum extraction). Wrapping a library whose defaults differ would mean
  translating at every boundary and still getting handed the wrong projection.
* **nlohmann/json** - the serializer needs *ordered* object keys so dumps diff
  stably in version control, plus a `Result`-returning parser with line numbers.
* **tinygltf** - glTF loading needs the reflection registry and the asset id
  scheme; a thin loader over our own JSON is smaller than adapting it.
* **Jolt / PhysX** - physics has an in-tree `Simple` backend and a pluggable
  interface; Jolt is an option (`L3D_PHYSICS_BACKEND=Jolt`), not a requirement.

## Platform libraries

SDL3 is *optional*: `L3D_HAVE_SDL3` decides whether `Sdl3Platform.cpp` is
compiled. Without it, `CreatePlatformBackend` falls back to the headless backend
and reports that through `outFallback`. Windowing, input and the whole test suite
work either way, which is what lets CI run without a display server.

The Vulkan loader is looked up at configure time (`L3D_VULKAN_LOADER`). When it
is absent the backend still compiles against the vendored headers; entry points
would be loaded at runtime. `L3D_RHI_VULKAN` is off by default until
`src/vulkan/VulkanDevice.cpp` exists, and enabling it without that source is a
configure-time error rather than a silent skip.
