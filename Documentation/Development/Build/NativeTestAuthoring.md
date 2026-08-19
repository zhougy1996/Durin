# Native Test Authoring

Last reviewed: 2026-08-20

This document defines native-test source ownership, target declarations,
deployment, writable sandboxes, lifecycle isolation, and resource policy.
Selection, execution modes, aggregate behavior, and failure diagnosis are
defined by [Native Test Execution](NativeTests.md).

## Source Layout

- Native test targets: `Engine/Tests/Native/<TargetName>/`
- Target-owned checked-in inputs: `Engine/Tests/Native/<TargetName>/Data/`
- Checked-in inputs shared by multiple targets: `Engine/Tests/Data/<FixtureName>/`

A large target composition may keep its shared declaration helper in the owning
`CMakeLists.txt` and include domain-oriented fragments from a target-local
`CMake/` directory. The owning root includes those fragments exactly once in
registry/discovery order; each complete target declaration, including links,
runtime dependencies, execution properties, finalization, and discovery,
stays together in one fragment.

Keep target-owned inputs beside the target that owns them. Promote an input to
the shared data directory only when multiple test targets depend on the same
fixture.

## Output Layout

- Test executables: `Engine/Binaries/<Platform>/<Config>/Tests/<Profile>/Bin/`
- Deployed read-only inputs: `<TestRoot>/Data/`
- Per-target sandbox parent: `<TestRoot>/Work/`
- Per-process writable sandbox: `<TestRoot>/Work/Runs/run-p<PID>-<nonce>/`

Test executables and their runtime DLLs share `Bin/`. Deployment helpers create
one build target per engine DLL or external runtime file, so every destination
has one writer even when many native-test targets require it. Do not add
target-owned `POST_BUILD` copies into the shared directory.

`durin_register_native_test(...)` derives each test's deployable runtime closure
from the final `LINK_LIBRARIES` and `INTERFACE_LINK_LIBRARIES` target graph before
it registers GoogleTest discovery. Shared and module libraries contribute their
binaries; static, object, and interface targets are traversed without a copy.
Imported targets contribute files published through
`DURIN_RUNTIME_DEPLOY_FILES`, so the target which introduces Assimp, Slang, or
another external runtime owns its deployment metadata. Test declarations do
not repeat ordinary transitive DLL or external-file lists.

`Data` is input-only. Never create, update, rename, or delete files there.
`Work` is only a parent managed by the test harness; tests must not write
directly beneath it. Every process receives a canonical unique directory under
`Work/Runs`, returned by `Durin::Testing::GetTestWorkDirectory()`. Logical
fixture names may repeat across processes because they resolve below different
sandboxes.

Successful runs delete their sandbox. Failed runs and runs using
`--durin-keep-test-work` print and retain it for diagnosis. The harness
periodically removes successful directories left behind by cleanup failures
after 24 hours, but skips the current process and every PID that may still be
alive. Crash directories and intentionally retained directories have no
success marker and are never removed by this periodic cleanup.

GoogleTest death-test children allocate a unique nested directory below their
parent run. A successful parent removes those child directories; a failed
parent retains the complete tree with the ordinary diagnostic sandbox.

Create a clean named fixture and perform recursive cleanup through the support
library:

```cpp
const std::filesystem::path Root =
    Durin::Testing::CreateTestFixtureDirectory("PackageRoundTrip");

// Use Root for every generated file.

Durin::Testing::RemoveTestWorkDirectory(Root);
```

`CreateTestFixtureDirectory` rejects absolute paths and traversal outside the
current process sandbox. `RemoveTestWorkDirectory` rejects the sandbox root,
checked-in `Data`, shared `Work`, and every other outside path. Do not call
`std::filesystem::remove_all` in native-test code; repository configuration
checks enforce use of the contained cleanup API.

## Add A Test Target

Create a feature/lifecycle target, not a target that mirrors a production
module. A target should own suites that share a coherent setup and dependency
stack. Every `.cpp` and every GoogleTest suite has exactly one target owner;
configuration rejects unowned sources, duplicate source or suite registration,
and the retired module-wide catch-all target names.

### Target Classification

Test kinds describe behavior, not expected duration:

- `contract`: API, representation, validation, and state-machine boundaries.
- `feature`: one coherent user-facing or runtime capability.
- `infrastructure`: native-test discovery, isolation, deployment, or execution.
- `integration`: real multi-system, runtime-lifecycle, backend, process, or
  hardware composition.
