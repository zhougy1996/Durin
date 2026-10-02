# Derived Data Cache Record Boundary Plan

Summary: Align Durin's build/cache boundary with UE by making cache records the public cache request unit and confining byte persistence to cache internals.

Last reviewed: 2026-09-30

Status: Archived
Completed: 2026-09-30

## Current Status

The migration is complete. Build execution constructs and consumes structured
records through `ICache`; the default cache owns record decode, encode,
compression, integrity validation, and private filesystem byte storage. Cache
tests cover structured round trips and rejection of corrupt stored bytes. The
affected 47-target native-test selection and final shared-API `all` build pass.

## Goal

Build execution owns build policy, output production, and conversion between a
build output and a cache record. The cache facade accepts and returns structured
records and owns serialization, compression, integrity validation, and its
private byte backend. Test substitution occurs at the structured cache boundary.

## Implementation Stages

### Stage 0: Freeze the selected boundary

- [x] Inventory raw cache, record codec, build execution, tests, and all declared
  source consumers.
- [x] Select structured synchronous record requests as the current public
  contract; leave asynchronous batching and backend graphs to later work.
- [x] Preserve action keys, output schemas, warm-hit behavior, negative records,
  corruption fallback, and validated cold-output ownership.

### Stage 1: Move persistence behind the cache facade

- [x] Separate cache key/error types, structured records, and private raw backend
  requests so public includes remain acyclic.
- [x] Make cache Get return a validated record and Put accept a record; keep
  record encoding, compression, and filesystem envelopes below that boundary.
- [x] Retain bounded reads/writes and corruption diagnostics.

### Stage 2: Migrate build execution and consumers

- [x] Remove encode, decode, and compression decisions from build execution and
  service configuration.
- [x] Replace byte cache test hooks with structured record cache substitution.
- [x] Migrate codec, corruption, and asset-family tests without weakening their
  trust-boundary coverage.

### Stage 3: Validate and document

- [x] Update the implemented derived-data build contract and module ownership.
- [x] Pass changed-document validation, DDC tests, affected family tests, and the
  required shared-API `all` build.
- [x] Record evidence, complete the plan, and commit with Plan and Stage trailers.
