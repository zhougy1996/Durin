# Native Test Process Isolation Stage 0 Evidence

This document freezes the native-test topology and collision baseline used by
`NativeTestProcessIsolation.md`. It is an implementation input, not the
authoritative developer workflow.

Baseline source commit after rebase: `88e54d76`

## Inventory method

- The seven legacy executables contain 91 test source files, 120 GoogleTest
  suites, and 647 cases.
- Suite names were extracted from `TEST`, `TEST_F`, and `TEST_P` registrations.
  Every source file is assigned once by the table below. Repeated fixture names
  occur only inside one proposed target.
- `DURIN_TEST_WORK_DIR` occurs 138 times in 42 native-test source/header files.
  `std::filesystem::remove_all` occurs in 32 files.
- `DURIN_TEST_DATA_DIR` is read by JSON/YAML, asset import, shader reflection,
  material/static-mesh, texture, sky-box, thumbnail, and Vulkan sampling tests.
  The corresponding new target receives only that data subset.
- No native test binds a fixed network port. The Tracy `8086-8105` text in
  `ProfilingTests.cpp` is a diagnostic assertion, not a listener.

## Proposed functional targets

All targets use the default timeout class unless a different class is listed.
`filesystem` means the state becomes process-sandbox local. `process` is a
temporary named compatibility group. `gpu` is an irreducible physical-device
resource lock.

| Proposed target | Sources from the legacy target | Cases / suites | Direct dependency and lifecycle contract | Data / resource policy |
| --- | --- | ---: | --- | --- |
| `CoreUtilityTests` | `CoreTests`: console command, GUID, JSON, logger, name, profiling, string, transform, version, xxHash, YAML | 67 / 11 | `Core`; no object or renderer bootstrap | JSON/YAML data; filesystem |
| `CoreFileSystemTests` | `CoreTests`: derived-data cache, file helper, lexical path, paths, project | 31 / 7 | `Core`; reset mount registry and path overrides per case | project config and path fixtures; filesystem |
| `CoreConcurrencyTests` | `CoreTests`: delegate and threading | 34 / 9 | `Core`; thread-pool creation/stop belongs to each case | no data; process until global-pool audit passes |
| `CoreObjectTests` | all `CoreDObjectTests` sources | 49 / 3 | `Core`, `CoreDObject`; explicit object-system/GC lifecycle | no data; process until object lifecycle audit passes |
| `AssetPackageTests` | `PackageTests.cpp` | 20 / 1 | `Core`, `CoreDObject`, `AssetCore`; package registry and mounts reset per case | filesystem |
| `AssetCookTests` | `CookedAssetTests.cpp` | 11 / 4 | `Core`, `CoreDObject`, `AssetCore`; cooked-mode context owned by cases | filesystem; extended timeout |
| `AssetDerivedDataTests` | `DerivedDataObjectStoreTests.cpp` | 3 / 1 | `Core`, `AssetCore`; derived-data override restored per case | filesystem |
| `AssetDecodeTests` | `ImageDecoderTests.cpp` | 7 / 1 | `Core`, `AssetCore`; no object/renderer bootstrap | decoder inputs generated in sandbox |
| `AssetImportTests` | `AssetImportTests.cpp` | 8 / 1 | `Core`, `AssetCore`, `AssetImport`, Assimp; thread pool explicit | `Tests/Data/AssetImport`; filesystem; extended timeout |
| `RenderShaderContractTests` | shader foundation and reflection sources | 25 / 2 | `Core`, `RHI`, `RenderCore`, Slang; compiler lifetime owned by target | shader data; process compiler group; extended timeout |
| `RenderShaderCacheTests` | shader cache/store source and its private production sources | 13 / 2 | `Core`, `RHI`, `RenderCore`, Slang; mount/cache reset per case | shader data; filesystem; extended timeout |
| `RenderShaderServiceTests` | shader compile-service source and its private production sources | 3 / 1 | `Core`, `RHI`, `RenderCore`, Slang; service shutdown is mandatory | shader data; process compiler group; extended timeout |
| `RenderContractTests` | render-target-layout and RHI-texture contract sources | 10 / 2 | `Core`, `RHI`, `RenderCore`; no physical device initialization | no data; parallel after static-state audit |
| `EditorPropertyTests` | reflected-property view and the three reflected-property editor sources | 25 / 2 | object system plus editor transaction state; no renderer | no data; process until transaction globals are reset |
| `EditorAssetWorkflowTests` | asset destination/upgrade, content-browser item/model, import dialog, source-library contract/index | 32 / 8 | `CoreDObject`, `AssetCore`, `Engine`, narrow LevelEditor model sources | source-reference data; filesystem |
| `MaterialTests` | material sources and material-parameter panel model | 56 / 4 | object system, assets, renderer; owns RHI startup/teardown because `FMaterialTests` includes Vulkan-backed cases | asset-import data; filesystem plus `gpu`; extended timeout |
| `StaticMeshTests` | static-mesh sources, slot details, material upgrade, and update | 26 / 6 | object system, assets, engine, BC7/Assimp where used | asset-import data; filesystem |
| `TextureTests` | texture build/derived-data/failure/import sources, equirectangular and cube sources | 43 / 4 | object system, assets, engine, render command queue; no RHI startup | sky-box and panorama data; filesystem |
| `ThumbnailTests` | asset/source-image/material/cube thumbnail and rendered-fixture sources | 44 / 5 | object system, assets, renderer thumbnail providers; fixture mount belongs to target support | sky-box data; filesystem; process until cached fixture removal |
| `WorldTests` | all `World` sources | 35 / 7 | object system and engine world lifecycle; no device startup | filesystem |
| `ViewportTests` | all `Viewport` sources | 43 / 17 | object system, world, LevelEditor viewport/model sources; no device startup | filesystem |
| `SplineTests` | `SplineTests.cpp` | 10 / 4 | object system, engine spline component, editor transactions | filesystem |
| `SkyBoxTests` | sky-box component, editor, and rendering contract sources | 8 / 3 | object system, engine, render-command contracts; no RHI startup | sky-box/panorama data; filesystem |
| `SkyBoxVulkanIntegrationTests` | `SkyBoxVulkanTests.cpp` | 1 / 1 | full renderer and Vulkan RHI startup/teardown | sky-box/panorama data; `gpu`; integration timeout |
| `EditorRenderingTests` | editor grid, renderer assistance/layout, editor texture smoke | 9 / 4 | renderer contracts plus narrow editor sources; no RHI startup | asset-import data; filesystem |
| `EditorShellTests` | notification, workspace, UI style, console-record model | 27 / 7 | ApplicationCore/Mona/editor shell lifecycle; no renderer | no mutable deployed data; process until UI globals reset |
| `ExternalToolTests` | profiling-tool service | 5 / 1 | ApplicationCore and MainFrame profiling service; child-process ownership explicit | filesystem; external-process label; extended timeout |
| `TextureCookIntegrationTests` | existing `TextureCookTests.cpp` target | 1 / 1 | cooked runtime mode and renderer/Vulkan teardown are process-global | filesystem plus `gpu`; integration timeout |
| `VulkanRHIIntegrationTests` | existing Vulkan sampling source | 1 / 1 | Vulkan device, Slang, and RHI startup/teardown | Vulkan shader data; `gpu`; integration timeout |

