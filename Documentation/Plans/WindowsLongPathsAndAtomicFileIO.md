# Windows Long Paths and Atomic File I/O Plan

Last reviewed: 2026-07-26

## Current Status

Active. The immediate shader-cache failure was mitigated by commit `4365c0d7`,
which stopped temporary artifact names from repeating the complete destination
filename. The repository does not yet embed a `longPathAware` manifest, has no
declared `LongPathsEnabled` development prerequisite, and retains separate
atomic-write implementations in Core and RenderCore.

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

- `FFileHelper` already owns common byte-file reads and writes but does not own
  atomic publication or structured filesystem diagnostics.
- `DerivedDataCache::WriteFileAtomically` appends `.tmp` to the complete
  destination and calls `MoveFileExW` directly on Windows.
- `ShaderCacheStore` independently creates sibling temporary files and performs
  atomic replacement. Commit `4365c0d7` shortened those temporary filenames,
  resolving the observed worktree-sensitive shader test failure.
- `add_durin_test(...)` is the central constructor for native-test executables,
  while `DurinLauncher` is created separately.
- No repository-owned application manifest currently declares
  `longPathAware`.
- Native tests have isolated `DURIN_TEST_WORK_DIR` roots, but current path tests
  do not dynamically cross the traditional Windows total-path boundary.
- Windows long-path behavior requires both a process manifest declaration and an
  enabled host policy; the repository currently validates neither.

## Implementation Stages

### Stage 0: Lock the Windows Capability Contract

- [ ] Inventory every repository-owned Windows executable target and every
  direct atomic-replacement implementation.
- [ ] Decide whether `LongPathsEnabled` is a hard Windows development
  prerequisite or a reported optional capability with conditional tests.
- [ ] Verify the supported MSVC standard library and Win32 operations used by
  `std::filesystem`, streams, directory traversal, and atomic replacement under
  paths longer than `MAX_PATH`.
- [ ] Select one manifest attachment mechanism that covers generated native-test
  targets and the separately declared launcher without per-target duplication.
- [ ] Record any unsupported path forms, Windows versions, or tool boundaries
  before implementation begins.

#### Acceptance Gate

- The host-policy behavior, executable target inventory, supported path forms,
  and manifest attachment point are explicit and have no conflicting open
  alternatives.
- A minimal probe demonstrates the expected behavior both below and above
  `MAX_PATH`, or records the exact standard-library operation requiring a
  centralized Windows extended-path adapter.

### Stage 1: Centralize Atomic File Publication in Core

- [ ] Add a Core-owned byte publication API with same-directory fixed-length
  temporary naming, atomic replacement, and best-effort cleanup.
- [ ] Make temporary-name uniqueness safe for concurrent threads and separate
  processes without including the destination filename.
- [ ] Return structured failure information sufficient to report operation,
  native error code, path length, and component length.
- [ ] Migrate `DerivedDataCache::WriteFileAtomically` to the shared
  implementation while preserving its public compatibility contract.
- [ ] Migrate shader binary, reflection, and dependency-manifest publication to
  the shared implementation and remove its private replacement logic.
- [ ] Add Core unit tests for initial publication, replacement, concurrent
  writers, failed replacement cleanup, and destination preservation.

#### Acceptance Gate

- CoreTests prove that readers never receive partial bytes and failed
  publication does not damage an existing destination.
- RenderCore no longer owns private temporary-path or atomic-replacement code.
- Atomic temporary filename length is constant with respect to destination
  filename length.
- CoreTests and RenderCoreTests pass through the repository BuildTool workflow.

### Stage 2: Enable Long-Path-Aware Windows Processes

- [ ] Add a repository-owned Windows application manifest containing
  `longPathAware=true`.
- [ ] Attach the manifest through the selected CMake helper to every native-test
  executable and to `DurinLauncher`.
- [ ] Add a configure-time or BuildTool preflight check for
  `LongPathsEnabled`, following the Stage 0 decision without changing machine
  state.
- [ ] Report missing policy with the exact Windows setting, affected capability,
  and remediation path.
- [ ] Add build-level verification that the launcher and representative native
  test executables contain the embedded manifest declaration.

#### Acceptance Gate

- The built editor/game launcher and all targets created by
  `add_durin_test(...)` declare `longPathAware=true`.
- Host policy absence produces the selected deterministic diagnostic or gating
  behavior.
- Existing non-Windows configuration remains unaffected.
- Representative launcher and native-test builds pass through BuildTool.

### Stage 3: Prove Long-Path Behavior and Publish the Contract

- [ ] Add tests that dynamically construct normalized absolute paths below,
  near, and above `MAX_PATH` without relying on checkout-root length.
- [ ] Cover byte write/read, directory creation and traversal, metadata queries,
  atomic replacement, cleanup, and shader/DDC cache round trips.
- [ ] Include a regression where the destination fits but the historical
  destination-derived temporary name would exceed `MAX_PATH`.
- [ ] Validate paths with long totals and ordinary components separately from an
  overlong component, and assert the latter fails with an actionable diagnostic.
- [ ] Run the complete native-test preset, a full `all` build, and a
  hidden-window runtime smoke test from a deliberately long worktree path.
- [ ] Move lasting manifest, host prerequisite, file-publication, and diagnostic
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
- [Shader Cache Hardening](Archive/2026-07/ShaderCacheHardening.md)

## Related Code

- `CMake/Project/ProjectTargets.cmake`
- `Engine/Source/Editor/DurinLauncher/CMakeLists.txt`
- `Engine/Source/Editor/DurinLauncher/Resources/DurinEditor.rc`
- `Engine/Source/Runtime/Core/Public/Misc/FileHelper.h`
- `Engine/Source/Runtime/Core/Private/Misc/FileHelper.cpp`
- `Engine/Source/Runtime/Core/Public/Misc/DerivedDataCache.h`
- `Engine/Source/Runtime/Core/Private/Misc/DerivedDataCache.cpp`
- `Engine/Source/Runtime/RenderCore/Private/Shader/ShaderCacheStore.cpp`
- `Engine/Tests/Native/CoreTests/Private/DerivedDataCacheTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderCacheStoreTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderCompileServiceTests.cpp`
