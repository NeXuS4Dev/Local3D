# Source assets

This directory is the **project asset root**: the files an artist or a tool
drops in, exactly as they came out of Blender, GIMP or a DAW. Nothing here is
shipped. The shipped data is the cooked output, which lives in a separate root
(`build/cooked/` by default) and is never committed.

```
assets/
  models/    .glb          -> cooked to <uuid>.l3dmesh + <uuid>.l3dmat
  textures/  .png .jpg     -> cooked to <uuid>.l3dtex
  audio/     .wav          -> cooked to <uuid>.l3daudio
```

Two kinds of generated files live next to the sources:

* `<file>.l3dmeta` — the sidecar that holds the **stable asset id**, the
  importer settings and the hashes of the last successful cook. Commit these.
  The id, not the path, is what scenes and materials reference, so renaming a
  file keeps every reference to it alive.
* `.l3dindex.json` — a cache of the scan so the editor can list assets without
  walking the tree. The sidecars remain the source of truth, so the index is
  gitignored and deleting it only costs one rescan.

## Cooked files

Cooked files are named by asset id (`a1b2...c3.l3dmesh`), never by source path,
so a rename does not invalidate anything that refers to the asset. Each one
starts with a shared header — tag, format version, source hash and the import
fingerprint it was produced from — so the runtime rejects a stale or foreign
file instead of reading garbage. The cook root also holds `manifest.json`, the
only file the runtime reads at start up.

See [`docs/architecture/assets.md`](../docs/architecture/assets.md) for why the
pipeline is split this way.
