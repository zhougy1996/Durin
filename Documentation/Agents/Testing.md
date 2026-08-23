# Agent Testing Workflow

Read this short guide before selecting or running native tests. Its purpose is
to obtain sufficient confidence with the smallest relevant test scope.

## Select Validation

Use the first scope that covers the changed behavior:

```powershell
.\DevTool.bat test <Target> [Suite.Case]
.\DevTool.bat test "@<domain>"
.\DevTool.bat test "@domain=<domain>,backend=<backend>"
.\DevTool.bat test fast-all
.\DevTool.bat test all
```

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

1. Start with the smallest affected named target or failing case.
2. Use a bounded domain or domain/backend set only when behavior crosses test
   targets.
3. Use `fast-all` for broad non-integration feedback. It includes `contract`,
   `feature`, and `infrastructure`, but excludes `integration`,
   `characterization`, and `qualification`.
4. If integration behavior changed, run its exact target or matching bounded
   set even after `fast-all` passes.
5. Run `test all` only for an explicit gate, a change to shared runtime or test
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

## Performance Qualification and Concurrent Agents

Ordinary correctness builds and tests may run while other agents are active,
subject to the repository's single-writer and no-overlapping-build rules. GPU
timing qualification is different: results are authoritative only from an
exclusive quiet GPU lane with no competing agent test, editor, browser workload,
capture tool, or other GPU application. A machine reboot is not required when
the qualification supplies its documented warm-up.

The `durin-gpu` resource lock serializes registered tests within one CTest
scheduler. It does not coordinate independent DevTool/CTest invocations,
separate worktrees, agents, or external applications. When any of those may be
competing, run correctness coverage normally but label timing output diagnostic
only: do not rebaseline a threshold, accept a performance gate, or claim a
regression from it. Rerun the exact qualification selection in a quiet window;
prefer consecutive passes and report the warm-up/sample count and median/p95.
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
