# Windows Long Paths and Atomic File I/O Plan

Summary: Windows process long-path capability, Core-owned atomic publication, diagnostics, and long-worktree validation.

Last reviewed: 2026-07-26

## Current Status

Active. Stages 0-2 are implemented and validated on 2026-07-26. The Windows
host policy is a checked development prerequisite, `DurinLauncher` and every
native-test executable receive one shared `longPathAware` manifest policy, and
their final PE images are verified after linking. Core now owns atomic byte
publication for DDC, shader-cache, and asset-package writes. CoreTests (121),
RenderCoreTests (48), and AssetCoreTests (30) passed after the publication
migration; the rebuilt CoreTests and DurinLauncher images passed embedded
manifest verification. Stage 3 is in progress: Core coverage now crosses 300
characters, covers the historical temporary-name regression, diagnoses a
256-character component, and exercises DDC plus shader binary, reflection, and
manifest round trips from dynamically constructed long roots. The lasting
runtime and build contracts are published; the full long-worktree end-to-end
matrix remains.

## Goal

Make repository-owned Windows runtime and native-test processes tolerate long
absolute paths, and make atomic file publication independent of destination-name
length, while preserving portable behavior and actionable failure diagnostics.

## Scope

- Define the supported Windows long-path contract and development-machine
  prerequisite.
- Embed one repository-owned `longPathAware` manifest policy in runtime and
  native-test executables.
- Centralize same-directory atomic file publication in Core.
- Migrate DerivedDataCache and shader-cache publication to the shared Core API.
- Add deterministic coverage for long paths, destination replacement,
  concurrent writers, cleanup, and diagnostics.
- Document the lasting build and file-I/O contracts after implementation.

## Non-Goals

- Automatically modifying the Windows registry or Group Policy.
- Guaranteeing long-path support in Git, CMake, Ninja, Slang, the Windows shell,
  or other independently launched third-party processes.
- Removing filesystem component-length limits, which commonly remain 255
  characters even when total long paths are enabled.
- Supporting arbitrary relative paths beyond `MAX_PATH`; long-path operations
  use normalized absolute paths.
- Replacing every direct `std::filesystem` use in the repository.
- Adding remote, network-shared, or transactional multi-file cache semantics.

## Design Decisions and Invariants

- Core owns reusable atomic file publication; cache subsystems do not maintain
  private temporary-path and replacement implementations.
- A temporary file is an immediate sibling of its destination so publication
  remains a same-volume atomic replacement.
- Temporary filenames contain only a fixed prefix, process identity, and
  monotonic uniqueness token. They never repeat the destination filename.
- Publication is last-writer-wins across processes. Readers observe either the
  previous complete file or the new complete file, never partial bytes.
- Failed writes and replacements perform best-effort cleanup without removing
  the destination or traversing outside its parent directory.
- Windows errors preserve the native error code, operation, final path, total
  path length, and longest component length in diagnostics.
- `longPathAware` is applied to process images, not DLL targets. The launcher and
  every target created through `add_durin_test(...)` receive the same manifest
  declaration.
- The repository reports a missing `LongPathsEnabled` host capability with
  remediation guidance. It never elevates privileges or changes machine policy
  on the user's behalf.
- The existing short shader-cache temporary naming remains valid after
  `longPathAware` support lands; long-path capability is not a reason to permit
  avoidable path inflation.
- Platform-neutral callers continue to use `std::filesystem::path`. Any Windows
  extended-path conversion is centralized and never exposed in serialized,
  logged-as-identity, virtual, or cache-key paths.

## Current Foundations and Gaps

- `FFileHelper` owns common byte-file reads, writes, atomic publication, and
  structured publication diagnostics.
- DerivedDataCache, ShaderCacheStore, and asset-package saves use the shared
  Core publication path. Commit `4365c0d7` first shortened shader temporary
  names; Stage 1 removed that private implementation.
- `add_durin_test(...)` is the central constructor for native-test executables,
  while `DurinLauncher` is created separately; both paths call the shared
  long-path helper.
- The shared manifest declares `longPathAware`, and final executable images are
  checked after linking.
- Native tests have isolated `DURIN_TEST_WORK_DIR` roots, and Core path tests
  dynamically cross the traditional Windows total-path boundary.
- Windows long-path behavior requires both a process manifest declaration and an
  enabled host policy; BuildTool enforces the latter before toolchain work.
- Full validation from a deliberately long worktree remains open Stage 3 work.

## Stage 0 Decisions and Evidence

