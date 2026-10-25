# Shader Job Submission Plan

Summary: Separate existing Shader job preparation from submission and remove the public admission-error protocol.

Last reviewed: 2026-10-21

Status: Archived
Completed: 2026-10-21

## Current Status

Both stages are complete. Tested the final implementation working tree based on
`11823eabc`; only documentation changed after the final code validation.

Validation on Windows, `Win64-Debug-DurinEditor`:

- Shader feature selection: 2 targets passed.
- Material feature selection: 12 targets passed.
- `RenderShaderContractTests`: 64 cases passed.
- `AssetPackageTests`: the Cook Shader-completion propagation case passed.
- `ShaderBuildQualificationTests`: compiled; performance and Metal execution were
  not run on this host.

`Win64-Debug-DurinGame` Renderer built successfully, covering the non-editor
Engine/RenderCore/Renderer interface. Reports and logs are under ignored
`Build/NativeTestResults/` and `Build/.agent-state/logs/`; final test logs begin
with `20261021-155003`, `20261021-155029`, `20261021-155141` and `20261021-155217`.
The qualification and Game build logs begin with `20261021-155237` and
`20261021-155243`. Documentation validation is recorded with the final commit.

## Goal

Prepare controls before submission, submit without a separate failure return, and
deliver cancellation or execution failure through the existing compiler output.
Keep ShaderBuild independent of Engine. Remove fixed aggregate Shader job admission
rejection thresholds; retain existing execution windows and input/output validation.
Material asset snapshot admission and generation checks remain unchanged.

## Implementation Stages

### Stage 0: Unify job submission and completion

- [x] Add preparation/submission to existing controls and remove admission errors.
- [x] Preserve independent cancellation, callback reentry, waits and shutdown safety.

### Stage 1: Migrate consumers and verify behavior

Depends on Stage 0.

- [x] Migrate material/global consumers, synchronous helpers and test backends.
- [x] Verify preparation has no execution side effects, completion occurs once,
      queued cancellation, failed dispatch and closed-owner submission terminate.
- [x] Run bounded material and Shader tests plus the affected package fixture;
      build non-editor Renderer to cover the public interface without ShaderBuild.
- [x] Update the runtime contract, validate documentation and commit.

Follow the repository [build](../../../Agents/BuildAndRun.md) and
[test](../../../Agents/Testing.md) workflows. Reports remain under ignored `Build/`.
