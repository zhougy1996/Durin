# Documentation

Use this file only when the task needs repository-specific guidance and the
owning document is not already known. Search task terms in the table rather than
printing it in full, or run `.\DevTool.bat doc find "<task terms>" --limit 5`.
Add coding standards, domain contracts, and validation guidance as the task
requires; do not read unrelated topics or scan an entire directory.

| Task trigger | Read first |
| --- | --- |
| Repository-owned C++ change | [C++ coding standards](Development/Standards/CodingStandards.md) |
| Affected code module or ownership boundary is unknown | [Code modules](Workspace/CodeModules.md) |
| Setup, build, test, worktree, or runtime problem | [Troubleshooting](Development/Build/Troubleshooting.md) |
| Configuring, building, running, or recovering for task validation | [Agent build and run workflow](Agents/BuildAndRun.md) |
| Changing build/run workflows, output layout, setup, or recovery behavior | [Build and run](Development/Build/BuildAndRun.md) |
| AI-assisted local toolchain configuration or setup checks | [DevTool setup configuration](Development/Tooling/DevToolSetup.md) |
| Reflected type identity, registration, class defaults, or enum metadata | [Reflection system](Runtime/ObjectSystem/ReflectionSystem.md) |
| Runtime transaction capture, custom-change participants, or transaction object contracts | [Runtime transactions](Runtime/ObjectSystem/Transactions.md) |
| DPROPERTY types, typed metadata, containers, or serialized field compatibility | [Property reflection](Runtime/ObjectSystem/PropertyReflection.md) |
| DSTRUCT value capabilities, detached storage, logical comparison, or type defaults | [Reflected struct operations](Runtime/ObjectSystem/StructOperations.md) |
| DHT parsing, reflected symbol exports, or generated C++ | [Reflection generation](Development/Tooling/ReflectionGeneration.md) |
| DFUNCTION native invocation, inherited CallInEditor buttons, or action transactions | [Native function reflection](Runtime/ObjectSystem/FunctionReflection.md), [Editor function actions](Editor/Architecture/PropertyEditing/EditorFunctionActions.md) |
| CMake metadata, target generation, or module binaries | [Build system](Development/Build/BuildSystem.md) |
| Runtime variants or build presets | [Runtime variants](Development/Build/RuntimeVariants.md) |
| Tracy or CPU profiling | [CPU profiling](Development/Build/Profiling.md) |
| IDE code model or debugging | [IDE code model](Development/Tooling/IDECodeModel.md) |
| DurinDevTool shell, path discovery, documentation commands, or scaffolding | [DurinDevTool command interface](Development/Tooling/DurinDevTool.md) |
| Toolset definitions, explicit native bindings, tool schemas, or exact tool invocation | [Toolset contracts](Runtime/Core/Tools.md) |
| Slow tasks, nested work progress, feedback contexts, delayed dialogs, or cooperative cancel feedback | [Slow task feedback](Runtime/Core/SlowTasks.md) |
| Editor toolset catalog, queued invocation, cancellation, detached endpoints, or external adapter seam | [Editor tool service](Editor/Architecture/Tools/EditorTools.md) |
| Console tool syntax, JSON argument files, merged help/completion, or PIE tool calls | [Console Toolset integration](Editor/Architecture/Tools/ConsoleTools.md) |
| Local MCP transport, external Toolset discovery/calls/cancellation, or client connection setup | [MCP adapter](Editor/Architecture/Tools/MCPTools.md), [connection guide](Editor/Guides/LocalMCP.md) |
| Embedded Python scripts, reflected type/object wrappers, Python Toolset calls, script jobs, or interpreter lifecycle | [Python adapter](Editor/Architecture/Tools/PythonTools.md), [script guide](Editor/Guides/PythonScripts.md) |
| Runtime HTTP listeners, routes, chunked responses, or transport shutdown | [HTTP server](Runtime/Networking/HTTPServer.md) |
| Dependencies, bootstrap, or worktrees | [Third-party bootstrap](Development/Build/ThirdPartyBootstrap.md) |
| Selecting or running native tests for task validation | [Agent testing workflow](Agents/Testing.md) |
| Advanced native-test selection, modes, diagnosis, aggregates, or CI execution | [Native test execution](Development/Build/NativeTests.md) |
| RHI, RDG, or Renderer performance baselines, measurement inventory, or comparison protocol | [Rendering performance baseline](Development/Build/RenderingPerformanceBaseline.md) |
| GPU qualification admission, quiet timing lanes, or Vulkan creation memory measurements | [Native test qualification](Development/Build/NativeTestQualification.md) |
| Adding, classifying, registering, or changing native-test targets | [Native test authoring](Development/Build/NativeTestAuthoring.md) |
| Workspace, project, module, or runtime-variant ownership | [Workspace projects](Workspace/WorkspaceProjects.md) |
| Runtime physical paths or atomic byte publication | [File I/O](Runtime/Core/FileIO.md) |
| Native dynamic libraries, loader diagnostics, reference lifetime, or Unicode library paths | [Platform libraries](Runtime/Core/PlatformLibraries.md) |
| Child process arguments, working directories, or environment overrides | [Platform processes](Runtime/Core/PlatformProcesses.md) |
| Asset architecture domain ownership or contract placement | [Asset domain rules](Runtime/Assets/AGENTS.md) |
| Asset paths, package format, references, residency, loading, or compatibility | [Asset packages](Runtime/Assets/Packages/AssetPackages.md) |
| StaticMesh source ownership, detached building, or payload compatibility | [Static mesh building](Runtime/Assets/Families/StaticMeshBuilding.md) |
| Asset catalog discovery, reference indexing, relocation, deletion, or redirector fix-up | [Asset catalog and mutation](Runtime/Assets/Packages/AssetCatalogAndMutation.md) |
| Offline Cook dependency discovery, reuse, or transactional output publication | [Asset cooking](Runtime/Assets/Build/Cooking.md) |
| Target/profile parsing, immutable build policy, or family target capabilities | [Target platform build policy](Runtime/Assets/Build/TargetPlatforms.md) |
| Authored sources, derived data, cooking, bulk payloads, or runtime data domains | [Asset data lifecycle](Runtime/Assets/Packages/AssetDataLifecycle.md) |
| Build definitions, captured input identity, typed recipe adapters, or shared DDC execution | [Derived data build protocol](Runtime/Assets/Build/DerivedDataBuild.md) |
| Persistent DDC manifests, value attachments, identity versions, selective queries, codec budgets, diagnostics, or offline cache maintenance | [Derived data cache storage](Runtime/Assets/Build/DerivedDataCacheStorage.md) |
| Field-level BulkData, package-resource ranges, DAST Bulk Directory, or raw `.dbulk` | [Package bulk data](Runtime/ObjectSystem/BulkData.md) |
| Engine release, Archive/custom versions, or package compatibility policy | [Versioning](Runtime/ObjectSystem/Versioning.md) |
| Async asset compilation domains, aggregate progress, selected finish/cancel, or provider registration | [Asset compilation](Runtime/Assets/Build/AssetCompilation.md) |
| Sky Light sources, procedural capture, Studio migration, or GPU environment filtering | [Sky lighting](Runtime/Rendering/Features/SkyLighting.md) |
| Volume texture source, build, payload, cook, or GPU resource contract | [Volume textures](Runtime/Assets/Families/VolumeTextures.md) |
| Runtime startup, shutdown, or frame lifecycle | [Runtime lifecycle](Runtime/Application/RuntimeLifecycle.md) |
| Async asset build completion, editor commit, rollback, or compensation | [Async asset operations](Editor/Architecture/Assets/AsyncAssetOperations.md) |
| Log ordering, queue admission, structured history, sinks, or logger shutdown | [Logging](Runtime/Core/Logging.md) |
| Engine-free package saves, async completion, or staged package persistence | [Package persistence](Runtime/ObjectSystem/PackagePersistence.md) |
| Canonical archives, object serialization, duplication, defaults, or authored override intent | [Serialization](Runtime/ObjectSystem/Serialization.md) |
| Window decoration modes, custom title bars, or native hit testing | [Window frames](Runtime/Application/WindowFrames.md) |
| Image decoding, Radiance HDR, or grayscale16 PNG | [Core image codec](Runtime/Core/ImageCodec.md) |
| Actor or Component Tick registration, groups, ordering, or mutation | [Tick scheduling](Runtime/World/TickScheduling.md) |
| Gameplay delay, looping callbacks, next-frame scheduling, or World time scale | [World timers](Runtime/World/WorldTimers.md) |
| Common subsystem lifecycle, Engine services, or scope selection | [Subsystems](Runtime/Engine/Subsystems.md) |
| Editor subsystem services, Actor/Component structural editing, or notification lifetime | [Editor subsystems](Editor/Architecture/Framework/EditorSubsystems.md) |
| World subsystem registration, per-World services, or subsystem retirement | [World subsystems](Runtime/World/WorldSubsystems.md) |
| Level ownership, World play state, gameplay session, lifecycle mutation, or Actor iteration | [Level system](Runtime/World/LevelSystem.md) |
| Sandbox gameplay controls, tuning, camera, or ground-plane limits | [Sandbox gameplay](Runtime/Gameplay/SandboxGameplay.md) |
| Logical input actions, binding overrides, context priority, UI capture, or cancellation | [Input actions](Runtime/Gameplay/InputActions.md) |
| Core math aliases, operation semantics, or the GLM boundary | [Core math](Runtime/Core/Math.md) |
| Collision shapes, body setup/instance, physics scene, traces, sweeps, or overlaps | [Runtime collision](Runtime/Physics/Collision.md) |
| Physics-framework long-term scaling, broadphase/narrowphase, cooked collision, dynamics, or backend evolution | [Physics evolution roadmap](Roadmaps/Archive/2026-08/PhysicsEvolution.md) |
| CPU tasks, dependencies, cancellation, waiting, or worker-thread ownership | [CPU task system](Runtime/Core/TaskSystem.md) |
| Scene viewport or window-backed rendering | [Viewport rendering](Runtime/Rendering/Pipeline/ViewportRendering.md) |
| Persistent view identity, previous-frame metadata, temporal discontinuity, or history lifetime | [Persistent view state](Runtime/Rendering/Scene/PersistentViewState.md) |
| Render-resource state, deferred C++ cleanup, producer teardown, or registry auditing | [Render resource lifecycle](Runtime/Rendering/Pipeline/RenderResourceLifecycle.md) |
| Loading-time material program, native Shader/PSO preparation, or scene activation readiness | [Load preparation](Runtime/Rendering/Pipeline/LoadPreparation.md) |
| Renderer resource creation failure, retry, fallback, or device invalidation | [Renderer resource recovery](Runtime/Rendering/Pipeline/RendererResourceRecovery.md) |
| Fixed non-Material shader registration, typed global lookup, atomic shader sets, or shader/pipeline generation coupling | [Global shaders](Runtime/Rendering/Shaders/GlobalShaders.md) |
| Render Graph, RDG, pass declarations, dependencies, barriers, or graph captures | [Render Graph](Runtime/Rendering/Pipeline/RenderGraph.md) |
| RHI public API stability, experimental headers, backend-only interfaces, deprecation, or compatibility | [RHI public API stability](Runtime/Rendering/RHI/RHIPublicAPIStability.md) |
| Production frame preparation, feature ordering, transient allocation, or output transactions | [Renderer frame preparation](Runtime/Rendering/Pipeline/RendererFramePreparation.md) |
| Rendering statistics overlay, diagnostics panel, or sampled graph inspection UI | [Viewport rendering diagnostics](Editor/Architecture/LevelEditing/ViewportRenderingDiagnostics.md) |
| GBuffer encoding, reconstruction, diagnostics, memory, or lifecycle | [Minimal GBuffer contract](Runtime/Rendering/Pipeline/GBuffer.md) |
| Deferred directional lighting, parity, diagnostics, memory, or qualification | [Deferred directional lighting](Runtime/Rendering/Features/DeferredDirectionalLighting.md) |
| Volumetric-cloud components, eligibility, scene selection, or texture-reference recovery | [Volumetric cloud scene contract](Runtime/Rendering/Features/VolumetricCloudSceneContract.md) |
| Volumetric-cloud spatial inputs, compute/fragment fallback, composition order, target budget, or recovery | [Volumetric cloud spatial rendering](Runtime/Rendering/Features/VolumetricCloudSpatialRendering.md) |
| Volumetric-cloud quality tiers, low-resolution reconstruction, per-view history, invalidation, temporal diagnostics, or 4K budgets | [Volumetric cloud temporal reconstruction](Runtime/Rendering/Features/VolumetricCloudTemporalReconstruction.md) |
| StaticMesh retained batch inputs, publication invalidation, or capture lifetime | [Retained StaticMesh batch inputs](Runtime/Rendering/Scene/StaticMeshBatchPublication.md) |
| Shadow quality, bias, PCF, cascades, or contact shadows | [Directional shadows](Runtime/Rendering/Features/DirectionalShadows.md) |
| Loaded material dependency queries, batch caches, or notification reentrancy | [Scoped material queries](Runtime/Rendering/Materials/MaterialQueries.md) |
| Material geometry/Pass usage, required shader stages, or vertex/pixel artifact reuse | [Shader stage selection and sharing](Runtime/Rendering/Materials/MaterialShaderStages.md) |
| Material vertex roots, World Position Offset, or vertex-to-pixel interpolation | [Material vertex evaluation](Runtime/Rendering/Materials/MaterialVertexEvaluation.md) |
| Primitive or Instance custom numeric channels, stable render instances, or material variation without separate instances | [Primitive and Instance material data](Runtime/Rendering/Materials/MaterialCustomData.md) |
| Material expression emission, output registration, graph traversal, or function invocation caches | [Material expression building](Runtime/Rendering/Materials/MaterialExpressionBuilding.md) |
| Material graph commands, function authoring, UV inputs, transactions, or clipboard | [Material graph operations](Editor/Architecture/Materials/MaterialGraphOperations.md) |
| Material editor panels, canvas geometry, node menus, or graph diagnostics | [Material graph canvas](Editor/Architecture/Materials/MaterialGraphCanvas.md) |
| Material preview resources, Compile/Apply/Save, document lifecycle, or instance overrides | [Material editor lifecycle](Editor/Architecture/Materials/MaterialEditorLifecycle.md) |
| Editor architecture domain ownership or contract placement | [Editor Architecture domain rules](Editor/Architecture/AGENTS.md) |
| Editor architecture, design, or user workflow | Use a targeted search within the relevant `Editor/Architecture/` domain, `Editor/Design/`, or `Editor/Guides/` |
| Reflected edit targets, detached validation, object notifications, or edit sessions | [Reflected property editing](Editor/Architecture/PropertyEditing/ReflectedPropertyEditing.md) |
| Details layouts, categories/search, custom property widgets, or host view lifetime | [Property Details views](Editor/Architecture/PropertyEditing/PropertyDetailsViews.md) |
| Level Editor Actor/Component Details, aggregation, or material-slot controls | [Level Editor Details](Editor/Architecture/LevelEditing/LevelEditorDetails.md) |
| Editor transaction identity, whole-object or array operation records, or collector-enumerated history references | [Transaction record foundation](Editor/Architecture/Transactions/TransactionRecords.md) |
| Editor transactor, scoped recording, bounded history, structural Undo/Redo, or transaction buffer ownership | [Editor transactor core](Editor/Architecture/Transactions/Transactors.md) |
| Material-system long-term status, sequencing, or future milestones | [Material system roadmap](Roadmaps/MaterialSystem.md) |
| Bounded implementation task or task selection | Run `.\DevTool.bat doc task list`, then open only the selected task |
| Active implementation plan | Humans run `.\DevTool.bat` for the interactive shell; agents run `.\DevTool.bat doc plan list` to select work, then `.\DevTool.bat doc plan context "<title-or-filename>"` for compact execution context |
| Cross-plan engineering roadmap | Run `.\DevTool.bat doc roadmap list`, then open only the matching roadmap |
| Completed roadmaps awaiting monthly archive | Run `.\DevTool.bat doc roadmap list --scope completed` |
| Named historical roadmap or required provenance | Run `.\DevTool.bat doc roadmap list --scope archive --query "<title-or-filename>"`, then open only the selected archived roadmap |
| Completed plans awaiting monthly archive | Run `.\DevTool.bat doc plan list --scope completed` |
| Named historical plan or required provenance | Run `.\DevTool.bat doc plan list --scope archive --query "<title-or-filename>"`, then open only the selected archived plan |
| Verified unresolved engineering problem | [Open investigations](Investigations/README.md) |

