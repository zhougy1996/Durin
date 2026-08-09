# File I/O

Summary: Define physical-path validation, byte I/O, and atomic file-publication behavior.

Modules: Core

This document defines the repository-owned runtime contract for physical file
paths and atomic byte publication.

## Physical Path Contract

Runtime file APIs use `std::filesystem::path`. On Windows, operations that may
cross the traditional `MAX_PATH` boundary receive normalized absolute
local-drive paths. Relative paths longer than `MAX_PATH`, extended UNC paths,
shell operations, and paths passed to independently launched third-party tools
are outside this contract. Every component must remain within the underlying
filesystem's component limit, commonly 255 characters.

Windows 10 version 1607 or newer and the enabled `LongPathsEnabled` machine
policy are development prerequisites. Repository-owned runtime and native-test
executables embed `longPathAware=true`; the build verifies the declaration in
the final PE image. DurinDevTool checks host policy without changing machine state.
The setup and remediation workflow is documented in
[Build and Run](../../Development/Build/BuildAndRun.md).

Serialized identities, virtual paths, cache keys, and logged resource identity
never use Windows extended-path syntax. Platform-specific path handling remains
at the physical I/O boundary.

## Atomic Byte Publication

`FFileHelper::SaveArrayToFileAtomically()` is the shared publication primitive
for a complete byte buffer. DDC objects, shader artifacts and manifests, and
asset packages use this Core-owned path instead of implementing private
replacement logic.

Publication has these invariants:

- the destination is normalized to an absolute physical path;
- the parent directory is created before writing;
- the temporary file is an immediate sibling, keeping replacement on one
  volume;
- the temporary name has fixed size and does not repeat the destination name;
- temporary creation is exclusive and safe across threads and processes;
- bytes are written, flushed, and closed before atomic replacement;
- concurrent publishers are last-writer-wins, while readers see only a prior or
  new complete file;
- failure preserves an existing destination and performs best-effort temporary
  cleanup.

This API publishes one file. Multi-file transactions, asset move/delete
rollback, and cook ordering retain their owning subsystem's coordination rules.

## Diagnostics

Atomic publication failures identify the failed operation, native error code,
normalized destination, total path length, and longest component length. Callers
that accept an error output should preserve this diagnostic rather than replace
it with a generic write failure.

A long total path with ordinary components is supported on a configured Windows
host. An overlong component remains unsupported and should be diagnosed using
the reported component length. Missing host policy is a build prerequisite
failure, not a reason for runtime code to modify the registry or Group Policy.

## Related Documentation

- [Build and Run](../../Development/Build/BuildAndRun.md)
- [Build System](../../Development/Build/BuildSystem.md)
- [Asset Data Lifecycle and Storage](../Assets/AssetDataLifecycle.md)
- [Shader Cache](../Rendering/ShaderCache.md)
