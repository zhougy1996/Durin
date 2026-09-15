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
.\DevTool.bat test MaterialCompilerTests --parallel
.\DevTool.bat test MaterialCompilerTests --parallel 4
.\DevTool.bat test MaterialCompilerTests FMaterialExpressionTests.* --parallel 4
```

`--parallel [N]` runs each selected case in a separate process through CTest,
with at most N concurrent cases. Omit N to use the build-job limit; use
`--parallel 1` for serial isolation. Use it for faster bounded CPU correctness
feedback after checking isolation; begin with 4 workers. It accepts a named
target or `@set`, and needs no wildcard when selecting all its cases.
Use `--report` to save an XML result under the preset's
`Build/NativeTestResults` directory, or `--report <path>` to choose its location.
It works with serial and parallel runs without changing execution.
Use the positional case filter; the redundant `--filter` and `--mode report`
forms have been removed. Routine execution needs no `--mode`; case isolation
uses `--parallel [N]` instead of `--mode isolation`.

Test builds use the configured concurrency; `test` does not accept `--jobs`.
Existing CTest resource locks and execution-host rules still apply; parallelism does not authorize GPU
or application-hosted coverage. A failure that also occurs when run alone is an
isolation/setup issue, not evidence of a concurrency conflict.

`affected`, `fast-all`, and ordinary `@set` runs instead schedule whole test
targets through CTest. Their cases remain sequential inside each process.
The configured concurrency controls builds and whole-target CTest scheduling;
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
