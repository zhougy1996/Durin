# Rendering Performance Baseline

Summary: Define the reproducible RHI, RDG, and Renderer performance qualification selection, measurement ownership, authority, and comparison rules.

Last reviewed: 2026-10-15

## Selection

The `render-performance` native-test domain is the single discovery and batch
execution entry point for repository-owned rendering measurements:

```bash
./DevTool configure --preset MacOS-arm64-Release-DurinEditor -DDURIN_ENABLE_TRACY=OFF
./DevTool test list "render-performance"
./DevTool test "@domain=render-performance" --mode qualification --preset MacOS-arm64-Release-DurinEditor --test-jobs 1 --report
```

Keep the targets separate. Each target owns a coherent module/backend lifecycle,
and the shared `durin-gpu` and `durin-rhi-lifecycle` locks do not make independent
DevTool invocations or external GPU users quiet. The domain is an execution
catalog, not a new aggregate test process.

Use an ordinary Release Editor preset for performance comparison. Debug, Tracy,
validation, sanitizer, capture-tool, and concurrent-machine results remain useful
diagnostics but cannot establish or replace a release performance budget. The
repository's Release preset currently resolves `DURIN_ENABLE_TRACY=AUTO` to on,
so explicitly configure it off and verify `DURIN_WITH_TRACY=0` in the measured
target's compile definitions. Run correctness gates separately from timing
acceptance.

## Measurement Inventory

| Target | Workload and authority | Primary observations |
| --- | --- | --- |
| `RDGQualificationTests` | Backend-free synthetic graph compilation; diagnostic on every host | declaration/compile median and p95, deterministic dump, dependency count |
| `VulkanCreationQualificationTests` | Real Vulkan cold/hot/concurrent creation and checked binding submission | shader/PSO/resource creation time, instrumentation overhead, allocation and request pressure; binding producer and queue/replay CPU samples |
| `MaterialCreationQualificationTests` | Fixed production material frames | producer/render/GPU completion spans, synchronous operations, queue and native creation counts |
| `StaticMeshRenderPreparationVulkanTests` | Mixed StaticMesh/Spline preparation with 64 primitives and 256 draws | preparation median/p95 and owned container capacity |
| `DirectionalShadowBaselineVulkanTests` | Single and cascaded shadow preparation plus rendered output | logical/discovery/static preparation, GPU shadow time, output parity |
| `HDRDisplayMappingQualificationTests` | 1920x1080 copy and FXAA routes | GPU median/p95 and incremental FXAA cost |
| `GBufferQualificationTests` | 1920x1080 production GBuffer/deferred/AO/shadow/contact/postprocess frame | per-pass and total GPU median/p95, retained and peak memory, output parity |
| `VolumetricCloudQualificationTests` | Spatial compute/fragment routes and temporal quality tiers | GPU median/p95, samples, route counters, history and retained target bytes, output parity |

Do not add a target to this domain merely because it initializes Vulkan. It must
own a stable performance, scale, or memory workload and publish reproducible
observations. Correctness-only Vulkan integration remains outside this catalog.

### Binding Update and Submission Workload

`FVulkanCreationQualificationTests.BindingUpdateAndSubmission` uses the checked
`DeferredBufferVersions.slang` input for graphics and compute, each with direct
and indirect commands. Each sample records 1024 draws or dispatches in a complete
RHI frame, so descriptor caches follow normal frame retirement. Scenarios
reuse unchanged bindings, replace one uniform binding while retaining storage,
or reset the same pipeline and restore its bindings. Pipeline reset measures
the existing reset boundary; it does not measure a change to a different PSO.
The fixture checks all output pixels, including alternating uniform values.

For each scenario, six runs provide three reference A/A pairs, with 30 warm-up
and 120 measured samples per run. `BindingTiming/samples.csv` records producer
parameter-update time, producer command-recording time, queue/replay completion
time, and combined CPU time in nanoseconds. The combined interval includes
render-pass/pipeline setup and RHI-thread replay; GPU idle waits and readback
are outside it. Producer columns do not isolate backend parameter-update or
native encoding time from replay. `device.txt` also records the cold combined
graphics/compute pipeline creation duration and total device cache metadata;
these are observations, not isolated prepared-operation storage measurements.

