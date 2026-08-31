# The asset pipeline

`engine/assets` turns files an artist exports into data the engine loads. It has
four layers, and each one is allowed to know only about the one below it:

```
source files (.glb .png .wav)
      │  AssetDatabase   - identity: what exists, what id it has, is it stale?
      ▼
ImportedAsset            - importers: opaque bytes -> MeshData / ImageData / AudioData
      │  Cooker          - cook: engine data -> flat little endian blobs, named by id
      ▼
cooked files + manifest
      │  AssetManager    - runtime: id -> decoded data, cached
      ▼
MeshData / ImageData     - the renderer uploads them (Renderer::RegisterMesh, UploadTexture)
```

## Identity: the id is not the path

Two different questions need two different answers, and conflating them is what
makes asset systems break the first time someone renames a file:

* **`AssetId`** is permanent. It is a 128 bit `Uuid` stored in a sidecar file
  next to its source (`models/cube.glb.l3dmeta`). Renaming, moving or copying the
  source carries the id with it, so every scene, prefab and material reference
  keeps resolving.
* **`AssetPath`** is a lookup key. It is normalised, root relative and **case
  insensitive** (Windows and macOS are, so pretending otherwise produces projects
  that break on another machine). It is never stored as a reference.

The alternative considered was a central database keyed by path, with a GUID
table. That is what Unity did for years and it is the reason a Unity `.meta`
file exists anyway: the moment the id lives somewhere other than next to the
asset, moving a file in a file manager breaks it. Sidecars are also what makes a
team share ids through version control without merging a binary database.

Two details fall out of this:

1. **A rename that loses its sidecar still keeps its id.** `Scan()` builds a map
   of the content hashes of the files that disappeared and matches them against
   new files with no sidecar. A match is reported as `moved`, not as
   `added` + `removed`, and the cook state is carried over so the file is not
   recooked. Only genuinely new bytes get a fresh id.
2. **A rename that carries its sidecar is also a move.** Same id, new path, old
   path gone - `moved`, and the removal of the old path is undone. If the old
   path is *still there* it was a copy, not a move, which leads to the next rule.
3. **Two files claiming one id never alias each other.** Copying a file together
   with its sidecar (a backup folder, a bad merge) would otherwise leave two
   assets answering to the same id, with the lookup table silently keeping one.
   The second file gets a fresh id, a warning, and a rewritten sidecar.
4. **Two paths that differ only in case are a warning, and the first one wins.**
   `AssetPath::operator==` is case insensitive while `operator<=>` is case
   sensitive, which is deliberate: equality drives lookups, ordering keeps both
   spellings visible so the conflict can be reported instead of silently
   shadowing an asset.

### Sub-asset ids

A GLB holds many meshes, materials and embedded images, so one source produces
many assets. Their ids are `source.Sub(index)` - `MixHash` of the source uuid
mixed with the index - which is deterministic across processes and therefore
usable as a cooked file name. The numbering order is fixed and documented here
because changing it invalidates every cooked mesh in a project:

| index | sub-asset |
| --- | --- |
| `0 … m-1` | meshes, in document order |
| `m … m+n-1` | materials |
| `m+n …` | embedded images |

The **first sub-asset of the primary type keeps the source id itself**
(`SubAssetId(id, 0) == id`). That keeps the useful property that looking up a
source path's id gives you its main asset, whether the source is a PNG (one
asset) or a GLB with four meshes.

## Why `MeshData` lives here and not in the renderer

`MeshData`, `ImageData`, `AudioData` and `MaterialData` are in
`local3d/assets/AssetData.hpp`. They used to be in `renderer/RenderTypes.hpp`,
which put the renderer below the asset pipeline in the dependency graph - the
pipeline produces them, so it would have had to depend on the renderer to do so.
The direction is now `Renderer → Assets`, and `render::MeshData` survives as an
alias so renderer code and tests keep their vocabulary. `engine/assets` is
therefore declared *after* `engine/rhi` in the top level CMakeLists: cooked
textures carry an `rhi::Format` and uploads go through `IDevice`.

The types are plain values with no handles and no device pointers, so they can
cross threads and be cached freely. That is also the reason `MeshData` keeps a
`materialIndex`: a glTF primitive has a material, and dropping the link would be
silent data loss that no test could catch.

## The file system seam

Nothing in this module calls `std::filesystem` or `stdio` directly; everything
goes through `IFileSystem`. That buys three things:

* tests run against `MemoryFileSystem`, so they are fast, parallel safe and
  never leave a temporary directory behind (the whole pipeline - scan, import,
  cook, load, upload - is tested without touching a disk);
* the cooker and the runtime use the same interface, so cooked data can be
  served from a pack or from memory later without touching either;
* every failure is a `Status`, never an exception and never a silent default.

Paths are UTF-8 with `/` separators, and `NormalizePath` rejects anything
containing `..`, so a manifest cannot point outside the root.

## Cooking

Cooking exists so a shipped game starts fast: source parsing happens once, at
build time. Cooked files are

* **named by asset id**, never by source path, so a rename invalidates nothing;
* **one file per sub-asset** (`.l3dmesh`, `.l3dtex`, `.l3daudio`, `.l3dmat`), so
  cooking and streaming are per asset rather than all or nothing;
