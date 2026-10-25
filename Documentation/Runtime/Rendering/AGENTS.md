# Rendering Documentation Rules

These instructions apply under `Documentation/Runtime/Rendering/` in addition
to the repository documentation rules.

## Domain Ownership

Keep contracts in the domain that owns their primary architectural responsibility;
use `Modules:` for source-module ownership, including cross-module contracts.
Place topic documents in the following domains rather than at the Rendering root:

- `RHI`: portable GPU capabilities, resources, views, transfers, command execution,
  graphics/compute pipelines, synchronization, backend memory, diagnostics, and
  public API stability. Keep backend-specific details with the contract they qualify.
- `Shaders`: shared shader registration, compilation/cache delivery, parameter
  metadata, and binding contracts. Material-specific stage selection stays in
  `Materials`; native pipeline execution stays in `RHI`.
- `Materials`: material assets and render proxies, expression evaluation, stage
  selection, custom data, dependency queries, and invalidation.
- `Scene`: Engine-to-Renderer scene publication, spatial conventions, retained
  geometry/batch inputs, and persistent per-view identity and history ownership.
- `Pipeline`: frame preparation, render graphs, shared frame buffers, display
  mapping, resource readiness/recovery/lifetime, and viewport output/presentation.
- `Features`: individual rendering algorithms and their integration contracts,
  including lighting, shadows, ambient occlusion, clouds, and editor assistance.
  Renderer-owned editor-assistance algorithms belong here; editor UI and user
  workflows belong under `Editor`.

Texture source, import/build, derived payload, cooking, and loading contracts
belong under `Runtime/Assets/Families`, alongside VolumeTextures. Keep shared
Material contracts in `Materials` because they also own expression and render behavior.
Link across domains rather than duplicating shared contracts or splitting a
cohesive cross-module contract solely to match directory or module boundaries.
