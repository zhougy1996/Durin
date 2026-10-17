# Agent Testing Workflow

## Select Validation

Choose validation by semantic risk. Low-risk text or mechanical edits need diff
review and compilation when useful; behavior changes need relevant test coverage.
Honor explicit acceptance gates and report validation performed or omitted.
Documentation-only changes need document validation and diff review, without
native builds or tests unless executable behavior or an explicit gate requires them.

Generate Cook packages, manifests, state and DDC records in test fixture directories
under ignored `Build/`; do not copy generated output snapshots into versioned test
data. Prefer runtime-generated inputs and focused codec assertions. Historical
qualification snapshots belong in local ignored storage or CI artifacts.

Use the smallest sufficient selection. These are alternatives; inspect affected
coverage before executing it:

```powershell
.\DevTool.bat test affected --explain
.\DevTool.bat test affected --base <git-ref> --explain
.\DevTool.bat test affected
.\DevTool.bat test <Target> [Suite.Case]
.\DevTool.bat test "@<domain>"
.\DevTool.bat test "@domain=<domain>,backend=<backend>"
```

## Choose Execution

`test <Target>` runs its cases sequentially in one process for direct-hosted
native tests. It can expose shared-state cleanup failures, but can also mask
missing per-case setup. A case must pass when run alone.

```powershell
.\DevTool.bat test MaterialCompilerTests --isolate --test-jobs 4
.\DevTool.bat test MaterialCompilerTests FMaterialExpressionTests.* --isolate --test-jobs 1
.\DevTool.bat test affected --test-jobs 4 --report
```

`--isolate` runs each selected case in a separate process through CTest. It
accepts a named target or `@set` in routine mode. Use `--test-jobs 1` for serial
isolation, or begin with 4 slots for bounded CPU correctness feedback after
checking isolation. Without `--isolate`, scheduling does not split target cases.

`--test-jobs N` controls CTest scheduling slots for whole-target or isolated
runs; omitting it uses configured concurrency. Direct execution of one target
still uses one process. Builds use configured concurrency independently; `test`
does not accept `--jobs`.

`--report [path]` saves XML without changing execution, including for `affected`.
The default is `Build/NativeTestResults/<Preset>/<Selection>.xml`. Direct runs
produce GoogleTest XML; CTest runs produce JUnit XML. Empty affected selections
write no report and leave any previous report unchanged; check the command log.
Use the positional case filter and `--isolate [--test-jobs N]` for isolated
execution; routine execution needs no `--mode`.

Existing CTest resource locks and execution-host rules still apply; parallelism does not authorize GPU
or application-hosted coverage. A failure that also occurs when run alone is an
isolation/setup issue, not evidence of a concurrency conflict.

`affected`, `fast-all`, and ordinary `@set` runs instead schedule whole test
targets through CTest. Their cases remain sequential inside each process.
Configured concurrency is the default for builds and whole-target CTest scheduling;
it does not parallelize cases for ordinary `test <Target>`.

`test affected` defaults to staged, unstaged, and untracked changes. Use `--base`
for changes relative to a Git ref. When runtime tests are needed, first inspect
`affected --explain`; execute it when the resolved coverage fits the task.
Selection uses module labels and declared test sources, not a production header
or function dependency graph. Test headers, data, and new or unknown test sources
can select a whole project's tests; shared infrastructure and unbounded build
changes can select all ordinary tests. Inspect these expansion reasons before
execution. Use a named target or registry set instead only when its coverage of
the changed behavior and consumers is justified; report the reason and remaining
gates. Keep broad coverage when the impact cannot be bounded reliably.

When the target name is not already known, query the configured test registry
instead of inferring it from the source tree:

```powershell
.\DevTool.bat test list <query>
.\DevTool.bat test <Target> --explain
```

- Iterate and diagnose with a named target or case. Batch whole-target coverage
  through `affected` or one domain/backend set.
- Reuse passing results for the final code when relevant inputs and environment
  are unchanged and recorded coverage satisfies the task and gates. Record the
  tested revision, selection, configuration, and environment; confirm that later
  changes leave relevant source, fixtures, generated/package inputs, toolchain,
  and runtime settings unchanged. A new commit identifier or documentation-only
  edit does not itself invalidate runtime evidence. Rerun affected coverage when
  relevant inputs change; explicit clean-build, package, backend, hardware, and
  timing conditions still apply to any reused receipt. With
  unrelated working-tree changes, inspect `affected --explain` and use a
  task-specific registry set when its coverage is clear; report the selection.
- `fast-all` covers contract, feature, and infrastructure tests; it excludes
  integration, characterization, and qualification. Changed integration behavior
  still needs its target or bounded set.
- Use `test fast-all` for justified broad coverage, not as a routine follow-up
  to passing focused tests. Use `test all` for an explicit gate or when shared
  runtime/test infrastructure changes or other evidence make bounded coverage
  insufficient; state the uncovered risk. A shared-file edit alone is not a
  reason to run every native test; tool-only changes may need focused Python
  tests and a representative native invocation instead.

Application-hosted tests are never implicit validation. Leave
`DURIN_ENABLE_APPLICATION_TESTS` off unless the user, selected plan gate, or
active CI job requires that coverage. When required, first read
[application-host execution](../Development/Build/NativeTests.md#application-hosted-tests).

Run GPU qualification only for changed GPU behavior or explicit user, plan, or
CI requirements. `affected` excludes it; CPU-only fixture changes may need
compilation of the qualification target, but not GPU execution.
Ordinary GPU integration targets can still appear in affected selections;
qualification exclusion does not make the selection CPU-only. Apply the same
coverage reasoning before executing those targets and preserve explicit GPU gates.

If GPU access is unavailable, retain diagnostics, report execution as not run,
and retry only when access changes. Optional coverage cannot block completion or
downstream plans; explicit GPU gates remain outstanding. Keep tests registered
and never convert initialization failures into passes.

Before macOS GPU execution, read
[GPU environment guidance](../Development/Build/NativeTestQualification.md#gpu-qualification-environments).
Before timing qualification, read
[performance qualification](../Development/Build/NativeTestQualification.md#performance-qualification-and-concurrent-agents).
Timing acceptance requires an exclusive quiet GPU lane;
correctness runs may proceed under the ordinary build ownership rules.

Use positional selections; whole-target execution is the default. Test execution
and transitive builds follow [Build And Run](BuildAndRun.md) timeout and recovery
rules. Read-only discovery does not need a long-running execution budget.

## Read the Complete Specifications

- [Native Test Execution](../Development/Build/NativeTests.md): execution
  mechanisms, advanced modes, CI, or diagnosis beyond a focused rerun.
- [Native Test Authoring](../Development/Build/NativeTestAuthoring.md): target
  registration, fixtures, dependencies, or isolation and resource ownership.
