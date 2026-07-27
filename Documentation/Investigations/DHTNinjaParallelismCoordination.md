# DHT And Ninja Parallelism Coordination

**Status:** Open  
**Last reviewed:** 2026-07-27

## Scope And Verdict

DurinHeaderTool and Ninja currently make parallelism decisions independently.
Ninja counts one module-level DHT custom command as one job, while that Python
process can create several libclang parser processes that Ninja cannot see.
The `durin_dht` pool limits how many parent commands may coexist, but it neither
charges their parser children against Ninja's global budget nor lends unused DHT
capacity back to compilation.

This is a scheduling-boundary problem, not primarily a request to raise
`DURIN_DHT_WORKERS`. No fixed combination of Ninja jobs, DHT pool depth, and DHT
worker limit can both avoid oversubscription during mixed compilation/DHT work
and consume all CPUs when a large DHT module is the only ready critical-path
work.

Keep the current bounded worker policy as the supported fallback until an
implementation is selected. When work begins, choose between a shared jobserver
and exposing DHT parse work as Ninja-visible edges; do not tune the existing
constants and treat that as a complete solution.

Relevant implementation and architecture:

- [`BuildSystem.md`](../Development/Build/BuildSystem.md#generated-metadata-flow);
- [`BuildOptions.cmake`](../../CMake/Config/BuildOptions.cmake), which defines
  `DURIN_DHT_WORKERS`, `DURIN_DHT_JOB_POOL_SIZE`, and the `durin_dht` pool;
- [`ProjectTargets.cmake`](../../CMake/Project/ProjectTargets.cmake), which
  creates one export command and one reflection command per reflected module;
- [`parallelism.py`](../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/runtime/parallelism.py);
- [`module_export_file_generator.py`](../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/generators/module_export_file_generator.py);
- [`module_reflection_files_generator.py`](../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/generators/module_reflection_files_generator.py);
- [`config.py`](../../Tools/BuildTool/durin_build_tool/config.py), especially
  `resolve_jobs`;
- [`core.py`](../../Tools/BuildTool/durin_build_tool/core.py), which passes
  the resolved job count to `cmake --build`.

## Verified Findings

### P2 — Ninja cannot account for DHT parser children

BuildTool reserves two logical processors by default. On the reviewed machine,
`os.cpu_count()` reports 20, so BuildTool invokes the build with 18 Ninja jobs.
The configured DHT defaults permit two active module commands and four parser
workers inside each command.

When enough non-DHT work is ready, approximate runnable CPU work is:

```text
Ninja job limit
- active DHT parent commands
+ active DHT parser processes
```

With the current defaults, one four-worker DHT command can therefore produce
approximately 21 runnable workers, and two can produce approximately 24. In the
opposite case, when the dependency graph exposes only one Engine DHT command,
only its parser workers can consume CPU even though Ninja has unused job slots.

**Impact:** the operating-system scheduler absorbs oversubscription during
mixed phases, while dependency-constrained DHT phases can leave CPUs idle. The
same constants cannot correct both behaviors.

### P2 — The DHT pool is a concurrency cap, not a shared CPU budget

Both module-level custom commands use the `durin_dht` Ninja pool. Ninja pools
limit the number of active edges assigned to that pool, but an edge still has a
unit cost. The current graph has no way to express that one DHT edge may consume
four parser CPUs while another consumes one.

Separate fixed compiler and DHT pools would also be incomplete. Reserving CPU
for DHT prevents compilation from borrowing those slots after DHT finishes;
reserving for compilation prevents a critical-path DHT module from borrowing
idle compiler slots.

**Impact:** changing only `DURIN_DHT_JOB_POOL_SIZE`, `DURIN_DHT_WORKERS`, or the
BuildTool job count may improve one workload snapshot while regressing another.

### P2 — Engine exposes enough parse work to benefit from dynamic borrowing

The Engine module currently declares 25 reflected headers. Its full export and
reflection commands each parse those headers independently and select four
workers under the current threshold policy.

Two recorded full-generation samples from the existing Ninja log were:

| Phase | Sample 1 | Sample 2 |
| --- | ---: | ---: |
| Engine export | 15.817 s | 9.128 s |
| Engine reflection | 7.520 s | 11.910 s |
| Serial total | 23.337 s | 21.038 s |

Read-only isolated parser measurements, without competing compilation, reduced
export parsing from 6.411 seconds at four workers to 4.272 seconds at eight, and
reflection parsing from 6.490 seconds to 4.386 seconds. These measurements do
not justify a global eight-worker default: they demonstrate available
parallelism when CPU is idle, while deliberately excluding the contention that
must be coordinated during a real build.

Engine is also an order-only predecessor of its own compilation and of multiple
dependent module compile-order targets in the generated Ninja graph. A long
Engine DHT phase can therefore sit on a broad build critical path.

**Impact:** a scheduler that can lend otherwise idle build capacity to Engine
DHT has a plausible critical-path benefit. A permanently larger hidden worker
pool risks taking the same capacity away from ready compiler jobs.

## Confirmed Correct Behavior

- Export and reflection manifests already cache at header granularity. A
  one-header incremental change normally selects one parser task in each phase,
  so larger fixed worker counts do not help that case.
- Export and reflection commands must remain ordered because reflection consumes
  the module's own export plus dependency exports.
- Generated files use compare-before-write and atomic replacement. Any finer
  build graph must preserve those restat-friendly and interruption-safe
  properties.
- Cross-process output locking remains necessary. Finer Ninja-visible work
  cannot share the current module-wide writer lock if that lock serializes all
  otherwise independent header outputs.
- A DHT worker limit remains useful as a memory/safety ceiling and as a fallback
  for direct DHT invocation. It should not remain the primary global CPU
  allocator after coordination is introduced.

## Candidate Directions

### Shared jobserver

BuildTool can own one cross-process token budget and let Ninja edges and DHT
parser children consume tokens from it. DHT would request extra tokens for
parallel parsing and return each token as work completes. Its configured worker
count would become a ceiling; the shared token supply would determine actual
parallelism.

This preserves the current module-level CMake graph and DHT caches, but has a
toolchain prerequisite. The reviewed build uses Visual Studio's bundled
`ninja.exe` at version 1.12.1. Ninja added GNU jobserver client support in 1.13,
and enables it only when no explicit `-j` is passed and `MAKEFLAGS` describes a
valid jobserver. See the
[Ninja GNU jobserver documentation](https://ninja-build.org/manual.html#_gnu_jobserver_support).
Ninja is a client rather than the top-level server, so upgrading alone is not
enough: BuildTool would still need to create and own the jobserver.

CMake's `JOB_SERVER_AWARE` option is not a shortcut for the current Ninja
generator; CMake documents it as ignored outside its Makefile generators. See
[`add_custom_command`](https://cmake.org/cmake/help/latest/command/add_custom_command.html).

Do not replace Visual Studio's installed Ninja binary in place. If this
direction is selected, use either a future compatible Visual Studio-bundled
version or a separately managed Ninja executable selected by the Durin
toolchain configuration.

### Ninja-visible header or shard edges

Without a jobserver-capable Ninja, move the hidden parser work into the generated
Ninja graph:

1. generate independent per-header export fragments;
2. merge fragments into the module export and export manifest;
3. generate each header's reflection outputs after the required module exports;
4. finalize the module manifest, cleanup state, and reflection stamp.

Each parser process then consumes one ordinary Ninja job, making the existing
BuildTool job count the single CPU budget. Header edges provide the best
incremental granularity and enough ready work for Engine to fill the machine.
Fixed shards can reduce Python/libclang startup cost, but need per-header cache
checks inside each shard and expose less scheduling flexibility.

This direction requires a larger DHT/CMake refactor than a shared jobserver. It
must also replace the module-wide writer boundary with header-fragment ownership
plus serialized merge/finalization edges.

## Recommendation And Decision Trigger

Do not create an implementation plan yet. Re-evaluate this issue when either:

- the Visual Studio-bundled Ninja used by the active preset gains compatible
  jobserver support;
- Durin adopts a separately managed Ninja toolchain;
- measured clean-build or broad DHT invalidation time is prioritized for a
  build-performance milestone.

At that point:

1. verify the actual Ninja version and invocation path;
2. prototype the shared jobserver if Ninja is compatible;
3. otherwise prototype Ninja-visible per-header or shard edges;
4. compare both against the current graph before adopting architecture.

Prefer the shared jobserver when it can be introduced without replacing a
machine-managed Ninja installation. Prefer Ninja-visible edges when the existing
Ninja toolchain must remain unchanged.

## Validation Gaps

- No mixed compilation/DHT CPU trace has been captured. The current evidence
  combines Ninja edge timings with isolated parser measurements.
- Peak memory and memory-bandwidth behavior at higher parser concurrency have
  not been measured.
- Python startup, libclang loading, and configuration loading costs for
  per-header Ninja edges have not been measured.
- Jobserver fairness between ready Ninja compilation edges and waiting DHT
  parser children needs a prototype; CPU utilization alone does not prove a
  shorter critical path.
- The current graph was inspected using one Win64 Debug DurinEditor Tests build.
  Other profiles may expose a different mix of reflected modules.

## Reproduction And Acceptance

Use an isolated worktree or preset for full-generation measurements;
do not delete shared DHT outputs from an active preset merely to force a
benchmark. Capture:

- Ninja version, resolved BuildTool job count, DHT worker ceiling, and DHT pool
  depth;
- per-module export/reflection wall time;
- active compiler and parser process counts over time;
- CPU utilization, peak memory, and total build wall time;
- clean/full-generation and one-header incremental workloads.

The issue is ready to close when all of the following hold:

1. One global budget accounts for compiler work and every active parser process,
   or parser work is represented directly as Ninja jobs.
2. When at least the global budget's worth of independent work is ready, the
   build keeps the CPUs occupied without persistent oversubscription.
3. A critical-path Engine DHT phase can borrow idle compilation capacity, and
   that capacity returns automatically when compiler work becomes ready.
4. One-header incremental DHT behavior does not regress materially.
5. Generated exports, reflection sources, manifests, cleanup, locking, and
   interrupted-process recovery remain deterministic and race-free.
6. Focused DHT tests and a full `all` build pass for the same preset, followed
   by the required hidden-window DurinEditor smoke test.
