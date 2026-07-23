# Active Implementation Plans

These plans have an active execution path. Plan authoring and archival rules are in `AGENTS.md`; completed history is indexed separately in [Archive](Archive/README.md).

| Plan | Primary Scope |
| --- | --- |
| [Actor Component System](ActorComponentSystem.md) | Reflected actor/component ownership, lifecycle, world integration, transforms, scene registration, and serialization |
| [SkyBoxComponent](SkyBoxComponent.md) | Complete vertical slice for the first static cubemap sky background |
| [Texture Support](TextureSupport.md) | Texture2D assets, platform data, material sampling, and validation |
| [Material System](MaterialSystem.md) | Material editing, surface models, shader maps, and runtime materials |
| [Static Mesh Material Slots](StaticMeshMaterialSlots.md) | Mesh-owned stable material-slot identities, sparse component overrides, reimport reconciliation, and fixed-row Details editing |
| [Multithreading V1](MultithreadingV1.md) | Production-safe CPU task states, lifecycle, dependencies, cancellation, parallel loops, and async consumer handoff |
| [Editor Workspace Refactor](EditorWorkspaceRefactor.md) | Editor workspaces, panels, and document lifecycles |
| [Editor Icon Atlas](EditorIconAtlas.md) | Offline atlas pipeline for editor visualization icons |
| [Asset Registry and Thumbnail Cache](AssetRegistryAndThumbnailCache.md) | Persistent asset discovery metadata and rebuildable Content Browser thumbnails |
