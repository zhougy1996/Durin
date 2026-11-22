# UE Material Attribute Inputs v6 Plan

Summary: Adopt typed material root fields and connection-only Make and Set Material Attributes inputs.

Last reviewed: 2026-11-23

Status: Completed
Completed: 2026-11-23

## Current Status

The v6 cutover shipped DefaultMaterial, DefaultWater and M_MetalRust. SampleNormal, SampleORM and UVTransform contain no exports that require the material output version. RoadWeaver content contains no authored material or material-function exports.

Use the [material system](../Runtime/Rendering/Materials/MaterialSystem.md), [expression building](../Runtime/Rendering/Materials/MaterialExpressionBuilding.md), [graph operations](../Editor/Architecture/Materials/MaterialGraphOperations.md) and [package versioning](../Runtime/ObjectSystem/Versioning.md) contracts for follow-up work. Materials and Make and Set exports require output version 6. The version also participates in Cook identity. Base Color defaults to black. Unconnected Make inputs use property defaults and unconnected Set overrides preserve the prototype value.

Win64 Debug acceptance at the cutover based on `c82e2728e` included the workspace build, 860 native regressions and a match against 558 ordered graph and compiler baseline records. The asset audit covered 28 tracked packages and verified 112 LFS payloads. The cutover reports are under `Build/MaterialV6/Stage4/`. Current-format persistence coverage generates its graphs in the test work directory and also opens the six shipped material and function graphs.

## Implementation Stages

### Stage 1: Inventory v5 inputs and establish the black fallback

- [x] Audit workspace packages and establish the black Base Color baseline.

### Stage 2: Introduce the v6 schema and conversion bridge

- [x] Convert active literals into explicit constants and validate the staged packages.

### Stage 3: Align MIR and authoring consumers

- [x] Use common aggregate evaluation and route editor changes through the owning input property.

### Stage 4: Publish converted content and retire the bridge

- [x] Publish the three converted materials, remove the conversion bridge and validate the strict reader.