- `characterization`: explicitly admitted observation of exceptional or legacy
  behavior.
- `qualification`: explicitly admitted performance, scale, memory, or hardware
  baselines.

Minimal pattern:

```cmake
include(GoogleTest)

add_durin_test(PackageRoundTripTests
    Private/AssetImportTests.cpp
)

target_link_libraries(PackageRoundTripTests PRIVATE
    AssetCore
)

durin_register_native_test(PackageRoundTripTests
    KIND feature
    DOMAINS asset-package
    MODULES asset-core
)
```

`durin_register_native_test(...)` is the single registration boundary. It must
appear after sources, links, runtime-only dependencies, data deployment, and
execution properties are known. It finalizes metadata, resolves policy, derives
the runtime closure, and registers GoogleTest discovery. `KIND` is exactly one of
`contract`, `feature`, `integration`, `characterization`, `infrastructure`, or
`qualification`; `DOMAINS` contains at least one stable selection slice.
Optional `MODULES`, `BACKENDS`, and `STACKS` aid discovery but never replace
real link, runtime-only dependency, resource-lock, or timeout declarations.
Targets are case-parallel by default. Add `SERIAL` only when the target requires
broad target serialization, together with a concrete
`DURIN_TEST_TARGET_LOCK_RATIONALE`. Use `TIMEOUT <seconds>` to override the
300-second default. Characterization kind automatically suppresses the direct
whole-target lifecycle registration.

`EXECUTION_HOST` on `durin_register_native_test(...)` declares the process
lifecycle required by the target and is independent of serialization, labels,
and locks. Omit it for the `direct` default; use
`EXECUTION_HOST application` when a target initializes Cocoa, AppKit, a GLFW
Cocoa window, a Metal layer, or native presentation. On macOS, CMake resolves
`application` to the repository LaunchServices host and uses `TEST_LAUNCHER`
for both `PRE_TEST` GoogleTest discovery and execution. Platforms where an
ordinary process already owns the required application lifecycle may resolve
the same declaration to direct execution. Do not create target-local `.app`
wrappers or call `open` manually.

Repository declarations that resolve to the macOS application host must be
guarded by `DURIN_ENABLE_APPLICATION_TESTS`. The option defaults to `OFF`; an
unguarded application declaration is a configuration error rather than being
silently executed as a direct process.

The macOS controller preserves the exact GoogleTest arguments and environment,
publishes stdout/stderr and the signal-derived child result, and cleans only
the retained Host and child PIDs for its invocation. Successful control
directories are removed; failed, crashed, timed-out, cancelled, or malformed
invocations retain bounded evidence below the owning test's
`Work/ApplicationHost` directory. When explicitly enabled, each
application-hosted target keeps its executable and deployment closure in its
own `Bin` directory beside `Data` and `Work`; Host, Controller, and Probe
infrastructure remains in the ordinary build tree. No application-test output
is redirected to `/private/tmp`. An unauthorized external-volume checkout is
expected to fail LaunchServices admission until the user approves it. Direct
targets retain the ordinary output layout below.

Metadata values are lowercase slugs matching
`[a-z][a-z0-9]*(-[a-z0-9]+)*`; duplicate values are errors and emitted values
are sorted. The label prefixes `kind-`, `domain-`, `module-`, `backend-`, and
`stack-` are reserved for generated metadata. Do not add them through
`DURIN_TEST_LABELS`. Structured declarations cannot compile a production
`Private/*.cpp` directly unless the owning module is named by both
`PRIVATE_SOURCE_OWNER` and `MODULES` and a reviewed
`PRIVATE_SOURCE_RATIONALE` explains the same-owner white-box seam. Feature and
integration targets should normally link the production boundary instead.
Do not aggregate unrelated production logic into a library solely to make it
test-linkable. A small module-owned test target may compile the specific
private implementation it exercises when exporting a DLL symbol or inventing
a production component would distort the production architecture.

Configuration writes the deterministic registry to
`<BuildDir>/DurinNativeTestRegistry.json`. Its schema version and
source/binary/preset/configuration identity belong to CMake. DurinDevTool
rejects a missing, unsupported, or identity-mismatched registry and asks for a
fresh configure rather than selecting from stale metadata.