If routing leaves the owning document unclear, run
`.\DevTool.bat doc find "<task terms>" --limit 5`; its ranked result includes
available summaries and owning modules without printing document bodies. Fall
back to `rg --files Documentation` or a targeted `rg` content query only when
the compact search has no useful result. Start with the closest document and
expand through direct references or targeted searches when task scope,
dependencies, or validation requirements remain unclear. Never load
`Documentation/` as one corpus or maintain a repository-wide file index.

## Reading Reference Sections

Read applicable `AGENTS.md` files and required `Agents/` workflows in full.
For reference documents, first locate relevant headings or symbols, then read
complete matching sections and any directly required contract dependencies.
For example, on Windows:

```powershell
rg -n '^#{1,3} |TextureCoordinates|UV' Documentation/Editor/Architecture/Materials/MaterialGraphOperations.md
Get-Content <document-path> | Select-Object -Skip <start-line-minus-one> -First <line-count>
```

Keep each output bounded (usually one section or about 120 lines); long lines
may require smaller ranges. Do not batch full bodies of long documents. If a
tool truncates output, narrow the search or range before continuing; truncation
does not count as reading the omitted rules. Expand when ownership, invariants,
dependencies, or validation remain unclear. Reuse sections already read unless
they changed or their context is unavailable.

Authoring and lifecycle rules are in the nearest `AGENTS.md`.
