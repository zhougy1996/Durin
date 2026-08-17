# Native C++ Tests

This is the complete native-test specification for test authors and test
infrastructure work. Agents selecting routine task validation should first use
the short [Agent Testing Workflow](../../Agents/Testing.md). Native tests are
available from every registered build preset; the default is
`Win64-Debug-DurinEditor`.

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

## Run Tests

Build and run a test executable through the root wrapper:

```powershell
.\DevTool.bat test CoreUtilityTests
.\DevTool.bat test CoreUtilityTests FJsonDocumentTests.ParseObjectFromString
.\DevTool.bat test fast-all
.\DevTool.bat test "@viewport"
.\DevTool.bat test all
```

`test fast-all` is the local feedback profile. It selects every configured
`contract`, `feature`, and `infrastructure` target while excluding all
`integration`, `characterization`, and `qualification` targets. It is a
convenience selection rather than a new test kind: use the affected named
target or domain when a change touches integration behavior, and retain
`test all` for the complete ordinary correctness aggregate.

The first command runs one target process. The second passes a GoogleTest
filter. Direct-hosted exact targets retain the focused executable path;
application-hosted exact targets run the same whole-target CTest registration
used by bounded sets so the platform launcher remains in force. The `@viewport`
command resolves a configured domain set, prints the exact target list, builds
only those executables and their dependency closures, and runs their CTest
registrations. A test executable has a 300-second timeout
by default; `--timeout <seconds>` changes it, and `--timeout 0` disables it for
an intentionally long diagnostic run. The timeout starts after the target has
finished building.

Discover configured choices without building them:

```powershell
.\DevTool.bat test list
.\DevTool.bat test list viewport
.\DevTool.bat test list private-sources
.\DevTool.bat test explain "@domain=viewport,backend=vulkan"
```

`test list private-sources` reports targets retaining an explicitly owned
production-private source seam. The report is derived from the active registry
and may be empty.

Set selectors start with `@`. `@viewport` is shorthand for
`@domain=viewport`. Within a dimension, `+` is union; comma-separated
dimensions intersect. For example,
`@kind=feature+integration,domain=viewport,backend=vulkan` selects Vulkan
viewport feature or integration targets. Exact target names take precedence
over set syntax. An empty result is an error and never falls back to `all`.
Ordinary selectors exclude characterization and qualification targets.

Execution scenarios keep the routine path short:

```powershell
.\DevTool.bat test "@viewport" ViewportSuite.Resize --mode isolation
.\DevTool.bat test "@viewport" --mode stress
.\DevTool.bat test "@viewport" --mode report
.\DevTool.bat test "@kind=characterization,domain=launch" --mode characterization
.\DevTool.bat test "@kind=qualification,domain=terrain" --mode qualification
```

Isolation requires a bounded selection and case filter. Stress mode randomizes
CTest scheduling and GoogleTest order, printing a reproducible seed. Report
mode writes JUnit XML under
`Build/NativeTestResults/<Preset>/<Selection>.xml` unless `--report <path>` is
given. Characterization and qualification admission are always explicit.
Qualification targets own performance, scale, memory, or hardware-baseline
measurements that should not extend routine correctness feedback.

Choose validation by risk and preserve the resolved target names in the
handoff or CI log:

- Routine changes run the smallest named target that owns the changed behavior.
- Broad local non-integration feedback runs `test fast-all`; it never replaces
  an affected integration target or backend/domain set.
- Cross-module behavior runs its feature domain, such as `test "@world"` or
  `test "@viewport"`; reproduce the result with the resolved named targets
  printed before execution.
- Backend-specific behavior intersects the domain with a backend, such as
  `test "@domain=viewport,backend=vulkan"`.
- Child-process, crash, or launcher behavior selects `stack=process`; explicit
  characterization additionally requires `--mode characterization`.
- Performance and scale qualification selects `kind=qualification` and requires
  `--mode qualification`.
- Shared discovery, registry, harness, locking, deployment, or aggregate
  changes run `test all` at default target granularity.

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

Scheduled/nightly repository validation owns the ordinary native aggregate.
Release qualification owns that aggregate, the explicit qualification set,
and any platform/backend matrix required by the release. A failed set is
diagnosed with its printed named targets and narrow case filters; local
implementation and handoff validation remain risk-based and do not inherit the
whole scheduled matrix.

