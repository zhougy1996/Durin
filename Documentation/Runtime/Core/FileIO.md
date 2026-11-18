# File I/O

Summary: Define physical-path validation, directory traversal, byte I/O, and atomic file-publication behavior.

Modules: Core

Last reviewed: 2026-11-18

This document defines the repository-owned runtime contract for physical file
paths and atomic byte publication.

## Result-Based File Interfaces

`Misc/FilePath.h` defines `Durin::FFilePath` as an alias of
`std::filesystem::path` for physical paths, not mounted or asset identities.
`HAL/FileManager.h` provides `IFileManager::Get()` for file and directory queries,
creation, and traversal. `Misc/FileHelper.h` provides whole-file byte operations
and the existing diagnostic read/write interfaces in `Durin::FFileHelper`.
`HAL/PlatformFile.h` exposes `FFileSystemError`, `IFileHandle`, and
`FFileOpenResult` from `GenericPlatform/GenericPlatformFile.h`. The old
`FFileError`, `EFileOperation`, and standalone file-error/handle headers are
removed. `FFileSystemError` owns a diagnostic message and a native
`std::error_code`; `GetMessage()` formats them without logging. Path and range
context belongs in the message, not separate public fields. `ToError()` converts
the diagnostic at a business boundary. Callers use native error codes, not text,
for recovery decisions.

Whole-file helpers retain value-returning `std::expected<T, FFileSystemError>`;
writes use `void`. They do not add parallel bool/output-parameter overloads.
This differs from UE's legacy synchronous helper signatures while matching the
value-or-error shape of its modern file-open interfaces.

Ordinary reads and writes do not retry. Whole-file reads return bytes only on
success; `ReadAt` can modify part of the caller's buffer before failure. Text
loads preserve exact bytes, including embedded NULs. Hashing uses bounded memory.
`IFileManager::FileExists` returns `bool` and excludes directories;
`DirectoryExists` tests directories. Both return `false` on query failure.
`FileSize` returns `-1` on failure. `MakeDirectory(Path, Tree)` creates one
directory or the parent chain and succeeds for an existing directory.
Consumers needing to distinguish absence from access failures should attempt
the actual read and inspect its native error, as Cook manifest loading does.

`LoadFileToArray(Path, FFileReadOptions)` checks the opened handle's size before
allocating. `MaximumBytes` bounds the allocation; optional `ExpectedBytes`
rejects a changed size. Both return native error codes: `file_too_large` for a
limit and `message_size` for a size mismatch (checked first). Open failures preserve native causes; post-open read failures report contextual
I/O errors. Content hash verification, aggregate closure budgets and
cancellation remain caller-owned; a successful size check is not proof of an
immutable file generation.

Ordinary writes create parent directories and truncate existing files; failures
may leave partial bytes. Exclusive and atomic operations directly return the
same result type, retaining their cleanup and bounded retry policy.
DDC, JSON/YAML loading, file fingerprints, image/source reads, and package staging
and whole-file asset reads use the result-based interfaces. Their business result
types retain file diagnostics without adding lower-layer logging. Publication
transaction coordination remains unchanged. Allocation failures can still throw.
No disk-durability guarantee is added to ordinary writes.

JSON/YAML `Parse` returns `expected<void, FJsonParseError/FYamlParseError>`.
`LoadFromFile` accepts `FFilePath` and returns a load error whose `Cause` is a
variant of `FFileSystemError` and the corresponding parse error. Callers can inspect
the native missing-file code without parsing diagnostic text; The load error's `ToString()`
formats either cause, including source coordinates for parse failures. A read
failure preserves the previous document; a parse failure invalidates it.

`FFileFingerprintCache::Get` returns `expected<FFileFingerprint, FFileSystemError>`;
successful cached and freshly computed fingerprints have the same value shape.
Its reuse API returns `expected<EFileFingerprintReuseStatus, FFileSystemError>`:
current and stale are successful inspections, while failed checks retain their
native cause. Source capture returns
`expected<FEncodedSourceSnapshot, FEncodedSourceError>` and
retains size, timestamp, limit and change-detection checks. Image decoding uses
the value-returning contract in [Core Image Codec](ImageCodec.md). These wrappers
do not log. Cache business statuses and package transaction results retain their
existing recovery and publication semantics. Package generation failures carry
either a file error or a bulk validation failure. Prepared package errors retain
one variant cause rather than independent optional causes. Cook input discovery
retains one structured cause in its result and formats it at presentation time;
the coordinator does not maintain a second diagnostic copy.

Completed package range reads return
`expected<FSharedByteBuffer, FPackageResourceReadError>`. There is no pending
result value: request readiness, binding, cancellation and waiting remain on
`FPackageResourceRequest`. Failure classification lives only in the error;
`GetPackageResourceReadStatus` derives a classification when a business adapter
needs it. Rejected waits remain caller-local failures and do not change the
eventual request outcome.

## Directory Traversal

`IFileManager::Get().IterateDirectory` enumerates immediate children;
`IterateDirectoryRecursively` also enumerates accepted directories. Both accept
`IPlatformFile::FDirectoryVisitor&` or a synchronous callable and return `bool`.
Names, ownership, and simple return shapes follow UE's file interfaces;
physical paths remain `FFilePath`, and callables use `std::function`.
The manager delegates to `IPlatformFile::GetPlatformPhysical()`. The platform
interface owns physical queries and single-directory enumeration; its shared
recursive implementation uses the single-directory operation.

