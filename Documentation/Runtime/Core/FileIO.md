# File I/O

Summary: Define physical-path validation, byte I/O, and atomic file-publication behavior.

Modules: Core

Last reviewed: 2026-09-21

This document defines the repository-owned runtime contract for physical file
paths and atomic byte publication.

## Result-Based File Interfaces

`Misc/FilePath.h` defines `Durin::FFilePath` as an alias of
`std::filesystem::path` for physical paths, not mounted or asset identities.
`Misc/FileIO.h` introduces `Durin::FFileIO` alongside the existing `FFileHelper`
API so consumers can migrate independently. New operations return
`std::expected<T, FFileError>`; writes and exact range reads use `void` as `T`.
Errors preserve the operation, system error category/code, physical path and,
where applicable, a related path and byte range. `ToString()` formats diagnostic
text without logging. Callers own severity, recovery and final presentation.

Ordinary reads and writes do not retry. Whole-file reads return bytes only on
success; `ReadAt` can modify part of the caller's buffer before failure. Text
loads preserve exact bytes, including embedded NULs. Hashing uses bounded memory.
`FileExists` returns a successful `false` for a missing path; its result's bool
conversion means the query succeeded, not that the path exists.

Ordinary writes create parent directories and truncate existing files; failures
may leave partial bytes. Exclusive and atomic operations currently adapt the
existing publication machinery, retaining its cleanup and bounded retry policy.
Legacy helpers remain available with their existing logging and retry behavior.
DDC, JSON/YAML loading, file fingerprints, image/source reads, and package staging
and whole-file asset reads use the result-based interfaces. Their business result
types retain file diagnostics without adding lower-layer logging. Publication
transaction coordination remains unchanged. Allocation failures can still throw.
No disk-durability guarantee is added to ordinary writes.

## Synchronous Random Reads

`FFileHelper::OpenRead()` returns one uniquely owned `IFileHandle` for a
physical path. The handle captures its size from the opened native resource and
closes that resource on destruction. `ReadAt()` is an exact synchronous read:
it fills the complete destination span or fails, rejects overflow and ranges
past the captured size before issuing I/O, and accepts an empty range at any
offset through `GetSize()`. Calls expose no mutable stream cursor. The initial
contract requires callers to serialize access to one handle; consumers that
need concurrent reads open independent handles.

`FFileIoError` reports open, size-query, and read failures separately from
`FAtomicFileError`, including the normalized path and requested extent. The
handle is a low-level byte capability, not an asynchronous request, immutable
snapshot guarantee, package resource, or cancellation owner. Higher layers
bound individual reads, check their own cancellation token, and compare
before/after file metadata when stable snapshot semantics are required.

Complete-buffer loads and incremental XXH128 hashing use this handle while
retaining their existing result and bounded-memory contracts.

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

`FFileHelper::SaveArrayToNewFile()` exclusively creates a caller-selected path,
writes all bytes, flushes and closes it. It creates parent directories, preserves
existing paths on creation failure and performs best-effort cleanup after a
write, flush or close failure. It does not provide atomic visibility while
writing; package staging uses it before publication through `Commit()`.

`FFileHelper::SaveArrayToFileAtomically()` is the shared publication primitive
for a complete byte buffer. `CopyFileAtomically()` provides the same sibling
temporary, flush, and replacement contract while copying an existing file
without materializing its complete contents. DDC objects, Shader dependency
manifests, and asset packages use these Core-owned paths instead of implementing
private replacement logic.

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
DURF/DAST v9 publication uses the same primitive for each complete file;
sections and their front directory are never updated in place. A package
transaction first validates detached main/bulk output, stages the optional
headerless raw `.dbulk`, preserves the prior stable closure, publishes the
segment before the referencing `.dasset`, and publishes catalog state last.
Failure restores the prior complete closure or removes a first uncommitted
closure. The backup is removed only after the new Registry extent/digest and
Bulk Directory ranges verify. Cook uses the same segment-before-package rule,
then publishes incremental state and its CMNF manifest last. Backups and hidden
atomic temporaries are recovery state, not submitted content.

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