`test all` builds the `DurinNativeTests` aggregate and then runs every
ordinary target once through CTest. Characterization and qualification targets
are neither built nor run by this aggregate. This `target` granularity is the
default.
Use `--granularity case` to run every discovered GoogleTest case in a separate
process for isolation diagnosis and independence qualification. Do not run
unfiltered `test all --granularity case`; diagnose an ordinary aggregate
failure with `test <FailedTarget> <Suite.Case>`, or use a narrow
case-name `--ctest-regex` when CTest-level isolation is required. After
diagnosis, use default target granularity for the final full-suite validation
unless the change specifically targets case isolation. `hybrid` is a
compatibility spelling which selects the same registrations as `target`.
Native-test executables and GoogleTest are excluded from CMake's
default `all` target, so routine `build` and `rebuild` commands do not compile
tests even when the selected preset enables `BUILD_TESTING`.
Its timeout applies to each CTest-registered test. GoogleTest `--filter` syntax
is executable-specific and therefore cannot be combined with `test all`.
The compatibility options `--schedule-random` and `--output-junit <path>`
retain their prior aggregate behavior. In target
and hybrid modes the command also prints and forwards a GoogleTest shuffle seed
so order failures can be reproduced with `GTEST_RANDOM_SEED`. Use
`--ctest-regex <regex>` only with case granularity for an isolated rerun of a
matching case registration. These options require `test all`. `--target`,
`--granularity`, `--ctest-regex`, `--schedule-random`, and `--output-junit`
remain temporarily accepted, emit a deprecation warning, and are hidden from
routine help. Existing repository automation may keep them while it moves to
positional selections and named modes.

Use a focused `test <Target> <GoogleTestFilter>` command for the
fastest failing-case iteration. It launches one target process with the filter;
aggregate granularity does not change focused execution.

`native-test-characterization` is always excluded from the aggregate and runs
only through its owning custom target. Such a dedicated target sets
`DURIN_TEST_DIRECT_LIFECYCLE FALSE` because its custom runner, rather than a
routine whole-executable smoke, owns the required environment and scheduling.

Do not record a current test or registration total in repository documentation.
CTest discovery is the source of truth; use the command summary or
`--output-junit` when a review needs an auditable count.

DurinDevTool clears build recovery state before launching the test executable. A failed assertion, crash, timeout, or interrupted test should be diagnosed and rerun with `test`; it does not require `rebuild`. Build ownership, recovery, and parallelism rules are documented in `Documentation/Development/Build/BuildAndRun.md`.

`NativeCrashCharacterizationTests` is the separately isolated native-crash
target. Its parent process launches one runtime child per intentional fault,
waits no more than 15 seconds, and validates the native exit status plus the
context, dump, and completion marker before the retained sandbox can be
cleaned. The target carries a runtime-stack rationale because a native fault
cannot be characterized safely inside the GoogleTest process. Current
supported fixtures cover read, write, and execute access violations,
`std::terminate`, a worker-thread access violation, logger-tail gaps, dump
failure, collision, unwritable roots, and recursive writer failure. Policy
tests cover path admission, age/count retention, partial cleanup, and
directory-link avoidance. Simultaneous/recursive faults remain best-effort and
stack overflow remains deferred as documented in
[Native Crash Diagnostics](../../Runtime/Core/NativeCrashDiagnostics.md).

In the interactive shell, use the equivalent commands:

```text
DurinDevTool> preset Win64-Debug-DurinEditor
DurinDevTool> test CoreUtilityTests
DurinDevTool> test CoreUtilityTests FJsonDocumentTests.ParseObjectFromString
DurinDevTool> test all
```

DurinDevTool rejects `test` for an IDE-only or custom preset that does not
enable `BUILD_TESTING`.

On macOS, the default `MacOS-arm64-Debug-DurinEditor` preset sets the
application-test capability to its default of `OFF`. Tests that require
LaunchServices, together with their Host and Controller infrastructure, are
omitted from that preset's configured registry and build graph. Enable the
capability explicitly only in a checkout selected for application validation:

```bash
./DevTool configure -DDURIN_ENABLE_APPLICATION_TESTS=ON
./DevTool test NativeTestApplicationExecutionTests
```

`-DNAME=VALUE` (or `--define NAME=VALUE`) forwards a repeatable CMake cache
override through the ordinary configure command. The example reuses the normal
`MacOS-arm64-Debug-DurinEditor` build directory and may run from an
external-volume checkout after macOS has received the required interactive
LaunchServices permission. Run ordinary `./DevTool configure` afterward to
reapply the preset's explicit `OFF` default. A main checkout on an internal
volume remains the recommended unattended validation lane.

This is an explicit optional validation lane, not a routine requirement. Do
not enable or execute application-hosted tests unless the user, the selected
plan gate, or the active CI job specifically requests that coverage. When the
current sandbox or graphical session cannot use LaunchServices, configuration
and compilation may still be checked, but application execution remains not
run and must be reported that way. Do not escape the current sandbox, change
macOS authorization, relocate the test artifacts, or use the product
application merely to satisfy this optional coverage.

