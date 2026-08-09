# Native Test Execution Granularity Stage 4 Evidence

This document is the final handoff for
`Documentation/Plans/Archive/2026-08/NativeTestExecutionGranularity.md`.

Baseline commit: `055e05b6`

Profile: `windows-msvc-x64`

Preset: `Win64-Debug-DurinEditor-Tests`

## Final Policy

Every ordinary native-test target now uses target-default execution. The CMake
fallback and DurinDevTool aggregate default are both `target`; explicit `case`
execution remains available for complete process-isolation diagnosis. The
transitional `hybrid` mode selects the same 48 ordinary target registrations,
and `--include-direct` is a no-op compatibility alias in target mode. The only
multi-process registrations outside the ordinary families are the intentional
native-test isolation characterization probes.

## Lifecycle Repairs

- Object-backed targets share one process-scoped DObject initialization helper.
  Core object tests were split into reflection, property-change, and
  property-snapshot execution domains, with exact test-only lifecycle seams
  replacing broad class-default-object release.
- Asset package tests reset the asset manager, garbage collector, writable
  asset root, derived-data cache, mount registry, and injected failure points
  for every test.
- Core concurrency tests reset their bounded attribution registry through a
  private test seam.
- Death-test children use unique nested sandboxes owned by the parent run;
  successful parents remove them and failed parents retain them.
- The default-material missing-content test unloads cached authored data before
  installing its own empty `/Engine/` mount. This isolates both process-global
  service state and filesystem data while remaining valid in case-isolated
  processes that have no default mount.

## Qualification

Three consecutive randomized target-default aggregates passed with 48 CTest
processes:

| Seed | Wall time | Process time | Result |
| --- | ---: | ---: | --- |
| 76087 | 18.74 s | 103.53 s | pass |
| 38151 | 19.16 s | 105.89 s | pass |
| 71638 | 19.14 s | 107.05 s | pass |

A final post-repair randomized target run also passed in 18.56 seconds with
seed 56462. Hybrid passed the same 48 registrations in 19.50 seconds with seed
86339. The complete case aggregate passed all 1,243 registered cases at eight
jobs in 28.06 seconds and retained the expected two skips; CTest also reported
the two disabled cases. A first 18-job case attempt saturated late in the
schedule and outlived the controlling ten-minute diagnostic timeout, so the
complete case qualification used the explicit lower local concurrency rather
than masking or retrying a test failure.

The 48-process target default reduces launches by 96.1 percent from the Stage 0
1,244-process case baseline. Its 19.14-second median and 19.16-second worst
qualified wall times are both below the Stage 0 30.00-second median and
31.44-second worst. Median target process time is 105.89 seconds versus 304.18
seconds for the three Stage 0 qualified case runs.

Focused shuffled qualification covered the repaired object, asset,
concurrency, material-service, renderer, and death-test domains. CMake
configuration and discovery-policy probes passed through the test preset. All
306 DurinDevTool tests passed. Controlled Stage 3 failure evidence continues to
prove target/case attribution, retained sandbox diagnostics, and the printed
case-mode rerun command.

## Handoff

The working set is the native-test execution policy and declarations,
DurinDevTool aggregate selection, shared native-test lifecycle support, the
bounded production test seams used by object and task tests, the affected test
fixtures, and the authoritative build/test documentation. There are no open
questions or temporary ordinary `CASE` exceptions.