Set `DURIN_TEST_KEEP_WORK=1` when collecting a receipt; successful tests otherwise
remove their work directories and raw samples. Preserve the printed work path
alongside the log and XML. Report medians/p95 and A/A noise from the raw rows;
the fixture also publishes per-run numeric timing properties and stable
`BindingTiming` console lines. It does not infer a quiet lane, assert device-independent timing
budgets, or supply Metal/mapping-counter coverage. Sparse bindings, descriptor
arrays, dynamic offsets, and distinct-pipeline reuse retain separate correctness
coverage in `VulkanRHIIntegrationTests`.

## Baseline Matrix

The default baseline is Vulkan, Release Editor, dedicated RHI thread, validation
off, no capture tool, production async-compute policy off, and backend queue
policy disabled. Record deviations rather than silently comparing them:

| Dimension | Required baseline | Optional comparison |
| --- | --- | --- |
| CPU execution | threaded RHI | inline replay to isolate queue/replay cost |
| GPU queues | graphics queue only | explicitly opted-in same-family or dedicated compute |
| transitions | production default | forced full versus split lowering on event-capable hardware |
| shader/PSO state | warm unless the workload names cold creation | cold creation/cache-miss run |
| diagnostics | Vulkan validation off | validation-on correctness observation |
| build | Release Editor, Tracy off | Debug/Tracy diagnostic only |

Target-owned fixtures define their frozen resolution, content, warm-up, sample
count, and correctness checks. New frame-oriented measurements use at least 30
warm-up frames, 120 measured frames, median and nearest-rank p95, and three
consecutive runs. Creation workloads may use a smaller explicitly documented
round count because cold-state reset is part of their cost.

## Authority and Comparison

A result is authoritative only when all of the following are recorded:

- exact Git revision, preset/configuration, compiler, and runtime options;
- GPU/device name, driver, Vulkan API/SDK and validation state when applicable;
- workload identity, resolution, warm-up, samples, runs, and cache state;
- raw test log and XML report path;
- an exclusive quiet timing lane as defined by
  [Native Test Qualification](NativeTestQualification.md#performance-qualification-and-concurrent-agents).

Results from another adapter, MoltenVK fallback, Debug/Tracy builds, or a busy
machine are observations. They may find a large regression or validate the
measurement pipeline, but they must not freeze a threshold or accept a timing
gate. Existing named RTX 3090 gates stay adapter-qualified; never transplant
their thresholds to another device.

Compare the candidate and reference revision with identical instrumentation,
fixture data, cache preparation, environment, and alternating run order when
both revisions can be measured in one quiet window. Preserve raw samples.
Median protects the central result; p95 detects intermittent stalls. A stable
sustained external load is not distinguishable from a regression without the
exclusive lane.

## Result Recording

Test-specific console prefixes remain part of their owning fixtures. Machine-
readable values also belong in GoogleTest properties. Qualification targets run
through CTest, whose JUnit aggregate does not retain inner GoogleTest properties;
the stable prefixed console lines and a normalized checked-in receipt therefore
remain required. Use lowercase property names with an explicit unit suffix such
as `_ns`, `_us`, `_bytes`, or `_count`; include warm-up/sample/run counts and an
`authority` value. Do not encode device-dependent thresholds into a generic
target name.

Plans may cite a result receipt and summarize the decision, but this document
owns the lasting selection and comparison protocol. Plans do not duplicate the
target inventory or quietly redefine authority.

## Related Documentation

- [Native Test Qualification](NativeTestQualification.md)
- [Native Test Execution](NativeTests.md)
- [Native Test Authoring](NativeTestAuthoring.md)
- [CPU Profiling](Profiling.md)
- [RHI Diagnostics and Conformance](../../Runtime/Rendering/RHIDiagnosticsAndConformance.md)
- [Render Graph](../../Runtime/Rendering/RenderGraph.md)
- [2026-09-29 Apple M4 diagnostic receipt](RenderingPerformanceBaselineReceipt20260929.md)
