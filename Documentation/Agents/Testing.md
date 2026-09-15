# Agent Testing Workflow

## Select Validation

Choose validation by semantic risk. Low-risk text or mechanical edits need diff
review and compilation when useful; behavior changes need relevant test coverage.
Honor explicit acceptance gates and report validation performed or omitted.

Use the smallest sufficient selection:

```powershell
.\DevTool.bat test affected
.\DevTool.bat test affected --base <git-ref>
.\DevTool.bat test affected --explain
.\DevTool.bat test <Target> [Suite.Case]
.\DevTool.bat test "@<domain>"
.\DevTool.bat test "@domain=<domain>,backend=<backend>"
.\DevTool.bat test fast-all
.\DevTool.bat test all
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
Use the positional case filter. The former `--parallel [N]` is replaced by
`--isolate [--test-jobs N]`; routine execution needs no `--mode`.

Existing CTest resource locks and execution-host rules still apply; parallelism does not authorize GPU
or application-hosted coverage. A failure that also occurs when run alone is an
isolation/setup issue, not evidence of a concurrency conflict.

`affected`, `fast-all`, and ordinary `@set` runs instead schedule whole test
targets through CTest. Their cases remain sequential inside each process.
Configured concurrency is the default for builds and whole-target CTest scheduling;
it does not parallelize cases for ordinary `test <Target>`.

`test affected` defaults to staged, unstaged, and untracked changes. Use `--base`
for changes relative to a Git ref, or `--explain` to inspect selection without
execution. It is the default handoff selection when runtime tests are needed.

When the target name is not already known, query the configured test registry
instead of inferring it from the source tree:

```powershell
.\DevTool.bat test list <query>
.\DevTool.bat test explain <Target>
```

- Iterate and diagnose with a named target or case. Batch whole-target coverage
  through `affected` or one domain/backend set.
- Reuse passing results for the final code when relevant inputs and environment
  are unchanged and recorded coverage satisfies the task and gates. With
  unrelated working-tree changes, inspect `affected --explain` and use a
  task-specific registry set when its coverage is clear; report the selection.
- `fast-all` covers contract, feature, and infrastructure tests; it excludes
  integration, characterization, and qualification. Changed integration behavior
  still needs its target or bounded set.
- Use `test all` only for an explicit gate, shared runtime/test infrastructure
  changes, or evidence that bounded coverage is insufficient; state the reason.

Application-hosted tests are never implicit validation. Leave
`DURIN_ENABLE_APPLICATION_TESTS` off unless the user, selected plan gate, or
active CI job requires that coverage. When required, first read
[application-host execution](../Development/Build/NativeTests.md#application-hosted-tests).

Run GPU qualification only for changed GPU behavior or explicit user, plan, or
CI requirements. `affected` excludes it; CPU-only fixture changes may need
compilation of the qualification target, but not GPU execution.

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
