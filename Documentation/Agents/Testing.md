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

Use positional selections. `--target` is deprecated. Whole-target execution is
the default and recommended granularity. Never run an unfiltered aggregate at
case granularity; isolate an aggregate failure with its named target and case.

Treat every test command as long-running and give the execution tool an
explicit timeout of at least 10 minutes. A failed assertion, crash, or timeout
does not require a rebuild; fix or diagnose the cause and rerun the same test
selection.

## Read the Complete Specification

Continue to [Native C++ Tests](../Development/Build/NativeTests.md) only when
the task does one of the following:

- adds, splits, classifies, or registers a test target;
- changes test discovery, selection, CMake metadata, the registry, harness,
  deployment, isolation, resource locks, aggregate behavior, or test CI;
- needs characterization, qualification, stress, report, or case-isolation
  modes;
- requires fixture, sandbox, runtime-dependency, or failure-diagnosis rules not
  covered by a focused rerun.
