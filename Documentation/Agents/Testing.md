# Agent Testing Workflow

Read this short guide before selecting or running native tests. Its purpose is
to obtain sufficient confidence with the smallest relevant test scope.

## Select Validation

For simple, low-risk changes such as text, formatting, or missing enum cases
that follow an established mapping, review the diff and compile the smallest
relevant target when needed. These changes do not require `test affected` or a
full build by default. Judge semantic risk rather than diff size: changes to
control flow, mutation, persistence, ownership, concurrency, or other behavior
without established coverage need focused tests. Explicit acceptance gates
still apply. Report the validation performed and any intentionally omitted tests.

Choose the smallest sufficient scope; these are alternatives, not a sequence:

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

`test affected` is the default handoff validation when runtime tests are needed.
It maps the current staged, unstaged, and untracked paths to configured native
test modules and domains, prints the exact selection, then builds the targets
once and runs them in one parallel CTest invocation. Pass `--base <git-ref>` to
analyze every change relative to a branch or commit. Pass `--explain` to inspect
the changed paths and decision without building or running.

When the target name is not already known, query the configured test registry
instead of inferring it from the source tree:

```powershell
.\DevTool.bat test list <query>
.\DevTool.bat test explain <Target>
```

1. During implementation, iterate with the smallest affected named target or
   failing case.
2. Before handoff, confirm sufficient coverage of the final code state. Reuse
   passing results when the tested code, relevant inputs, and environment are
   unchanged and the recorded selection covers the task's behavior and gates.
   Otherwise use `test affected` or an explicit acceptance selection. If the
   working tree includes unrelated changes, inspect `test affected --explain`
   and choose a task-specific bounded registry set when its coverage is clear;
   report that selection. Batch whole targets in one invocation rather than
   assembling coverage through separate commands. Focused iteration and
   diagnosis may use separate runs.
3. Use a bounded domain or domain/backend set when behavior crosses test
   targets.
4. Use `fast-all` for broad non-integration feedback. It includes `contract`,
   `feature`, and `infrastructure`, but excludes `integration`,
   `characterization`, and `qualification`.
5. If integration behavior changed, run its exact target or matching bounded
   set even after `fast-all` passes.
6. Run `test all` only for an explicit gate, a change to shared runtime or test
   infrastructure, or concrete evidence that bounded validation is
   insufficient. State the reason before starting it.

Application-hosted tests are never implicit validation. Leave
`DURIN_ENABLE_APPLICATION_TESTS` off unless the user, selected plan gate, or
active CI job requires that coverage. When required, first read
[application-host execution](../Development/Build/NativeTests.md#application-hosted-tests).

GPU qualification is not implicit validation for CPU changes or migrated test
fixtures. `test affected` already excludes qualification targets. Do not append
GPU qualification solely because a changed fixture belongs to such a target;
build that target when compile coverage is needed. Run GPU qualification only
when the changed GPU behavior, an explicit user request, a selected acceptance
gate, or the active CI job requires it.

If the current session is known to lack GPU access, do not retry GPU execution
without evidence that access changed. Report it as unavailable/not run, retain
any prior failure diagnostic, and continue the supported validation. Optional
GPU coverage must not become a new completion or downstream-plan gate. Explicit
GPU acceptance gates remain outstanding until validated in a capable environment.
Keep GPU tests registered for those environments; do not turn initialization
failures into unconditional passes.

Before macOS GPU execution, read
[GPU environment guidance](../Development/Build/NativeTests.md#gpu-qualification-environments).
Before timing qualification, read
[performance qualification](../Development/Build/NativeTests.md#performance-qualification-and-concurrent-agents).
Timing acceptance requires an exclusive quiet GPU lane;
correctness runs may proceed under the ordinary build ownership rules.

Use positional selections. Whole-target execution is the default. Isolate an
aggregate failure with its named target and case.

Treat test execution and any transitive build as long-running; follow the
timeout and continuation rules in [Build And Run](BuildAndRun.md). Read-only
discovery commands such as `test list`, `test explain`, and
`test affected --explain` do not need a long-running execution budget. A failed assertion,
crash, or timeout does not require a rebuild; fix or diagnose the cause and
rerun the same test selection.

## Read the Complete Specifications

Continue to [Native Test Execution](../Development/Build/NativeTests.md) when
the task changes discovery, selection, registry consumption, harness execution,
aggregate behavior, test CI, application hosting, or characterization,
qualification, stress, report, and case-isolation modes. Use it also for
failure diagnosis beyond a focused rerun.

Continue to
[Native Test Authoring](../Development/Build/NativeTestAuthoring.md) when the
task adds, splits, classifies, or registers a test target, or changes target
metadata, source ownership, fixtures, sandboxes, runtime dependencies,
deployment, capability guards, lifecycle isolation, or resource locks.
