# Active Implementation Plans

These plans have an active execution path. Plan authoring and archival rules are in `AGENTS.md`; completed history is indexed separately in [Archive](Archive/README.md).

| Plan | Primary Scope |
| --- | --- |
| [Project And Module Scaffolding](ProjectAndModuleScaffolding.md) | Transactional BuildTool workflows for module creation, workspace project creation, descriptor/CMake registration, and typed project launch |
| [Actor Component System](ActorComponentSystem.md) | Reflected actor/component ownership, lifecycle, world integration, transforms, scene registration, and serialization |
| [Texture Support](TextureSupport.md) | Texture2D assets, platform data, material sampling, and validation |
| [Static Mesh Derived Data and Cooking](StaticMeshDerivedDataAndCooking.md) | Source-model provenance, native mesh payloads, DDC caching, cooking, and shared material-preview assets |
| [Material System](MaterialSystem.md) | Material editing, surface models, shader maps, and runtime materials |
| [Multithreading V1](MultithreadingV1.md) | Production-safe CPU task states, lifecycle, dependencies, cancellation, parallel loops, and async consumer handoff |
| [Resource Dependency Updates](ResourceDependencyUpdates.md) | Forward material dependency queries, batched loaded-object scans, and registration-free render invalidation |
| [Editor Icon Atlas](EditorIconAtlas.md) | Offline atlas pipeline for editor visualization icons |
| [Windows Long Paths and Atomic File I/O](WindowsLongPathsAndAtomicFileIO.md) | Windows process long-path capability, Core-owned atomic publication, diagnostics, and long-worktree validation |