- Repository-owned Windows executable targets are `DurinLauncher` plus
  `CoreTests`, `CoreDObjectTests`, `AssetCoreTests`, `EngineTests`,
  `RenderCoreTests`, and `VulkanRHITests`, all of which are constructed through
  `add_durin_test(...)` except the launcher.
- Direct native byte-publication implementations existed in
  `DerivedDataCache::WriteFileAtomically`, `ShaderCacheStore`, and
  `FAssetManager::SavePackage`. Asset move and delete staging are multi-file
  rollback workflows rather than byte publication and remain AssetCore-owned.
- `LongPathsEnabled=1` is a hard Windows development prerequisite. Setup and
  BuildTool report the missing registry or Group Policy setting and never modify
  it.
- Supported Windows paths are normalized absolute local-drive paths on Windows
  10 version 1607 or newer, with each component within the filesystem limit.
  Relative paths beyond `MAX_PATH`, extended UNC paths, shell commands, build
  tools, and independently launched third-party processes remain outside the
  contract.
- MSVC 14.44 `std::filesystem`, file streams, and the selected wide Win32
  operations use the process opt-in. CoreTests demonstrate creation, byte I/O,
  metadata, traversal, replacement, and cleanup above 300 characters on the
  configured host; no extended-path adapter is required for the covered
  operations.
- `durin_target_enable_windows_long_paths(...)` attaches the shared manifest as
  a target source. CMake's MSVC manifest flow merges and embeds it, and a
  post-link verifier inspects the final PE. This avoids per-target declarations
  and covers both the test constructor and the separate launcher.

## Implementation Stages

### Stage 0: Lock the Windows Capability Contract

- [x] Inventory every repository-owned Windows executable target and every
  direct atomic-replacement implementation.
- [x] Decide whether `LongPathsEnabled` is a hard Windows development
  prerequisite or a reported optional capability with conditional tests.
- [x] Verify the supported MSVC standard library and Win32 operations used by
  `std::filesystem`, streams, directory traversal, and atomic replacement under
  paths longer than `MAX_PATH`.
- [x] Select one manifest attachment mechanism that covers generated native-test
  targets and the separately declared launcher without per-target duplication.
- [x] Record any unsupported path forms, Windows versions, or tool boundaries
  before implementation begins.

#### Acceptance Gate

- The host-policy behavior, executable target inventory, supported path forms,
  and manifest attachment point are explicit and have no conflicting open
  alternatives.
- A minimal probe demonstrates the expected behavior both below and above
  `MAX_PATH`, or records the exact standard-library operation requiring a
  centralized Windows extended-path adapter.

### Stage 1: Centralize Atomic File Publication in Core

- [x] Add a Core-owned byte publication API with same-directory fixed-length
  temporary naming, atomic replacement, and best-effort cleanup.
- [x] Make temporary-name uniqueness safe for concurrent threads and separate
  processes without including the destination filename.
- [x] Return structured failure information sufficient to report operation,
  native error code, path length, and component length.
- [x] Migrate `DerivedDataCache::WriteFileAtomically` to the shared
  implementation while preserving its public compatibility contract.
- [x] Migrate shader binary, reflection, and dependency-manifest publication to
  the shared implementation and remove its private replacement logic.
- [x] Migrate asset-package byte publication to the shared implementation;
  retain AssetCore's separate multi-file move and delete rollback workflows.
- [x] Add Core unit tests for initial publication, replacement, concurrent
  writers, failed replacement cleanup, and destination preservation.

#### Acceptance Gate

- CoreTests prove that readers never receive partial bytes and failed
  publication does not damage an existing destination.
- RenderCore no longer owns private temporary-path or atomic-replacement code.
- Atomic temporary filename length is constant with respect to destination
  filename length.
- CoreTests and RenderCoreTests pass through the repository BuildTool workflow.

### Stage 2: Enable Long-Path-Aware Windows Processes

- [x] Add a repository-owned Windows application manifest containing
  `longPathAware=true`.
- [x] Attach the manifest through the selected CMake helper to every native-test
  executable and to `DurinLauncher`.
- [x] Add a configure-time or BuildTool preflight check for
  `LongPathsEnabled`, following the Stage 0 decision without changing machine
  state.
- [x] Report missing policy with the exact Windows setting, affected capability,
  and remediation path.
- [x] Add build-level verification that the launcher and representative native
  test executables contain the embedded manifest declaration.

#### Acceptance Gate

- The built editor/game launcher and all targets created by
  `add_durin_test(...)` declare `longPathAware=true`.
- Host policy absence produces the selected deterministic diagnostic or gating
  behavior.
- Existing non-Windows configuration remains unaffected.
- Representative launcher and native-test builds pass through BuildTool.

