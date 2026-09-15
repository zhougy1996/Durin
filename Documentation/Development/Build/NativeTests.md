# Native Test Execution

Summary: Define native-test discovery, selectors, execution granularity, modes, reports, and failure diagnosis.

Last reviewed: 2026-09-15

Routine validation decisions belong to [Agent Testing Workflow](../../Agents/Testing.md).
Target registration, classification, deployment, and resource-lock declarations
belong to [Native Test Authoring](NativeTestAuthoring.md). Hardware, timing, and
memory measurement policy belongs to [Native Test Qualification](NativeTestQualification.md).
Read only the section needed for the current execution or diagnosis.

## Discover and Select

Use the configured registry instead of inferring targets from source directories:

```powershell
.\DevTool.bat test list <query>
.\DevTool.bat test explain "@domain=viewport,backend=vulkan"
.\DevTool.bat test affected --explain
```

`test list private-sources` reports registered production-private source seams.
The former `EngineTests` and `MaterialTests` executables are not runnable targets;
their source directory names do not identify the focused targets that replaced them.
Do not maintain target counts or inventories here; discovery and reports are the
source of truth. DevTool rejects presets without `BUILD_TESTING` enabled.

| Selection | Behavior |
| --- | --- |
| `<Target> [GoogleTestFilter]` | Run a named target, optionally filtering cases |
| `@domain=viewport,backend=vulkan` | Intersect dimensions; `+` unions values within one dimension; `@viewport` abbreviates `@domain=viewport` |
| `affected [--base <git-ref>]` | Resolve ordinary coverage from changed paths; see below |
| `fast-all` | Select contract, feature, and infrastructure targets; exclude integration, characterization, and qualification |
| `all` | Build `DurinNativeTests` and run each ordinary target once through CTest |

