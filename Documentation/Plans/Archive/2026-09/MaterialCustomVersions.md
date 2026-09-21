# Material Custom Versions Plan

Summary: Move material graph and instance parameter package compatibility to registered Custom Versions and convert the maintained material corpus.

Last reviewed: 2026-09-15

Status: Archived
Completed: 2026-09-15

## Current Status

Conversion complete: all eight package rewrites passed canonical roundtrip and
byte-identical reconstruction after removing only the new version record. The
temporary converter was removed. Final validation passed: `all` build,
MaterialPackageTests (5), MaterialCookTests (6), MaterialFunctionTests (28), and
MaterialEditingTests (104), totaling 143 retained tests. The conversion itself
passed its one temporary test before removal. Changed-document and all-plan
validation passed.

Inventory: the clean baseline already removed `GraphOwnershipVersion`.
`ParameterStorageVersion = 2` remains on instances. Of 20 maintained packages,
eight Engine packages contain material exports: DefaultMaterial and seven
functions (DecodeImportedNormalRG, ImportedSurfaceValues, SampleNormal,
SampleORM, StandardPBR, StandardPBR_ORM, UVTransform). No maintained instance
exports were found. All custom-version tables are empty.

## Goal

Use one registered version domain for Material/MaterialFunction and another for
MaterialInstance parameter storage. Both start at version 1 of their new domains.
Require exact current versions for authored/cooked package loading; in-memory
duplication, snapshots and object graphs retain their existing unversioned behavior.
Keep non-version `AlwaysSerialize` fields and StaticMesh versioning unchanged.

## Implementation Stages

### Stage 0: Inventory and scope

- [x] Inspect current serializers and all Engine/Sandbox/RoadWeaver package exports.
- [x] Select an exact offline table conversion for the eight graph packages.

### Stage 1: Migrate material package versions

- [x] Register the two domains and integrate package/discovery serialization.
- [x] Remove the instance marker and replace marker tests with version tests.
- [x] Use a temporary bounded converter to add graph versions to the eight packages;
      verify canonical bytes, unchanged non-version linker data and bulk payloads.
- [x] Remove the temporary converter; retain strict runtime version checks.
- [x] Validate authored reload, function/material/instance duplication, missing/old/future
      versions, mixed domains and Cook with focused tests and an `all` build.
- [x] Document the lasting contract and complete plan/document validation.

Validation follows [agent testing](../../../Agents/Testing.md) and
[build guidance](../../../Agents/BuildAndRun.md). No persistent legacy reader is introduced.