`ShouldVisitLeafPathname(std::string_view)` runs before entry metadata queries.
Returning `false` skips the entry and its subtree. `Visit(const FFilePath&, bool)`
returns `true` to continue or `false` to stop the whole traversal. The root itself
is not visited. Callbacks run on the calling thread with normalized absolute
paths; enumeration order is unspecified. Dot entries, extensions, and generated
folder exclusions remain caller policy. There is no optional-root flag; callers
such as shader policy scans decide which roots are optional.

A missing or non-directory root returns `false`; visitor termination also returns
`false`. Existing directories that cannot be enumerated are skipped when their
existence can be established. Disappearing or inaccessible descendant entries
and iterator failures stop only the affected entry/directory, allowing remaining
directories to be scanned. `true` means traversal was not stopped, not proof of
an exhaustive filesystem snapshot. Unlike the previous helper API, Core does
not collect an error vector or prioritize errors over visitor termination.

Traversal retains Durin's no-link policy: symbolic links and Windows reparse
points, including junctions, are skipped without callbacks or recursion, also
when supplied as the root. This is not full UE link-behavior parity. Ancestors
of the supplied root are not checked; callers requiring an ancestor policy must
validate them. This API is not a security boundary.

The current manager is a focused subset of UE's API. Archive readers/writers,
copy/move/delete, stat visitors, and a replaceable platform-file chain are not
introduced by this change. Existing random reads and atomic publication remain
under their documented contracts below.

## Mounted Path Results

`FMountPaths` lookup, resolution/classification, and dependency checks return
`expected<FMountLookup/FMountPath/FMountDependency, FMountPathError>`.
Success values contain only resolved path or dependency data. Failures retain
`EMountPathError`, requested paths, a discovered mount when available, and the
native `std::error_code`. Mount pointers retain the registry lifetime on both
branches. Missing files remain errors only when `RequireFile` was requested.
`ToString` lives in `Paths.cpp`; import destination reports can retain mount
identity even when its physical root is unavailable.

Startup registry publication/default configuration still exposes its existing
boolean and textual diagnostic adapter. Its callers display configuration
errors; they do not branch on generated strings.

## Platform Read/Write Handles

`IPlatformFile::OpenRead(Path, EOpenReadFlags)` and
`OpenWrite(Path, EOpenWriteFlags)` return `FFileOpenResult`, an expected uniquely
owned `IFileHandle` or `FFileSystemError`. There is no `FFileHelper::OpenRead`
forwarder or legacy bool-argument opening overload. Destroying the handle closes
its native resource. Opens do not create parent directories; whole-file save
helpers retain that responsibility.

Read flags are `None`, `AllowWrite`, `AllowDelete`, and `NoBuffering`; write flags
are `None`, `AllowRead`, and `Append`. Windows enforces sharing flags.
`NoBuffering` uses native Windows alignment requirements and currently reports
`operation_not_supported` on other platforms. Migrated readers and whole-file
helpers explicitly retain their previous write/delete sharing.

The handle follows UE names and parameter order: `Tell`, `Seek`, `SeekFromEnd`,
`Size`, `Read`, `ReadAt(Destination, Count, Offset)`, `Write`, `Flush`, and
`Truncate`. Positions and sizes use `int64`; operations return `bool`, with `-1`
for failed position/size queries. `Size()` queries the open file, not a path or
cached snapshot. Append starts at the current end without truncating the file;
it is not a concurrent append transaction. `AllowRead` permits reads through a
write handle. Ordinary writes do not promise durable storage.

Reads are exact; failures can modify part of the buffer. `ReadAt` rejects invalid
ranges before reading and restores the cursor. Empty reads at EOF succeed.
Callers serialize handle access; no thread-safety or immutable snapshot guarantee
is added. Post-open boolean failures are translated to contextual I/O errors by
whole-file or package adapters; they do not carry the removed operation enum.
`Flush(true)` requests native full flushing. Exclusive creation and atomic
publication keep their stronger flush/close and cleanup rules below.

Complete-buffer loads and bounded-memory hashing use the same platform handles;
they do not maintain a second native reader or stdio writer implementation.

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

Atomic publication errors include native causes and relevant paths in their
messages. Callers preserve that diagnostic rather than substituting a generic
write failure. Temporary-file ownership is explicit internal state; cleanup and
retry decisions do not depend on a diagnostic operation identifier. Only a file
created by the current operation is eligible for failure cleanup.

Long total paths with ordinary components are supported on configured Windows
hosts; overlong individual components remain unsupported. File diagnostics no
longer calculate path-length metrics for every failure. Missing host policy is
a build prerequisite failure, not a reason to modify system configuration.

## Related Documentation

- [Build and Run](../../Development/Build/BuildAndRun.md)
- [Build System](../../Development/Build/BuildSystem.md)
- [Asset Data Lifecycle and Storage](../Assets/Packages/AssetDataLifecycle.md)
- [Shader Cache](../Rendering/Shaders/ShaderCache.md)