Exact target names take precedence over set syntax. Empty sets are errors and
never fall back to `all`. Ordinary selections exclude characterization and
qualification; their admission requires the corresponding explicit mode.
Classification meanings are in [Target Classification](NativeTestAuthoring.md#target-classification).
Native-test executables and GoogleTest are excluded from CMake's default `all`
build target even when `BUILD_TESTING` is enabled.

### Affected Selection

Without `--base`, `affected` unions staged, unstaged, and untracked paths.
With `--base`, it includes tracked changes relative to that Git ref plus untracked
paths. Production module paths map to registry `MODULES`. Declared test `.cpp`
files map directly to their owning targets through the registry `sources` field;
selection does not guess target names or domains from filenames. Characterization
and qualification targets remain excluded, including when their sources change.
Documentation-only or unrelated tooling changes may select no native tests.
Execution uses one build and one parallel whole-target CTest selection.
`--explain` prints input paths and decisions without building or running.

Project test roots declared in `.dproject` carry explicit ownership. Test headers,
data, CMake changes, and unrecognized/new/deleted test sources select the project's
ordinary targets. Unknown test paths without a project owner select `all`, even
when other changed paths have bounded coverage. A production-private `.cpp`
compiled directly into tests selects those exact consumers and still contributes
its production module coverage. Generated harness and environment sources are
not recorded, and this map is not a production header dependency graph.

Before execution, DevTool compares declarations, CMake contents, and test-file
membership with the configured registry fingerprint, reconfiguring and resolving
again when needed. Assertion-only edits do not require this refresh. `--explain`
remains read-only and warns when the registry is stale. After a registry schema
upgrade, run configure to regenerate it before selection.

Shared discovery, registry, harness, execution, workspace membership, project
descriptor, or unbounded CMake changes resolve conservatively to `all`. Runtime
inputs outside modules known to the registry likewise resolve to `all`.

## Execution and Reports

```powershell
.\DevTool.bat test CoreUtilityTests FJsonDocumentTests.ParseObjectFromString
.\DevTool.bat test "@viewport" --parallel 4 --report
.\DevTool.bat test "@viewport" --mode stress
.\DevTool.bat test "@kind=characterization,domain=launch" --mode characterization
.\DevTool.bat test "@kind=qualification,domain=renderer" --mode qualification
```

| Execution | Process and scheduling contract |
| --- | --- |
| Direct-hosted exact target | One executable; its cases run sequentially |
| Application-hosted exact target | Whole-target CTest registration preserves the platform launcher |
| Ordinary set, `affected`, `fast-all`, or `all` | CTest schedules whole targets; cases within each remain sequential |
| `<Target>` or `@set` with `--parallel [N]` | CTest isolates each case in a separate process; N limits concurrent cases; `--parallel 1` isolates serially |

Default build and CTest concurrency use `build.parallelJobs`, or automatic
parallelism when unset. `--parallel N` changes only case concurrency; omission of
N uses the build-job limit. `test` does not accept `--jobs`. Resource locks remain
authoritative; their scope and declaration rules are in
[Native Test Authoring](NativeTestAuthoring.md#add-a-test-target).
Whole-target execution detects shared-state cleanup failures; isolated execution
can expose missing per-case setup.

A positional GoogleTest filter supports `*`, `?`, colon-separated alternatives,
and exclusions after `-`. Isolation accepts an optional filter and requires a
named target or `@set`. `all` does not accept an executable-specific positional
filter. `--parallel` combines with `--report`, but not another execution mode.
Routine runs omit `--mode`; case isolation uses `--parallel`.
Stress randomizes CTest scheduling and GoogleTest order and prints a reproducible
shuffle seed, forwarded to GoogleTest and reproducible with `GTEST_RANDOM_SEED`.
Characterization uses its owning custom runner rather than a routine direct
whole-executable lifecycle.

The default execution timeout is 300 seconds, starting after the build.
`--timeout <seconds>` changes it; `--timeout 0` disables it for a deliberate
long diagnostic run. For CTest selections, the limit applies to each registration.
`--report` writes `Build/NativeTestResults/<Preset>/<Selection>.xml`;
`--report <path>` overrides the destination. Direct runs produce GoogleTest XML;
CTest runs produce JUnit XML. Reporting does not change execution granularity.

Scheduled/nightly validation owns the ordinary aggregate. Release qualification
adds explicit qualification and its required platform/backend matrix. Local
handoffs follow the risk and result-reuse rules in
[Agent Testing Workflow](../../Agents/Testing.md#select-validation), without
inheriting the scheduled matrix. Preserve resolved target names in the handoff
or CI log.

## Failure Diagnosis

Rerun the printed failing target with a narrow case filter; use `--parallel 1`
when process isolation is needed. Failed assertions, crashes, timeouts, or test
interruptions do not require `rebuild`: DevTool clears build recovery state before
test execution. Build failures and lost build-process ownership follow
[Build And Run](BuildAndRun.md).

On Windows, direct diagnostic executables reside under
`Engine/Binaries/Win64/Debug/Tests/DurinEditor/Bin/`; CTest state is under
`Build/Win64-Debug-DurinEditor`. Adapt these paths to the selected profile.
Direct and filtered runs use the same sandbox harness as CTest.
`--durin-keep-test-work` or `DURIN_TEST_KEEP_WORK=1` retains successful work;
failures print their retained sandbox. Output and cleanup contracts are in
[Output Layout](NativeTestAuthoring.md#output-layout).
Native crash fixtures and supported fault coverage belong to
[Native Crash Diagnostics](../../Runtime/Core/NativeCrashDiagnostics.md).

## Application-Hosted Tests

Application validation is opt-in only when requested by the user, selected plan
gate, or active CI job. The default macOS editor preset sets
`DURIN_ENABLE_APPLICATION_TESTS=OFF`, omitting LaunchServices tests and their Host
and Controller infrastructure from its registry and build graph.

Use `configure -DDURIN_ENABLE_APPLICATION_TESTS=ON` through the host launcher in
the selected checkout, then `test NativeTestApplicationExecutionTests`.
`-DNAME=VALUE` or `--define NAME=VALUE` is repeatable. Ordinary `configure` restores
the preset's explicit `OFF` default. An external-volume checkout requires the
interactive LaunchServices permission already granted; an internal-volume main
checkout is preferred for unattended validation.

On macOS, always use DevTool's target, filter, isolation, stress, report, or
qualification path; direct execution bypasses LaunchServices admission. Inspect
the retained control directory printed on launcher failure. A locked or missing
graphical login session is a bounded failure, not a skip. If the sandbox or session
cannot use LaunchServices, compilation may be checked but report execution as not
run. Do not escape the sandbox, change macOS authorization, relocate artifacts,
or use the product application to satisfy optional coverage.

## Material Test Selection

Use `test list material` for focused targets and
`test "@domain=material,kind=feature" --report` for material feature regression.
The feature intersection excludes Vulkan integration and qualification.
`@material` also includes thumbnail Vulkan integration; shader resource reload
coverage belongs to `RendererResourceReloadVulkanTests`.
CPU material tests and thumbnail resource-lifetime integration do not establish
final material pixel correctness; the former screenshot/pixel comparisons were
removed. For material timing and payload measurements, use
[performance qualification](NativeTestQualification.md#performance-qualification-and-concurrent-agents).
