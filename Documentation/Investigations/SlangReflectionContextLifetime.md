# Slang Reflection Context Lifetime

**Status:** Open — original reflection fault is not yet deterministically reproduced

**Last reviewed:** 2026-10-18

## Scope And Verdict

A captured macOS 27.0.1 crash with Slang 2026.5.2 faults in
`Slang::_calcResourceBindingType` while `getBindingRangeCount()` lazily expands
resource reflection. The public Slang program and local session are still in
the compiler call's lexical scope. A project-side session-pool identity defect
was reproduced separately and repaired, but its causal relationship to this
specific crash remains unproven. Do not treat a passing rerun as resolution.

The [shader cache lifecycle contract](../Runtime/Rendering/ShaderCache.md#concurrency-and-lifecycle)
owns session leasing. The separate dyld startup failure is addressed through
[explicit profiler process lifetime](../Development/Build/Profiling.md#runtime-ownership).

## Verified Findings

- **P1: original reflection fault remains unattributed.** The faulting SDK
  instruction reads through a null nested context pointer; this is not sufficient
  evidence of either a dangling application-owned reflection pointer or a Slang
  implementation defect. The application retains the composed program and local
  session throughout reflection in
  [SlangShaderCompiler.cpp](../../Engine/Source/Developer/ShaderBuild/Private/SlangShaderCompiler.cpp).
- **P2: recycled thread identifiers violated pool affinity.** Two joined,
  sequential threads received the same global session under the old
  `std::thread::id` key. A retained per-thread lifetime token now prevents this
  reuse in [SlangGlobalSessionPool.h](../../Engine/Source/Developer/ShaderBuild/Private/SlangGlobalSessionPool.h).
  The pre-fix `PooledSessionsDoNotCrossThreadLifetimes` regression failed on the
  captured host. This fixes the pool contract without claiming to explain the
  original SDK fault.

A controlled comparison retained the profiler startup repair while temporarily
using the old pool key. Sixteen sequential short-lived compilation workers still
completed successfully. Recycled identifiers alone are therefore not a sufficient
reproducer for the original reflection crash with that shader input.

## Reproduction And Validation Gaps

[ShaderReflectionTests.cpp](../../Engine/Tests/Native/RenderCoreTests/Private/ShaderReflectionTests.cpp)
contains focused thread-lifetime and concurrent reflection/resolver-destruction
coverage. The latter performs 96 compilations across six threads and more active
threads than the idle-pool capacity. Run these through
`DevTool test RenderShaderContractTests FShaderReflectionTests.PooledSessions*`.

Preserve complete process crash reports and exact matching SDK binaries when
the original reflection fault recurs. Compare a debug or memory-instrumented
Slang build before attributing the fault to a particular owner; do not add
platform-specific exception suppression or silently replace a failed compile.
Raw diagnostic logs and repeat-run scripts for this investigation belong under
the ignored `Build/CrashInvestigation-20261018/` directory.

Follow-up validation on `MacOS-arm64-Debug-DurinEditor` passed 12 shuffled
whole-target material runs (seeds 1–12), 200 startup-only process launches, and
eight rounds of the three focused session tests with `MallocScribble=1` and
`MallocGuardEdges=1`. The focused rounds execute 896 direct shader compilations
without the application DDC. The Core/shader domain selection, material compiler,
material shutdown, and harness tests also pass. These results do not establish
the original reflection fault's cause; the prebuilt Slang backend was not built
with a memory sanitizer.
