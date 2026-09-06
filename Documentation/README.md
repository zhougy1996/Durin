# Documentation

Use this file only when the task needs repository-specific guidance and the
owning document is not already known. Read the first matching topic below; do
not open the other rows or scan an entire directory.

| Task trigger | Read first |
| --- | --- |
| Repository-owned C++ change | [C++ coding standards](Development/Standards/CodingStandards.md) |
| Affected code module or ownership boundary is unknown | [Code modules](Workspace/CodeModules.md) |
| Setup, build, test, worktree, or runtime problem | [Troubleshooting](Development/Build/Troubleshooting.md) |
| Configuring, building, running, or recovering for task validation | [Agent build and run workflow](Agents/BuildAndRun.md) |
| Changing build/run workflows, output layout, setup, or recovery behavior | [Build and run](Development/Build/BuildAndRun.md) |
| CMake metadata, target generation, or module binaries | [Build system](Development/Build/BuildSystem.md) |
| Runtime variants or build presets | [Runtime variants](Development/Build/RuntimeVariants.md) |
| Tracy or CPU profiling | [CPU profiling](Development/Build/Profiling.md) |
| IDE code model or debugging | [IDE code model](Development/Tooling/IDECodeModel.md) |
| DurinDevTool shell, path discovery, documentation commands, or scaffolding | [DurinDevTool command interface](Development/Tooling/DurinDevTool.md) |
| Dependencies, bootstrap, or worktrees | [Third-party bootstrap](Development/Build/ThirdPartyBootstrap.md) |
| Selecting or running native tests for task validation | [Agent testing workflow](Agents/Testing.md) |
| Advanced native-test selection, modes, diagnosis, aggregates, or CI execution | [Native test execution](Development/Build/NativeTests.md) |
| Adding, classifying, registering, or changing native-test targets | [Native test authoring](Development/Build/NativeTestAuthoring.md) |
| Workspace, project, module, or runtime-variant ownership | [Workspace projects](Workspace/WorkspaceProjects.md) |
| Runtime physical paths or atomic byte publication | [File I/O](Runtime/Core/FileIO.md) |
| Asset paths, package format, references, residency, loading, or compatibility | [Asset packages](Runtime/Assets/AssetPackages.md) |
| Asset catalog discovery, reference indexing, relocation, deletion, or redirector fix-up | [Asset catalog and mutation](Runtime/Assets/AssetCatalogAndMutation.md) |
| Authored sources, derived data, cooking, bulk payloads, or runtime data domains | [Asset data lifecycle](Runtime/Assets/AssetDataLifecycle.md) |
| Field-level BulkData, package-resource ranges, DAST v9 Bulk Directory, or raw `.dbulk` | [Package bulk data](Runtime/Assets/BulkData.md) |
| Async asset compilation domains, aggregate progress, selected finish/cancel, or provider registration | [Asset compilation](Runtime/Assets/AssetCompilation.md) |
| Volume texture source, build, payload, cook, or GPU resource contract | [Volume textures](Runtime/Assets/VolumeTextures.md) |
| Runtime startup, shutdown, or frame lifecycle | [Runtime lifecycle](Runtime/Core/RuntimeLifecycle.md) |
| Async asset build completion, editor commit, rollback, or compensation | [Async asset operations](Editor/Architecture/AsyncAssetOperations.md) |
| Log ordering, queue admission, structured history, sinks, or logger shutdown | [Logging](Runtime/Core/Logging.md) |
| Canonical archives, object serialization, duplication, defaults, or authored override intent | [Serialization](Runtime/Core/Serialization.md) |
| Window decoration modes, custom title bars, or native hit testing | [Window frames](Runtime/Core/WindowFrames.md) |
| Image decoding, Radiance HDR, or grayscale16 PNG | [Core image codec](Runtime/Core/ImageCodec.md) |
| Actor or Component Tick registration, groups, ordering, or mutation | [Tick scheduling](Runtime/World/TickScheduling.md) |
| World subsystem registration, per-World services, or subsystem retirement | [World subsystems](Runtime/World/WorldSubsystems.md) |
| Level ownership, World play state, gameplay session, lifecycle mutation, or Actor iteration | [Level system](Runtime/World/LevelSystem.md) |
| Sandbox gameplay controls, tuning, camera, or ground-plane limits | [Sandbox gameplay](Runtime/Gameplay/SandboxGameplay.md) |
| Core math aliases, operation semantics, or the GLM boundary | [Core math](Runtime/Core/Math.md) |
| Collision shapes, body setup/instance, physics scene, traces, sweeps, or overlaps | [Runtime collision](Runtime/Physics/Collision.md) |
| Physics-framework long-term scaling, broadphase/narrowphase, cooked collision, dynamics, or backend evolution | [Physics evolution roadmap](Roadmaps/Archive/2026-08/PhysicsEvolution.md) |
| CPU tasks, dependencies, cancellation, waiting, or worker-thread ownership | [CPU task system](Runtime/Core/TaskSystem.md) |
| Scene viewport or window-backed rendering | [Viewport rendering](Runtime/Rendering/ViewportRendering.md) |
| Persistent view identity, previous-frame metadata, temporal discontinuity, or history lifetime | [Persistent view state](Runtime/Rendering/PersistentViewState.md) |
| Render-resource state, deferred C++ cleanup, producer teardown, or registry auditing | [Render resource lifecycle](Runtime/Rendering/RenderResourceLifecycle.md) |
| Renderer resource creation failure, retry, fallback, or device invalidation | [Renderer resource recovery](Runtime/Rendering/RendererResourceRecovery.md) |
| Fixed non-Material shader registration, typed global lookup, atomic shader sets, or shader/pipeline generation coupling | [Global shaders](Runtime/Rendering/GlobalShaders.md) |
| Render Graph, RDG, pass declarations, dependencies, barriers, or graph captures | [Render Graph](Runtime/Rendering/RenderGraph.md) |
| Production frame preparation, feature ordering, transient allocation, or output transactions | [Renderer frame preparation](Runtime/Rendering/RendererFramePreparation.md) |
| Rendering statistics overlay, diagnostics panel, or sampled graph inspection UI | [Viewport rendering diagnostics](Editor/Architecture/ViewportRenderingDiagnostics.md) |
| GBuffer encoding, reconstruction, diagnostics, memory, or lifecycle | [Minimal GBuffer contract](Runtime/Rendering/GBuffer.md) |
| Deferred directional lighting, parity, diagnostics, memory, or qualification | [Deferred directional lighting](Runtime/Rendering/DeferredDirectionalLighting.md) |
| Volumetric-cloud components, eligibility, scene selection, or texture-reference recovery | [Volumetric cloud scene contract](Runtime/Rendering/VolumetricCloudSceneContract.md) |
| Volumetric-cloud spatial inputs, compute/fragment fallback, composition order, target budget, or recovery | [Volumetric cloud spatial rendering](Runtime/Rendering/VolumetricCloudSpatialRendering.md) |
| Volumetric-cloud quality tiers, low-resolution reconstruction, per-view history, invalidation, temporal diagnostics, or 4K budgets | [Volumetric cloud temporal reconstruction](Runtime/Rendering/VolumetricCloudTemporalReconstruction.md) |
| Shadow quality, bias, PCF, cascades, or contact shadows | [Directional shadows](Runtime/Rendering/DirectionalShadows.md) |
| Material graph commands, canvas, clipboard, diagnostics, or editor lifecycle | [Material graph authoring](Editor/Architecture/MaterialGraphOperations.md) |
| Editor architecture, design, or user workflow | Use a targeted search under `Editor/Architecture/`, `Editor/Design/`, or `Editor/Guides/` |
| Editor transaction identity, focused records, or collector-enumerated history references | [Transaction record foundation](Editor/Architecture/TransactionRecords.md) |
| Editor transactor, scoped recording, bounded history, structural Undo/Redo, or transaction buffer ownership | [Editor transactor core](Editor/Architecture/Transactors.md) |
| Material-system long-term status, sequencing, or future milestones | [Material system roadmap](Roadmaps/MaterialSystem.md) |
| Bounded implementation task or task selection | Run `.\DevTool.bat doc task list`, then open only the selected task |
| Active implementation plan | Humans run `.\DevTool.bat` for the interactive shell; agents run `.\DevTool.bat doc plan list` to select work, then `.\DevTool.bat doc plan context "<title-or-filename>"` for compact execution context |
| Cross-plan engineering roadmap | Run `.\DevTool.bat doc roadmap list`, then open only the matching roadmap |
| Completed roadmaps awaiting monthly archive | Run `.\DevTool.bat doc roadmap list --scope completed` |
| Named historical roadmap or required provenance | Run `.\DevTool.bat doc roadmap list --scope archive --query "<title-or-filename>"`, then open only the selected archived roadmap |
| Completed plans awaiting monthly archive | Run `.\DevTool.bat doc plan list --scope completed` |
| Named historical plan or required provenance | Run `.\DevTool.bat doc plan list --scope archive --query "<title-or-filename>"`, then open only the selected archived plan |
| Verified unresolved engineering problem | [Open investigations](Investigations/README.md) |

If no row matches, run
`.\DevTool.bat doc find "<task terms>" --limit 5`; its ranked result includes
available summaries and owning modules without printing document bodies. Fall
back to `rg --files Documentation` or a targeted `rg` content query only when
the compact search has no useful result. Open only the closest document and
follow its direct references only when the task requires them. Never load
`Documentation/` as one corpus or maintain a repository-wide file index.

Authoring and lifecycle rules are in the nearest `AGENTS.md`.
