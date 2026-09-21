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

## Parameter Contract

DevTool owns selection and execution policy. A positional case filter uses
GoogleTest syntax; CTest owns scheduling of the registrations DevTool selects.
CMake declarations own `RESOURCE_LOCKS`, `PROCESSORS`, and per-registration
`TIMEOUT`; these are not additional DevTool CLI flags. DevTool does not pass
arbitrary CTest arguments through to the scheduler.

| Parameter | Scope and meaning |
| --- | --- |
| `<selection>` | Named target, `@set`, `affected`, `fast-all`, or `all` |
| `[GoogleTestFilter]` | Exact target; `@set` only with `--isolate`; never aggregate or affected |
| `--isolate` | Separate case processes for a named target or `@set`; routine mode only |
| `--test-jobs N` | CTest scheduling slots, 1–256; independent of isolation and build concurrency |
| `--mode stress` | Randomized CTest scheduling and GoogleTest order for ordinary explicit selections |
| `--mode characterization` / `qualification` | Explicit target or `@set` admission; unavailable with `affected`, `all`, or `--isolate` |
| `--report [PATH]` | All executing selections, including `affected`; format follows execution path |
| `--timeout SECONDS` | Execution timeout policy described below; does not limit the build |
| `--base REF` / `--explain` | Only `affected`; explanation performs no execution |

Discovery (`list`, `explain`, and `affected --explain`) rejects `--report`,
`--test-jobs`, and explicit `--timeout`. `affected` keeps ordinary whole-target execution and accepts no
`--mode`, `--isolate`, or case filter. Report, scheduling, and timeout options
combine with each supported execution mode.

Migration: replace `--parallel` with `--isolate`, and `--parallel N` with
`--isolate --test-jobs N`. The old flag is rejected rather than silently changing
from process isolation to scheduling alone. The removed `--filter`, `--mode report`,
and `--mode isolation` forms remain unsupported.

## Execution and Reports

```powershell
.\DevTool.bat test CoreUtilityTests FJsonDocumentTests.ParseObjectFromString
.\DevTool.bat test "@viewport" --isolate --test-jobs 4 --report
.\DevTool.bat test "@viewport" --mode stress
.\DevTool.bat test "@kind=characterization,domain=launch" --mode characterization
.\DevTool.bat test "@kind=qualification,domain=renderer" --mode qualification
```

| Execution | Process and scheduling contract |
| --- | --- |
| Direct-hosted exact target | One executable; its cases run sequentially |
| Application-hosted exact target | Whole-target CTest registration preserves the platform launcher |
| Ordinary set, `affected`, `fast-all`, or `all` | CTest schedules whole targets; cases within each remain sequential |
| `<Target>` or `@set` with `--isolate` | CTest isolates each case in a separate process; `--test-jobs 1` isolates serially |

Default build and CTest concurrency use `build.parallelJobs`, or automatic
parallelism when unset. `--test-jobs N` overrides only CTest scheduling slots,
for whole-target and isolated runs alike. It does not change execution granularity
or build concurrency. A direct exact-target run remains one process. `test` does
not accept the build command's `--jobs` option. Resource locks remain
authoritative; their scope and declaration rules are in
[Native Test Authoring](NativeTestAuthoring.md#add-a-test-target).
Whole-target execution detects shared-state cleanup failures; isolated execution
can expose missing per-case setup.

A positional GoogleTest filter supports `*`, `?`, colon-separated alternatives,
and exclusions after `-`. Isolation accepts an optional filter and requires a
named target or `@set`. `all` does not accept an executable-specific positional
filter. `--isolate` combines with `--test-jobs` and `--report`, but not `--mode`.
Routine runs omit `--mode`; case isolation uses `--isolate`.
Stress randomizes CTest scheduling and GoogleTest order and prints a reproducible
shuffle seed, forwarded to GoogleTest and reproducible with `GTEST_RANDOM_SEED`.
Characterization uses its owning custom runner rather than a routine direct
whole-executable lifecycle.

`--timeout <seconds>` defaults to 300 and applies after the build. For direct
runs it limits the executable process; `--timeout 0` removes that limit.
For CTest runs it sets the scheduler's default timeout, not a total batch budget.
A registration's CMake `TIMEOUT` property takes precedence; repository tests
declare that property. `--timeout 0` omits the CLI default and does not disable
registered timeouts. Change the owning declaration for a longer CTest diagnostic
run; do not assume the CLI overrides it.
`--report` writes `Build/NativeTestResults/<Preset>/<Selection>.xml`;
`--report <path>` overrides the destination. Direct runs produce GoogleTest XML;
CTest runs produce JUnit XML. Reporting does not change execution granularity.
`affected --report` uses `affected.xml` even when selection expands to `all`.
If no tests are selected, no report is written and any previous file is unchanged;
the command log explicitly reports this. Do not interpret an older XML as this run.

Scheduled/nightly validation owns the ordinary aggregate. Release qualification
adds explicit qualification and its required platform/backend matrix. Local
handoffs follow the risk and result-reuse rules in
[Agent Testing Workflow](../../Agents/Testing.md#select-validation), without
inheriting the scheduled matrix. Preserve resolved target names in the handoff
or CI log.

## Failure Diagnosis

Rerun the printed failing target with a narrow case filter; use `--isolate --test-jobs 1`
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

The internal LaunchServices host keeps binaries and dependencies under the
owning test's output root. Admission, test, crash, timeout, cancellation, and
cleanup failures retain bounded evidence in `Work/ApplicationHost` and print
its path. Do not assemble a bundle or invoke `open` manually; product packaging,
signing, and installation are separate workflows.

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