For diagnosis, the corresponding executable is under
`Engine/Binaries/Win64/Debug/Tests/DurinEditor/Bin/` and may be run directly
with normal GoogleTest arguments. Direct and filtered runs use the same
process-isolation harness as CTest. Add `--durin-keep-test-work`, or set
`DURIN_TEST_KEEP_WORK=1`, to retain a successful run's files. CTest discovery
state is in `Build/Win64-Debug-DurinEditor`.

Do not run an application-hosted macOS executable directly for diagnosis; that
bypasses LaunchServices admission. Use the ordinary DevTool target, filtered,
isolation, stress, report, or qualification command and inspect the retained
control directory printed by a failed launcher invocation. These tests require
an active graphical login session. A locked or missing GUI session is a real,
bounded test failure rather than a skip.

## Output Layout

- Test executables: `Engine/Binaries/<Platform>/<Config>/Tests/<Profile>/Bin/`
- Deployed read-only inputs: `<TestRoot>/Data/`
- Per-target sandbox parent: `<TestRoot>/Work/`
- Per-process writable sandbox: `<TestRoot>/Work/Runs/run-p<PID>-<nonce>/`

Test executables and their runtime DLLs share `Bin/`. Deployment helpers create
one build target per engine DLL or external runtime file, so every destination
has one writer even when many native-test targets require it. Do not add
target-owned `POST_BUILD` copies into the shared directory.

`durin_discover_tests(...)` derives each test's deployable runtime closure from
the final `LINK_LIBRARIES` and `INTERFACE_LINK_LIBRARIES` target graph before it
registers GoogleTest discovery. Shared and module libraries contribute their
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

Minimal pattern:

```cmake
include(GoogleTest)

add_durin_test(PackageRoundTripTests
    Private/AssetImportTests.cpp
)

target_link_libraries(PackageRoundTripTests PRIVATE
    AssetCore
)

set_target_properties(PackageRoundTripTests PROPERTIES
    DURIN_TEST_CASE_PARALLEL_SAFE TRUE
)

durin_finalize_native_test(PackageRoundTripTests
    KIND feature
    DOMAINS asset-package
    MODULES asset-core
)
durin_discover_tests(PackageRoundTripTests)
```

`durin_finalize_native_test(...)` is the structured declaration boundary. It
must appear after sources, links, runtime policy, and execution properties are
known and before `durin_discover_tests(...)`. `KIND` is exactly one of
`contract`, `feature`, `integration`, `characterization`, `infrastructure`, or
`qualification`; `DOMAINS` contains at least one stable selection slice.
Optional `MODULES`, `BACKENDS`, and `STACKS` aid discovery but never replace
real link, runtime-only dependency, resource-lock, or timeout declarations.

`EXECUTION_HOST` on `durin_finalize_native_test(...)` declares the process
lifecycle required by the target and is independent of kind, direct-lifecycle
registration, labels, and locks. Omit it for the `direct` default; use
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

Complete every target link declaration before calling
`durin_discover_tests(...)`. The discovery call is the runtime-closure
finalization point as well as the test-policy registration point. Configuration
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
before discovery:

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
through `durin_discover_tests(...)`; direct `gtest_discover_tests(...)`
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

Case-level parallel safety remains required for the explicit case diagnostic
mode. If a target cannot use
`DURIN_TEST_CASE_PARALLEL_SAFE TRUE`, set
`DURIN_TEST_TARGET_LOCK_RATIONALE` to a concrete reviewed reason. Broad
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

## Qualified Parallel Baseline

The `windows-msvc-x64` Agent Build Profile qualified the native suite at
14 jobs on 2026-07-28. Three consecutive randomized aggregates completed in
17.85, 17.98, and 18.16 seconds. The same suite took 74.30 seconds at one job
and 39.74 seconds at two jobs. Whole-target direct lifecycle qualification also
passed.

The remaining aggregate critical path includes explicit physical-resource
ownership. Vulkan-backed correctness targets use `durin-gpu` while they own the
physical device, global RHI, or renderer lifecycle. Do not relax that lock
without separating those lifecycles. Move performance-only work into explicit
qualification targets so it does not extend the routine GPU lock chain.

Incremental `all` dependency checks took 0.88-0.96 seconds in the qualification
matrix. Whole-target startup and multi-case execution accounted for 34.36
process-seconds in that historical run; the slowest direct entries were
`CoreUtilityTests` (5.42 seconds), `AssetPackageTests` (4.54 seconds), and
`TextureTests` (4.14 seconds).

Derived closures register dependencies on shared deployment targets. Multiple
tests consuming the same module or external file therefore reuse one writer;
unchanged deployments remain incremental. Two external files with the same
destination filename are rejected during configuration unless they resolve to
the same source file.

## Related Docs

- `Documentation/Agents/Testing.md`
- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/ThirdPartyBootstrap.md`