The source assignment totals 91 files, 647 cases, and 120 unique suites. A
static cross-domain fixture-name check reports zero duplicated suite owners.

## Suite ownership

The compact suite map below is the review surface for the topology. Source
files sharing one fixture name remain in the same target.

- `CoreUtilityTests`: `FConsoleCommandTests`, `FEngineVersionTests`,
  `FGuidTests`, `FJsonDocumentTests`, `FLoggerTests`, `FNameTests`,
  `FProfilingTests`, `FStringHelperTests`, `FTransformTests`, `FXxHashTests`,
  `FYamlDocumentTests`.
- `CoreFileSystemTests`: `FDerivedDataCacheTests`, `FFileHelperTests`,
  `FLexicalPathTests`, `FMountRegistryTests`, `FPathsTests`,
  `FProjectHistoryTest`, `FProjectTests`.
- `CoreConcurrencyTests`: all delegate, thread-pool, runnable-thread, task, and
  thread-event suites.
- `CoreObjectTests`: `FCoreDObjectReflectionTests`,
  `FPropertyChangeEventTests`, `FPropertyValueSnapshotTests`.
- Asset targets own `FPackageAssetTests`; the four cooked suites;
  `FDerivedDataObjectStoreTests`; `FImageDecoderTests`; and
  `FAssetImportTests`, respectively.
- Shader targets own the foundation/reflection, cache/utilities, and
  compile-service suites, respectively. `RenderContractTests` owns
  `FRenderTargetLayoutTests` and `FRHITextureTests`.
- `EditorPropertyTests` owns `FReflectedPropertyViewTests` and
  `FReflectedPropertyEditSessionTests`.
- `EditorAssetWorkflowTests` owns the destination, structure-upgrade,
  content-browser, import-dialog, source-path, and source-index suites.
- `MaterialTests` owns `FMaterialTests`, `FMaterialDependencyTests`,
  `FMaterialUpdateContextTests`, and `FMaterialParameterPanelModelTests`.
- `StaticMeshTests` owns the payload, derived-data, upgrade, update, and slot
  detail suites.
- `TextureTests` owns `FTexture2DTests`, `FTextureDerivedDataTests`,
  `FTextureCubeTests`, and `FEquirectangularTextureCubeTests`.
- `ThumbnailTests` owns all five asset/source/rendered thumbnail suites.
- `WorldTests` owns the world, level, scene component, directional light,
  engine-object, camera component, and camera-editing suites.
