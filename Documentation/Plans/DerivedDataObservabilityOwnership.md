# Derived Data Observability Ownership Plan

Summary: Remove the DDC request observer compatibility layer and stop returning internal cache and input diagnostics through family APIs.

Last reviewed: 2026-09-30

Status: Completed
Completed: 2026-09-30

## Current Status

The correction is complete. DDC exposes no request observer, producer metric
callback, persistence timing, failure-source enum, or cache-operation enum.
Infrastructure detail is logged at the owning layer and accepted failures retain
only generic completion semantics. Texture request timing and persistence-phase
reporting, Shader cache-failure counters, and Engine input-phase reconstruction
were removed instead of recreated behind private adapters. Shader compiler-work
counting derives from the existing local-build status fact.

## Goal

The DerivedDataCache build API owns deterministic output construction and request
completion facts only. Texture recipe metrics remain producer-local, Shader
compiler-work counting derives from the existing local-build status fact, and
cache/input failures are logged by their owning layers. Asset APIs do not reconstruct DDC internal phases
or detailed infrastructure causes from completion.

## Implementation Stages

### Stage 0: Freeze ownership replacements

- [x] Inventory every request observer, producer metric, persistence timing, and
  family counter consumer.
- [x] Keep Texture recipe metrics in producer-local qualification paths and use
  the existing local-build status fact for Shader compiler-work counting.
- [x] Accept generic family infrastructure errors instead of preserving DDC
  input-resolution details through a private bridge.

### Stage 1: Remove DDC observation APIs

- [x] Remove `FBuildRequestObserver`, request observer options, producer metric
  callbacks, and DDC persistence timing.
- [x] Keep infrastructure failures in owning-layer logs and completion semantics
  limited to status, build status, key, and output.

### Stage 2: Move family telemetry to owners

- [x] Remove Texture recipe and persistence timing from Engine request APIs while
  retaining producer-local qualification metrics.
- [x] Derive Shader compilation count from build status and remove cache-failure
  counters instead of rebuilding a diagnostic bridge.
- [x] Remove asset input-resolution phase reconstruction and return generic
  infrastructure failures when completion has no output.

### Stage 3: Validate and document

- [x] Update implemented contracts and ownership documentation.
- [x] Pass focused DDC and family tests, affected tests, documentation validation,
  and the required shared-API `all` build.
- [x] Record evidence, complete the plan, and commit with exact plan/stage trailers.

## Validation Evidence

- `DerivedDataBuildTests`: 30 passed.
- `DerivedDataCacheTests`: 15 passed.
- `RenderShaderBuilderTests`: 22 passed.
- `StaticMeshTests`: 149 passed.
- `TextureTests`: 133 passed.
- `./DevTool test affected --report`: all affected targets passed except one
  parallel `RendererSceneContractTests` timeout; its isolated rerun passed all 69
  tests in 0.9 seconds.
- `./DevTool build`: shared-API `all` target passed.
- `./DevTool doc validate --scope changed` and
  `./DevTool doc plan validate --scope all`: passed.
