# CPU Profiling

Durin provides opt-in Tracy CPU instrumentation through isolated Release
profiling presets. Ordinary Debug, Release, Shipping, setup, and worktree
workflows do not require or link Tracy.

## Preparation And Presets

Prepare the pinned Tracy `v0.13.1` source explicitly:

```powershell
Engine\Scripts\Bootstrap\Setup_tracy.bat
```

The supported profiling presets are:

- `Win64-Release-DurinEditor-Profiling`
- `Win64-Release-DurinGame-Profiling`

Build them through BuildTool:

```powershell
.\BuildTool build --preset Win64-Release-DurinEditor-Profiling --target all
.\BuildTool build --preset Win64-Release-DurinGame-Profiling --target all
```

Both use `CMAKE_BUILD_TYPE=Release`, set `DURIN_PRESET_ROLE=Profiling`, enable
`DURIN_ENABLE_TRACY`, and write binaries beneath
`Engine/Binaries/Win64/Release-Profiling/`. Shipping rejects Tracy during
configuration.

The client is configured for on-demand, localhost-only capture. Capturing is
optional at runtime. Use the profiler or capture tool from the matching upstream
Tracy `v0.13.1` release.

## Instrumentation Surface

Repository C++ call sites include `Profiling/Profiling.h`, not Tracy headers.
The supported operations are:

- `DURIN_PROFILE_CPU_ZONE()`
- `DURIN_PROFILE_CPU_ZONE_NAMED("Stable.Name")`
- `DURIN_PROFILE_FRAME_MARK()`
- `DURIN_PROFILE_THREAD(Name)`

Zone names use bounded, stable strings that describe an owned operation, such as
`EngineLoop.GameLogic` or `QueuedTask.Execute`. Do not include asset paths,
object names, task ids, or other unbounded per-item data in zone names.

When `DURIN_WITH_TRACY=0`, these macros do not require Tracy headers or symbols
and do not evaluate their profiling-only arguments. Tracy types must not appear
in Durin function signatures, reflected declarations, or public data contracts.

## Runtime Ownership

Profiling builds produce one shared `TracyClient.dll` in the selected runtime
variant directory. Every instrumented module links that same process-wide
runtime. Ordinary output directories contain no Tracy runtime.

The initial integration covers CPU frame, thread, task, renderer, and asset
boundaries. GPU, allocation, lock, frame-image, sampling, and call-stack
instrumentation remain outside this workflow.