- `ViewportTests` owns its 17 transaction, camera, selection, projection,
  visualization, customization, and viewport-client suites.
- `SplineTests` owns the four spline component/curve/editing/reflection suites.
- CPU sky-box suites remain in `SkyBoxTests`;
  `FSkyBoxVulkanTests` moves to `SkyBoxVulkanIntegrationTests`.
- Editor rendering owns the grid, assistance, target-layout, and texture-smoke
  suites. Editor shell owns console, notification, layout/style, picker, and
  workspace suites. `ExternalToolTests` owns `FProfilingToolServiceTests`.
- Cooked texture and Vulkan sampling retain dedicated integration owners.

## Shared support ownership

Stage 2 should create small object libraries or test-support libraries instead
of restoring a catch-all executable:

- `NativeTestSupport`: common main, process sandbox, cleanup listener, and
  sandbox path API; depends only on Core and GoogleTest.
- `EditorPropertyTestSupport`: reflected-property editor fixtures.
- `MaterialTestSupport`: material/mesh asset construction helpers.
- `TextureTestSupport`: texture imports and derived-data override RAII.
- `ThumbnailTestSupport`: rendered fixture creation; it must not retain a
  process-static fixture or path.
- `WorldTestSupport`, `ViewportTestSupport`, and `SkyBoxTestSupport`: feature
  lifecycle fixtures with no cross-library static ownership.
- Narrow object libraries own the private production `.cpp` files currently
  compiled directly into `RenderCoreTests` and `EngineTests`. They are linked
  only by the functional target exercising that implementation.

## Shared-state audit

- All writable paths derived from `DURIN_TEST_WORK_DIR` are classified
  `filesystem` and migrate below the process sandbox.
- Mount registries, derived-data overrides, object-system state, thread pools,
  editor transactions, UI registries, renderer globals, and cooked-runtime mode
  are process-global. Each target above declares its bootstrap contract and
  remains in a named compatibility group until teardown is proven.
- Path-capturing statics requiring removal or sandbox-keying exist in material
  static-mesh fixtures, texture-cube fixtures, sky-box support, and
  `RenderedAssetThumbnailTestFixtures.h`. The latter also caches raw object
  pointers in a process-static `std::optional`.
- Physical GPU access is limited to the material Vulkan cases, sky-box Vulkan,
  cooked texture runtime, and Vulkan RHI sampling. These use one documented
  `durin-gpu` resource lock. Renderer/RHI value-contract tests do not.
- The profiling service owns spawned Tracy processes and temporary output, but
  no fixed port. It receives an external-process label rather than a global
  network lock.

## Characterization regression

`NativeTestIsolationProbeTests` contains two discovered cases that coordinate
through a control directory and both use `same-logical-name.txt`.

- With `DURIN_TEST_ISOLATION_PROBE_MODE=legacy`, both cases resolve the shared
  target `Work` root; at least one case fails after observing the peer's value.
- With `DURIN_TEST_ISOLATION_PROBE_MODE=isolated`, the probe substitutes
  distinct per-process roots and both cases pass.
- The discovered cases skip in routine CTest runs unless the probe mode is
  explicitly set. Stage 3 removes the probe-only root substitution and runs
  them against `GetTestWorkDirectory()`.
- `NativeTestIsolationProbeCharacterization` is a non-default target. Invoked
  through DurinDevTool, it runs only the two discovered cases at `-j 2`, asserts
  that `legacy` returns failure, then asserts that `isolated` returns success.

## Process directory decision

- Portable process identity is supplied by a tiny platform adapter:
  `GetCurrentProcessId()` on Windows and `getpid()` on POSIX.
- A run directory is created atomically with bounded retries using
  `run-p<PID>-<128-bit-lowercase-hex-nonce>`. The nonce comes from
  `std::random_device`; uniqueness is accepted only after
  `std::filesystem::create_directory` succeeds.
- The harness writes `run.json` with target, PID, nonce, start time, selected
  filter, and outcome. Failed/crashed directories retain their original run
  name rather than being renamed, avoiding cross-volume and open-handle races.
- Successful runs are removed. A retained failure is diagnosed by its
  `run.json`; cleanup errors add `cleanup-error.txt` inside the same directory.
  `DURIN_KEEP_TEST_WORK=1` records outcome `kept` and preserves successful work.

## Aggregate failure baseline

The machine-readable baseline is the
[archived baseline](../../Plans/Archive/2026-07/NativeTestProcessIsolationBaseline.json).
It records the 2026-07-28
18-job run: 647 cases, 31 failures, one skip, and 6.90 seconds elapsed. The
dominant signatures are shared-root deletion races, missing files during
copy/load/publication, shader-cache publication failures, and missing or
concurrently rebuilt thumbnail mounts/fixtures.
