# TextureCube Owner Async Bridge Plan

Summary: Migrate TextureCube platform-cache owners to asynchronous derived-data sessions.

Last reviewed: 2026-10-12

Status: Archived
Completed: 2026-10-12

## Current Status

All stages are complete. Cube platform-cache dispatch uses the shared asynchronous
texture session and owner mailbox. Canonical and normalized panorama paths share
source definition validation and output assembly with synchronous consumers.
Lasting behavior is recorded in
[Asset Compilation](../../../Runtime/Assets/AssetCompilation.md#initial-compiling-managers)
and [Derived Data Build](../../../Runtime/Assets/DerivedDataBuild.md#family-boundaries).

Validation on `Win64-Debug-DurinEditor`:

- Final `test TextureTests --report`: all 142 cases passed, including 16 Cube cases
  covering six-face, HDR/LDR panorama, PostLoad, synchronous rebuild and Cook paths.
- `test AsyncTaskPilotLifecycleTests --report`: passed with two Core workers. New
  Cube coverage proves retained owner slots during slow reads, worker availability,
  cancellation, slow persistence, callback retirement, selected finish, noncanonical
  panorama normalization, HDR warm hits without another source read, supersession,
  and combined Cube/Texture2D suspended-I/O shutdown. It also verifies queued
  requests cancel when the scheduler has already stopped.
- `test "@module=derived-data-cache" --report`: all five targets passed, including
  the final lifecycle test and shared build/cache and shader consumers.
- `test AssetCompilingManagerTests --report`: passed.
- `build --target all`: passed; build log:
  `Build/.agent-state/logs/20261012-022455-330811-70864-cmake.log`.
- Changed documentation, all-plan validation and diff whitespace checks passed.

Validation uses bounded CPU suites for the changed owner and DDC behavior; the
all build covers Engine, Sandbox, RoadWeaver and native target compilation.
GPU behavior, canonical keys and output schemas are unchanged. Detached import,
Cook and explicit `RebuildPlatformData` remain synchronous.

## Goal

Move TextureCube owner cache queries, producer continuations and persistence onto
the asynchronous texture session. Keep detached import and Cook APIs synchronous,
preserve canonical keys and output schemas, and apply results only on GameThread.

## Selected Decisions

- Replace the private platform-cache worker Build contract with a callback-based
  dispatch contract retaining the owner's build request group and cancellation.
- Reuse Texture2D owner readiness, admission budgets and shutdown drain rules.
- Canonical six-face and HDR sources query before reading source pixels.
- Noncanonical panoramas normalize on the dispatch worker, then use the same
  asynchronous source build and shared-output assembly as canonical sources.
- Keep normalization and source validation shared with synchronous consumers;
  add no worker wait, thread pool, wire revision or second asset reservation.
- When Core is already stopped, cancel queued owner requests without dispatch.
  The combined suspended Cube/Texture2D shutdown regression exposed this preexisting
  queue boundary, so it is included in the bridge's lifecycle handling.

## Implementation Stages

### Stage 0: Record the Cube bridge

- [x] Inspect platform-cache input and source-build boundaries.
- [x] Save and validate the selected plan before changing source.

### Stage 1: Migrate Cube owner consumption

- [x] Add asynchronous Cube source dispatch and share output consumption.
- [x] Bridge canonical and normalized panorama inputs to the owner mailbox.
- [x] Preserve cancellation, callback retirement, latest-wins and shutdown behavior.
- [x] Update the lasting asset/build contracts.

### Stage 2: Validate and deliver

- [x] Add two-worker Cube regressions for slow I/O, cancellation and selected finish.
- [x] Pass Texture and owner lifecycle suites, relevant DDC tests and the all build
  using [build](../../../Agents/BuildAndRun.md) and [testing](../../../Agents/Testing.md) guidance.
- [x] Validate documentation and all plans, record evidence and commit.
