# Derived Data Build Diagnostics Boundary Plan

Summary: Align Durin's build diagnostics boundary with UE by separating deterministic output messages, transient logs, completion status, and request observation.

Last reviewed: 2026-09-30

Status: Completed
Completed: 2026-09-30

## Current Status

This migration established status-only completion and separated deterministic
messages from transient logs. Its temporary request-observation compatibility
path was subsequently removed by
`Documentation/Plans/DerivedDataObservabilityOwnership.md`; the implemented
contract no longer exposes request metrics, persistence timing, cache failures,
or coarse failure sources.

## Goal

Accepted build requests complete with only status, build-status facts, cache key,
and optional output. Deterministic producer diagnostics remain output messages and
may be cached. Non-deterministic producer logs remain attached only to the cold
output and prevent cache storage. Cache failures, metrics, and timing are optional
request observations and never affect completion semantics.

## Implementation Stages

### Stage 0: Freeze the diagnostic ownership model

- [x] Inventory completion reports, producer messages, metrics, cache diagnostics,
  and all declared consumers.
- [x] Select request observation as the compatibility path for family telemetry.
- [x] Keep admission rejection separate from accepted-request completion.

### Stage 1: Separate output diagnostics and request observations

- [x] Add transient output logs and prevent outputs containing logs from being
  stored in the cache.
- [x] Replace completion reports and operation-tagged cache errors with typed,
  best-effort request observations.
- [x] Reduce build completion to status, build status, cache key, and output.

### Stage 2: Migrate families and tests

- [x] Move Texture and Shader metrics to request observation.
- [x] Stop asset families from interpreting DDC internal execution phases.
- [x] Cover deterministic messages, transient logs, cache fallback observation,
  and store success facts.

### Stage 3: Validate and document

- [x] Update the implemented derived-data build contract and module ownership.
- [x] Pass focused DDC and family tests, affected tests, documentation validation,
  and the required shared-API `all` build.
- [x] Record evidence, complete the plan, and commit with exact plan/stage trailers.