### Stage 3: Prove Long-Path Behavior and Publish the Contract

- [x] Add tests that dynamically construct normalized absolute paths below,
  near, and above `MAX_PATH` without relying on checkout-root length.
- [x] Cover byte write/read, directory creation and traversal, metadata queries,
  atomic replacement, cleanup, and shader/DDC cache round trips.
- [x] Include a regression where the destination fits but the historical
  destination-derived temporary name would exceed `MAX_PATH`.
- [x] Validate paths with long totals and ordinary components separately from an
  overlong component, and assert the latter fails with an actionable diagnostic.
- [ ] Run the complete native-test preset, a full `all` build, and a
  hidden-window runtime smoke test from a deliberately long worktree path.
- [x] Move lasting manifest, host prerequisite, file-publication, and diagnostic
  rules into the owning Development and Runtime documentation.
- [ ] Record final evidence, archive this plan, and update plan indexes.

#### Acceptance Gate

- All repository-owned file operations covered by the matrix succeed beyond
  `MAX_PATH` on a configured Windows host.
- Unsupported policy, relative-path, third-party-process, and component-length
  cases fail at documented boundaries with actionable diagnostics.
- Full build, native tests, and runtime smoke validation pass from the long-path
  worktree.
- No active plan remains as the sole source of a lasting implemented contract.

## Validation Matrix

| Area | Validation |
| --- | --- |
| Manifest | Launcher and representative CoreTests/RenderCoreTests executables contain `longPathAware=true` |
| Host capability | Enabled and disabled `LongPathsEnabled` states follow the selected Stage 0 contract |
| Path lengths | Below, near, and above `MAX_PATH` using dynamically sized absolute paths |
| Components | Long total with valid components succeeds; an overlong component fails diagnostically |
| Core I/O | Create, overwrite, read, metadata query, directory traversal, rename, and remove |
| Atomicity | Initial write, replacement, concurrent writers, destination preservation, and orphan cleanup |
| Cache integration | DerivedDataCache and shader binary/reflection/manifest round trips |
| Portability | Non-Windows build and Core tests retain existing filesystem semantics |
| End to end | Full native-test preset, full `all` build, and hidden-window runtime smoke from a long worktree |

## Definition of Done

- Repository-owned Windows runtime and native-test processes embed the selected
  long-path-aware manifest.
- The required host-policy behavior is enforced or reported consistently without
  mutating the machine.
- One Core implementation owns atomic byte-file publication, and both DDC and
  shader caches use it.
- Temporary filename length is independent of destination filename length.
- Automated tests cross the traditional Windows path boundary and cover the
  affected cache workflows.
- Failures report the operation, native cause, path length, and component length.
- Lasting contracts are documented outside this plan, all acceptance gates have
  evidence, and the completed plan is archived.

## Deferred Follow-ups

- Auditing independently launched third-party tools for their own long-path
  manifests and command-line path handling.
- Supporting extended-length UNC paths if a concrete shared-cache or network
  workspace requirement appears.
- Moving derived-data roots outside the checkout for performance or workspace
  isolation reasons.
- Broad migration of unrelated direct `std::filesystem` call sites after
  evidence identifies another unsupported operation.

## Related Documentation

- [Build and Run](../Development/Build/BuildAndRun.md)
- [Build System](../Development/Build/BuildSystem.md)
- [Native C++ Tests](../Development/Build/NativeTests.md)
- [File I/O](../Runtime/Core/FileIO.md)
- [Shader Cache Hardening](Archive/2026-07/ShaderCacheHardening.md)

## Related Code

- `CMake/Project/ProjectTargets.cmake`
- `Engine/Source/Editor/DurinLauncher/CMakeLists.txt`
- `Engine/Source/Editor/DurinLauncher/Resources/DurinEditor.rc`
- `Engine/Source/Runtime/Core/Public/Misc/FileHelper.h`
- `Engine/Source/Runtime/Core/Private/Misc/FileHelper.cpp`
- `Engine/Source/Runtime/Core/Public/Misc/DerivedDataCache.h`
- `Engine/Source/Runtime/Core/Private/Misc/DerivedDataCache.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/RenderCore/Private/Shader/ShaderCacheStore.cpp`
- `Engine/Tests/Native/CoreTests/Private/FileHelperTests.cpp`
- `Engine/Tests/Native/CoreTests/Private/DerivedDataCacheTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderCacheStoreTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderCompileServiceTests.cpp`
- `CMake/Windows/DurinLongPathAware.manifest`
- `Engine/Scripts/Build/verify_windows_manifest.py`