* **prefixed by a common header**: a four character tag, the format version, the
  source hash and the import fingerprint. A runtime that is handed the wrong
  file, or a cooked file from an older engine, fails with a clear status instead
  of reading garbage.

### What makes a cook incremental

`AssetRecord::NeedsImport()` compares two things against the last successful
cook, both persisted in the sidecar:

* `sourceHash` - FNV-1a over the current bytes;
* `ImportFingerprint()` - `HashOf(importer, importerVersion, settingsHash)`.

The fingerprint is what lets a change with no visible diff in the source - a new
importer revision, or a user ticking `srgb` off - invalidate exactly the assets
it affects. Settings hashes come from `JsonValue::Dump(0)`, which is stable
because `JsonObject` is an ordered map. Hashes are written into JSON as **hex
strings**, not numbers: a JSON number is a double and would silently lose the
top eleven bits of a 64 bit hash.

A second `CookAll()` writes nothing. Deleting a source deletes its cooked files
and drops its manifest entries on the next pass.

### What the runtime reads

`manifest.json` is the runtime's only entry point: id, source path, cooked path,
type, name and dependency ids. `AssetManager::Initialize` fails when it is
missing, because a runtime with no cooked data should die at start up rather
than per asset later.

## Colour space

The classic texture bug is filtering sRGB values directly: they are *encoded*,
so a box filter over them blends in a non linear space and darkens gradients.
`ImageImporter` therefore keeps mip 0 exactly as the source stored it (no lossy
round trip) and, for sRGB assets, decodes to linear, filters, and re-encodes for
every generated mip. **Alpha is always linear** - the transfer function is
defined for colour, not for coverage. Data textures (normal maps, masks) skip
the transfer functions entirely, which is why `srgb` is a per-asset setting
rather than a guess from the file name.

HDR sources (`.hdr`) are stored as `RGBA16_Float`; 8 bit sources as `RGBA8_SRGB`
or `RGBA8_UNorm`. Everything is normalised to four channels so the renderer
never has to care how many the source had. The float→half conversion rounds to
nearest with ties to even and flushes subnormals, which image data never needs.

## Importers

An importer takes bytes plus settings and returns data. It knows nothing about
ids, the file system or the GPU, which keeps it testable and lets the cooker run
one on a worker thread.

**Importers must be deterministic.** The cook hash decides whether work is
skipped, so a non deterministic importer would silently serve stale assets.

Things an importer must not do, all of which are covered by tests:

* **silently drop data.** An unsupported primitive mode or a missing image is a
  warning in the `ImportLog`, and the message is prefixed with the source path by
  the cooker. What cannot be salvaged at all is an error status.
* **read past the end.** Every accessor is bounds checked against the bin chunk,
  including the stride and the last element, and every RIFF chunk against the
  file size. Truncated input is a `ParseError`.
* **guess.** Node transforms are deliberately *not* baked into imported
  geometry: a mesh stays in its own local space and the scene layer owns
  transforms. Baking them would make the same mesh imported twice through
  different nodes produce different bytes, and would put scene-graph policy
  inside a file parser.

The GLB and WAV readers are hand written; see
[dependencies.md](dependencies.md) for why that is smaller than adapting
tinygltf. Supported GLB features: POSITION/NORMAL/TEXCOORD_0, indexed and non
indexed triangles, interleaved or packed views, all five component types
including normalised integers, PBR metallic-roughness materials, external and
embedded images. Not supported, and reported: sparse accessors, external buffer
files, non triangle primitive modes, morph targets, skins. Missing normals are
generated area weighted (unnormalised face normals accumulated per vertex) and
missing uvs are zeroed - both with a warning.

## Runtime cache lifetime

`AssetManager` hands out `const T*` into its cache. The rules, because this is
where asset systems crash:

* pointers stay valid until `Release(id)` or `ClearCache()`;
* loading another asset never invalidates a pointer already handed out - entries
  are held through `unique_ptr`, so the map can rehash freely;
* **nothing is evicted behind your back.** There is no budget driven eviction
  yet, so a long running game has to release what it stops using. That is a
  deliberate gap rather than a hidden policy: an LRU that frees a mesh the
  renderer is still drawing is worse than no eviction at all.

## Invariants the code relies on

1. `record.cookedSourceHash == record.sourceHash` **and**
   `record.cookedImportHash == record.ImportFingerprint()` means the cooked file
   on disk matches the source. `NeedsImport()` is the negation.
2. A scanned tree is idempotent: scanning an unchanged root reports
   `added == removed == modified == moved == 0`.
3. Cooked file names are a pure function of the asset id and type.
4. `mesh.positions.size() == normals.size() == uvs.size()`, `indices.size() % 3
   == 0`, `indices.size() >= 3` - `MeshData::IsValid()`, enforced by the importer
   and re-checked by the cooked reader.
5. `image.pixels.size()` equals the sum of `MipBytes(mip)` over the chain; the
   importer asserts it before returning and `UploadTexture` checks it again
   before handing spans to the device.
6. Every cooked reader validates its tag and version before reading a single
   field, and bounds every count (`kMaxReasonableCount`) before allocating.
