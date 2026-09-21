# Static Mesh Custom Version Plan

Summary: Replace the reflected static mesh source version with an authored package custom-version domain.

Last reviewed: 2026-09-15

Status: Archived
Completed: 2026-09-15

## Current Status

The authored source domain and construct-free inspection projection are implemented.
All 20 maintained packages were inspected through the detached linker; exactly
four export static meshes: Engine Box, Sphere and SplineBox, and Sandbox
GrayboxPawn. Their packages were converted by adding the version record and
removing the reflected field and its schema entries. Each main file grew by
12 bytes. Bulk bytes were unchanged; restoring only migrated fields, descriptors
and version records reproduced the original package bytes exactly. The temporary
converter was removed after successful conversion. Validation passed: 102
StaticMeshTests (including restart, negative versions and Cook), 8
StaticMeshMaterialTests, 5 RoadSceneIntegrationTests and 13 SandboxGameplayTests.
The shared Engine API `all` build, changed-document validation and all-plan
validation passed. Content changes make affected-test selection resolve to all;
the bounded 128-test selection covers the changed behavior and project consumers.

## Goal

Register an authored source package domain, remove the reflected version and its
accessor, and name the independent bulk codec version explicitly. Preserve bulk
bytes, source identity and derived-data keys. Cooked packages must not require
the stripped source domain. In-memory copies remain independent of package versions.

## Implementation Stages

### Stage 0: Inventory and boundaries

- [x] Identify source serialization, bulk codec, identity and inspection consumers.
- [x] Inspect all maintained packages and identify the exact conversion corpus.

### Stage 1: Migrate static mesh source versions

- [x] Register and enforce the authored source domain; remove the reflected marker.
- [x] Expose file custom versions in construct-free inspection and migrate its consumer.
- [x] Convert maintained source packages offline, preserve all other data, and remove the temporary converter.
- [x] Verify missing/old/future versions, source round trips, copies, inspection and cooked stripping.
- [x] Verify maintained content after asset-runtime restart.
- [x] Complete relevant native tests, the shared Engine API `all` build, and documentation validation.

Follow [build guidance](../../../Agents/BuildAndRun.md) and
[test guidance](../../../Agents/Testing.md). The lasting version contract belongs in
[Versioning](../../../Runtime/Assets/Versioning.md).
