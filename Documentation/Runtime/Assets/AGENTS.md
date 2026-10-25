# Asset Documentation Rules

These instructions apply under `Documentation/Runtime/Assets/` in addition
to the repository documentation rules.

## Domain Ownership

Organize asset contracts by architectural responsibility and use `Modules:` for
source ownership. Place topic documents in these domains rather than at the
Assets root:

- `Packages`: asset identity, mounts, residency/loading, catalog and reference
  projections, mutation/publication, and authored/derived/cooked data lifecycles.
- `Build`: asset compilation, Cook orchestration, target capabilities, derived
  build protocols, and persistent cache services. Keep reusable build/cache
  mechanisms distinct from family-owned recipes, scheduling, object mutation,
  and publication policy even when they share this documentation domain.
- `Families`: concrete asset-family source, import/build, compatibility, payload,
  loading, and GPU-upload contracts, including Texture2D, TextureCube,
  VolumeTexture, and StaticMesh source/building.

Object serialization, reflected BulkData and package-resource substrate, and
cross-layer version compatibility belong under `Runtime/ObjectSystem`. Asset
packages retain their Engine/Registry-facing lifetime and publication contracts
in `Packages`. Renderer scene/batch consumption and shared Material expression
or render behavior belong under `Runtime/Rendering`; editor orchestration and
UI contracts belong under `Editor/Architecture`.

Keep cohesive cross-module and cross-family contracts together and link to
their authority rather than duplicating shared rules. The `Build` domain covers
engine asset production services, not workspace/CMake build workflows under
`Development/Build`.
