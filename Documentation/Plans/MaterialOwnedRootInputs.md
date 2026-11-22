# Material-Owned Root Inputs Plan

Summary: Move authored root inputs and placement onto the base material and project the root in MaterialEditor.

Last reviewed: 2026-11-23

Status: Completed
Completed: 2026-11-22

## Current Status

Base materials own their individual root properties, packed input, mode and root coordinates. MaterialEditor projects that state onto the graph root and records edits on the material. The subsequent [v6 cutover](UEMaterialAttributeInputsV6.md) supersedes this plan's package format.

Use the [material system](../Runtime/Rendering/Materials/MaterialSystem.md) and [graph operations](../Editor/Architecture/Materials/MaterialGraphOperations.md) for current contracts. Coordinate changes to gesture replay and layout with [Material Graph Transaction Alignment](MaterialGraphTransactionAlignment.md).

Win64 Debug acceptance at the cutover based on `332df5c1c` included the workspace build, reopening all 15 staged packages and comparing 558 graph and compiler records. The cutover reports are under `Build/MaterialOwnedRootInputs/Stage4/`.

## Implementation Stages

### Stage 1: Inventory assets and capture migration evidence

- [x] Identify the three shipped base materials requiring conversion and capture graph and compiler baselines.

### Stage 2: Move the authored root and convert packages

- [x] Move root state onto base materials and convert the tracked packages.

### Stage 3: Project the editor root and record changes on the material

- [x] Route root edits and replay through the owning material.

### Stage 4: Remove the legacy terminal and close acceptance

- [x] Retire terminal decoding and validate the shipped graphs and workspace consumers.