Complete every target declaration before calling
`durin_register_native_test(...)`. Registration is the runtime-closure
finalization point as well as the test-policy and discovery point. Configuration
fails when a target-bearing link expression cannot be resolved for the active
preset, a referenced runtime target is missing, two external files claim the
same destination name from different sources, or a manual deployment repeats
an ordinary derived dependency.

Register a test target only in configurations that provide every capability it
exercises. In particular, targets that link editor-only modules or consume
editor-only offline build services must be guarded by `DURIN_WITH_EDITOR`.
When a mixed-capability integration target remains useful in runtime-only
builds, keep its runtime cases registered and explicitly skip only the case
whose documented prerequisite is unavailable.
Guard that case's editor-only headers, helper implementations, and link edges
with the same capability condition so the runtime target does not acquire an
authoring or Build dependency merely to compile a skipped case.

An owning test helper may expose an `EDITOR_ONLY` option when the prerequisite
is not represented by an editor-module link edge, such as source decoding or
offline cooking. The option must still register the target's exact source list
as a configuration exclusion in runtime-only builds.

The source-ownership validator still covers configuration-excluded `.cpp`
files. In the unavailable branch, register each exact source through
`durin_exclude_native_test_sources(RATIONALE ... SOURCES ...)`; directory or
pattern exclusions are unsupported, and the rationale must name the missing
configuration capability.

Use a runtime-only exception only for a plugin, delay-loaded module, or file
which is selected without a CMake link edge. Declare the owner and rationale
before registration:

```cmake
durin_test_register_runtime_only_dependencies(RendererIntegrationTests
    RATIONALE "RHIInit selects the Vulkan backend dynamically at runtime."
    TARGETS VulkanRHI
)
```

The helper also accepts `FILES` for a genuinely unlinked runtime file. It
rejects a target already present in the ordinary link closure; add or correct
the link edge instead. Repository policy rejects target-owned `POST_BUILD`
runtime copies. The lower-level explicit deployment helpers remain build-system
primitives, not native-test authoring requirements.

`add_durin_test(...)` links `NativeTestSupport`, generates the harness entry
point, and provides `DURIN_TEST_DATA_DIR` for input lookup. Use
`durin_test_deploy_directory_to_data(...)` or
`durin_test_deploy_files_to_data(...)` for checked-in inputs. Always register
through `durin_register_native_test(...)`; direct `gtest_discover_tests(...)`
boilerplate bypasses repository policy and is rejected.

Every ordinary target receives one direct target-lifecycle registration and
must reset mutable process-global state between tests and release owned
resources at its target boundary. If suites cannot safely share those
lifecycles, split them into cohesive targets instead of selecting routine case
execution. Checked-in data is read-only; generated assets, caches, databases,
and external-tool outputs must resolve below the current process sandbox.
Intentional multi-process, crash, and abrupt-exit characterization belongs in
a separately registered characterization target with a concrete rationale.
Performance, scale, memory, and hardware-baseline measurements belong in a
separately registered qualification target. Keep a bounded correctness case in
the ordinary owning target, and run the qualification target explicitly. A
physical-resource lock such as `durin-gpu` describes lifecycle ownership; it
does not make a correctness target a performance test.

Case-level parallel safety is the default for explicit case diagnostic mode. If
a target cannot run cases concurrently, add `SERIAL` to its registration and
set `DURIN_TEST_TARGET_LOCK_RATIONALE` to a concrete reviewed reason. Broad
`durin-test-target-*` locks without that rationale are rejected. Explicit
shared resources must come from the central registry in
`CMake/Project/ProjectTargets.cmake`:

- `durin-gpu`: exclusive ownership of the physical GPU/device lifecycle.

Use `DURIN_TEST_RESOURCE_LOCKS durin-gpu` only when a target owns that physical
resource. Do not invent lock names. Targets that directly link `Renderer`,
`VulkanRHI`, `DurinEd`, `Mona`, or `MonaImGui` must also provide a
`DURIN_TEST_HEAVY_RUNTIME_RATIONALE`; this keeps feature targets narrow and
makes their dependency cost reviewable.

Tests should avoid editor startup or real window creation unless that behavior
is under test. When it is, keep the target scoped to that lifecycle and declare
the corresponding rationale and resource ownership explicitly.

## Related Docs

- [Native Test Execution](NativeTests.md)
- [Agent Testing Workflow](../../Agents/Testing.md)
- [Build System](BuildSystem.md)
- [Third-Party Bootstrap](ThirdPartyBootstrap.md)
