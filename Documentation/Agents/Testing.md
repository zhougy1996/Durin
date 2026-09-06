# Agent Testing Workflow

Read this short guide before selecting or running native tests. Its purpose is
to obtain sufficient confidence with the smallest relevant test scope.

## Select Validation

Use the first scope that covers the changed behavior:

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

`test affected` is the default handoff validation for an ordinary code change.
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

For example, the former monolithic `EngineTests` executable was intentionally
split into cohesive functional and lifecycle targets. Its
`Engine/Tests/Native/EngineTests` source directory remains, but `EngineTests`
is not a runnable selection and must not be inferred or restored from that
directory name. A test beneath it may belong to a focused target such as
`ViewportTests`; use `test list viewport` to discover that target before
running it, or select a registered domain when the behavior crosses targets.

1. During implementation, iterate with the smallest affected named target or
   failing case.
2. Before handoff, run `test affected` once unless an explicit acceptance gate
   names a different selection. Do not execute two or more targets through
   separate commands to assemble coverage; use `affected` or one bounded set so
   the build and CTest scheduler can batch them.
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

Application-hosted tests are never implicit validation. Unless the user, a
selected plan acceptance gate, or the active CI job explicitly requires
application-host coverage, leave `DURIN_ENABLE_APPLICATION_TESTS` off and do
not run those targets. If that coverage is required but the current sandbox or
session cannot use LaunchServices, validate configuration or compilation when
useful and report application execution as not run. Do not leave the current
sandbox, change machine authorization, relocate artifacts, or substitute the
product application merely to make this optional lane pass.

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

## Performance Qualification and Concurrent Agents

Ordinary correctness builds and tests may run while other agents are active,
subject to the repository's single-writer and no-overlapping-build rules. GPU
timing qualification is different: results are authoritative only from an
exclusive quiet GPU lane with no competing agent test, editor, browser workload,
capture tool, or other GPU application. A machine reboot is not required when
the qualification supplies its documented warm-up.

The `durin-gpu` resource lock serializes physical GPU owners within one CTest
scheduler. `durin-rhi-lifecycle` separately serializes real backend startup,
shutdown, and module replacement while allowing CPU-only tests to overlap.
Neither lock coordinates independent DevTool/CTest invocations, separate
worktrees, agents, or external applications. When any of those may be competing,
run correctness coverage normally but label timing output diagnostic only: do
not rebaseline a threshold, accept a performance gate, or claim a regression
from it. Rerun the exact qualification selection in a quiet window; prefer
consecutive passes and report the warm-up/sample count and median/p95.
Statistical stability checks can reject bursty contention, but stable sustained
contention is indistinguishable from a code regression without exclusive
execution.

Use positional selections. Whole-target execution is the default. Isolate an
aggregate failure with its named target and case.

Treat every test command as long-running and give the execution tool an
explicit timeout of at least 10 minutes. A failed assertion, crash, or timeout
does not require a rebuild; fix or diagnose the cause and rerun the same test
selection.

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
