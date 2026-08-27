# Native Test Execution

Last reviewed: 2026-08-20

This is the complete native-test selection, execution, diagnosis, and
infrastructure specification. Agents selecting routine task validation should
first use the short [Agent Testing Workflow](../../Agents/Testing.md). Test
authors should use [Native Test Authoring](NativeTestAuthoring.md).

Native tests are available from every registered build preset; the default is
`Win64-Debug-DurinEditor`.

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

Kind selectors use the behavior classifications defined by
[Native Test Authoring](NativeTestAuthoring.md#target-classification).

Scheduled/nightly repository validation owns the ordinary native aggregate.
Release qualification owns that aggregate, the explicit qualification set,
and any platform/backend matrix required by the release. A failed set is
diagnosed with its printed named targets and narrow case filters; local
implementation and handoff validation remain risk-based and do not inherit the
whole scheduled matrix.

`test all` builds the `DurinNativeTests` aggregate and then runs every
ordinary target once through CTest. Characterization and qualification targets
are neither built nor run by this aggregate. Diagnose an ordinary aggregate
failure with `test <FailedTarget> <Suite.Case>`, or isolate a bounded target set
with a case filter and `--mode isolation`. Native-test executables and
GoogleTest are excluded from CMake's
default `all` target, so routine `build` and `rebuild` commands do not compile
tests even when the selected preset enables `BUILD_TESTING`.
Its timeout applies to each CTest-registered test. GoogleTest `--filter` syntax
is executable-specific and therefore cannot be combined with `test all`.
Stress mode prints and forwards a GoogleTest shuffle seed so order failures can
be reproduced with `GTEST_RANDOM_SEED`. Report mode writes the CTest JUnit
result described above.

Use a focused `test <Target> <GoogleTestFilter>` command for the
fastest failing-case iteration. It launches one target process with the filter;
aggregate execution does not change focused execution.

`native-test-characterization` is always excluded from the aggregate and runs
only through its owning custom target. `KIND characterization` automatically
suppresses the direct whole-executable lifecycle because its custom runner,
rather than a routine smoke, owns the required environment and scheduling.

Do not record a current test or registration total in repository documentation.
CTest discovery is the source of truth; use the command summary or
`--mode report` when a review needs an auditable count.

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

## Qualified Parallel Baseline

The `windows-msvc-x64` Agent Build Profile qualified the native suite at
14 jobs on 2026-07-28. Three consecutive randomized aggregates completed in
17.85, 17.98, and 18.16 seconds. The same suite took 74.30 seconds at one job
and 39.74 seconds at two jobs. Whole-target direct lifecycle qualification also
passed.

The remaining aggregate critical path includes explicit physical-resource
ownership. Vulkan-backed correctness targets use `durin-gpu` while they own the
physical device and `durin-rhi-lifecycle` while they own real backend startup,
shutdown, or module replacement. CPU-only tests may overlap both locks. Do not
relax either lock without separating its lifecycle. Move performance-only work
into explicit qualification targets so it does not extend the routine GPU lock
chain.

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

- [Agent Testing Workflow](../../Agents/Testing.md)
- [Native Test Authoring](NativeTestAuthoring.md)
- [Build And Run](BuildAndRun.md)
- [Third-Party Bootstrap](ThirdPartyBootstrap.md)
