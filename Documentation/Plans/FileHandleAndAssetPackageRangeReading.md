# File Handle and Asset Package Range Reading Plan

Summary: Add one Core-owned synchronous random-read file handle, then migrate DAST v7 compatibility inspection from whole-file buffers and reconstructed object streams to bounded range reads.

Last reviewed: 2026-08-30

Status: Active
Completed:

## Current Status

DAST v7 is the only supported package format. Compatibility inspection now has
a descriptor-only logical decode and avoids recursive `FValue` materialization
when no relevant deprecated route exists, but the outer path still calls
`LoadFileToArray` and the v7 codec reconstructs a complete logical ObjectStream.
Large packages therefore retain a complete package buffer plus a second logical
stream buffer before descriptor scanning begins.

Core currently exposes complete-buffer helpers and incremental hashing, but no
owned synchronous random-read handle. Engine separately owns
`FPackageResource`, whose retirement, cancellation, and asynchronous-result
contract is specific to already validated package bulk segments. This plan adds
the lower-level Core capability without merging those responsibilities.

## Goal

Provide a narrow, tested `IFileHandle` that supports exact offset reads without
exposing a mutable stream cursor, then use it to make compatibility inspection
read DAST front matter and logical metadata directly from declared ranges. The
steady-state probe must not allocate a package-sized buffer, reconstruct a
complete ObjectStream, or read field payload bytes merely to advance across
them.

## Scope

- Add a Core-owned read-only file handle and open factory for physical paths.
- Preserve existing `FFileHelper` complete-buffer and hashing behavior while
  routing suitable implementations through the handle.
- Add an Engine-private package byte-source adapter over `IFileHandle` and an
  in-memory adapter for existing byte-based tests and callers.
- Change only the compatibility codec capability to consume a seekable byte
  source; ordinary load, mutation, validation, and writing retain their current
  byte-buffer APIs until a separate consumer justifies migration.
- Parse DURF/DAST v7 front matter, section ranges, and object-stream descriptors
  without `MakeObjectStream`.
- Preserve current compatibility findings, cancellation, snapshot freshness,
  and deterministic report ordering.

## Non-Goals

- Changing the frozen DAST v7 wire format or introducing DAST v8.
- Replacing `FPackageResource`, `FBulkData`, or their asynchronous lifecycle.
- Adding a general asynchronous file API, memory mapping, writable handles, or
  scatter/gather requests.
- Replacing every `std::ifstream` or `LoadFileToArray` caller in one change.
- Making a file handle an immutable-file guarantee; compatibility snapshot
  freshness remains a higher-level before/after size and timestamp check.
- Removing the full incremental content-hash pass required by the current audit
  report. This plan removes the second full package read and package-sized
  allocations; changing fingerprint policy requires a separate decision.

## Selected Design

### Core file-handle contract

`IFileHandle` is a uniquely owned synchronous read capability created through
`FFileHelper::OpenRead`. Its minimum public surface is:

```cpp
class IFileHandle
{
public:
	virtual ~IFileHandle() = default;
	virtual auto GetSize() const -> uint64 = 0;
	virtual auto ReadAt(uint64 Offset, std::span<std::byte> Output,
		FFileIoError* OutError = nullptr) -> bool = 0;
};
```

`ReadAt` succeeds only when the complete requested range is read. It rejects
overflow and out-of-file ranges before backend I/O; an empty range is valid at
any offset through `GetSize()`. Calls do not change caller-visible cursor state.
The initial implementation does not promise concurrent calls on one handle;
callers serialize access or open independent handles. Destruction closes the
native resource. Cancellation stays above Core so every consumer can use its
own cancellation type and bound individual reads.

`FFileIoError` identifies open, size, seek/read, and close-time failure with the
physical path and native error where available. It is separate from
`FAtomicFileError`, whose operation set and recovery meaning remain specific to
atomic publication.

### Package byte-source contract

The codec does not receive a physical path. Engine-private
`IAssetPackageByteSource` exposes size plus exact `ReadAt`, with file-handle and
memory implementations. A counting decorator owns `MetadataBytesRead`; the
descriptor cursor owns `PayloadBytesSkipped`; peak metadata measures live
buffers rather than declared section sizes.

The compatibility capability receives the source and cancellation callback.
Codec resolution reads only the common DURF prefix. DAST v7 then reads bounded
front matter, validates directory arithmetic against source size, and exposes
section offset/size facts rather than section spans.

### Payload and integrity boundary

Metadata compatibility validates the DURF/DAST header, directory, required
section layout, ids, ordering, object topology, descriptor grammar, and every
payload extent. It does not verify a skipped payload's section hash, because
doing so requires reading the payload. Complete package integrity remains the
responsibility of the explicit validation/load paths. Diagnostics and
documentation must not describe metadata-only inspection as full payload
integrity validation.

If the captured reflection catalog contains a deprecated property route that
is relevant to a schema in the package, the probe may explicitly enter the
existing value-decoding fallback. That fallback must be visible in statistics:
decoded bytes count as bytes read and never as payload skipped.

## Implementation Stages

### Stage 0: Freeze semantics and caller inventory

- [ ] Inventory Core complete-buffer/hash readers, Engine package readers, and
  the existing `FPackageResource` boundary; assign only compatibility inspection
  to the first range-reading migration.
- [ ] Freeze exact-read, empty-range, overflow, short-read, mutation, error, and
  single-handle concurrency semantics.
- [ ] Record the metadata-only versus full-integrity diagnostic boundary in the
  asset-package contract before changing behavior.

#### Acceptance Gate

- The new handle has one owner and cannot be mistaken for `FPackageResource` or
  an asynchronous API.
- Compatibility corruption claims do not require bytes the probe intentionally
  skips.

### Stage 1: Add the Core read handle

Depends on Stage 0.

- [ ] Add `IFileHandle`, `FFileIoError`, and `FFileHelper::OpenRead` with a
  platform-neutral implementation behind the public interface.
- [ ] Route `LoadFileToArray` and incremental file hashing through the handle
  where doing so preserves their exact observable behavior.
- [ ] Add focused Core tests for empty files/ranges, exact and unaligned ranges,
  EOF and arithmetic rejection, repeated out-of-order reads, files larger than
  4 GiB through sparse fixtures where supported, long physical paths, and
  deterministic failure outputs.
- [ ] Prove the handle owns and closes its resource under success, early return,
  and read failure.

#### Acceptance Gate

- Existing complete-file and hash tests remain unchanged and pass through the
  new implementation.
- No Engine or asset type appears in the Core API.

### Stage 2: Introduce the package byte source

Depends on Stage 1.

- [ ] Add file-handle and memory implementations of
  `IAssetPackageByteSource`, plus a counting wrapper used by probe statistics.
- [ ] Change codec resolution for compatibility inspection to read the DURF
  prefix from the source while retaining the current span resolver for other
  capabilities.
- [ ] Change `FAssetPackageCodec::ProbeCompatibility` to accept the byte source
  and cancellation callback; keep the capability mandatory for readable codecs.
- [ ] Replace the compatibility path's `LoadFileToArray` with one opened handle
  and bounded reads, preserving stale-input checks and terminal classification.

#### Acceptance Gate

- Unsupported/corrupt prefix, open failure, truncation, cancellation, and file
  mutation retain their existing public statuses.
- Cancellation is checked before every bounded read and between descriptor
  records; no individual metadata read exceeds the selected scratch-buffer cap.

### Stage 3: Read DAST v7 metadata by range

Depends on Stage 2.

- [ ] Split v7 parsing into front-matter validation and section consumers.
  Replace `FParsedPackage::RequiredSections` spans with immutable range facts on
  the compatibility path.
- [ ] Read Public Summary, Imports, Name, Type, Schema, and Object metadata into
  bounded owned buffers; validate each range before allocation or I/O.
- [ ] Add a cached range cursor for the Value section that reads object/override
  framing, records schema id, field id, provenance, payload offset and payload
  size, then seeks over unrelated payloads.
- [ ] Remove `MakeObjectStream` and complete logical-stream allocation from v7
  compatibility inspection. Keep them for load, inspection projection,
  reference rewriting, relocation, and other value-consuming operations.
- [ ] Preserve the relevant deprecated-route fallback without making unrelated
  registered routes force payload decoding.

#### Acceptance Gate

- A fixture with a large inline payload completes compatibility inspection with
  bounded metadata memory and zero payload-range reads.
- Malformed section and payload extents fail before allocation/seek; malformed
  descriptor framing fails at a stable absolute file offset.
- `PayloadBytesSkipped` equals extents actually crossed without reads, and
  `MetadataBytesRead` equals bytes returned by the counting source.

### Stage 4: Qualify and publish the contract

Depends on Stages 1-3.

- [ ] Add cancellation tests before open, during front-matter reads, between
  objects, and while scanning a large Value section.
- [ ] Add peak-memory and byte-read regression gates using a package whose
  payload dominates its metadata.
- [ ] Run the smallest Core, AssetRegistry, package, and editor compatibility
  targets selected through the native-test registry; run broader coverage only
  if shared-reader changes cross those targets.
- [ ] Update [File I/O](../Runtime/Core/FileIO.md),
  [Asset Packages](../Runtime/Assets/AssetPackages.md), and, only where the
  ownership boundary changes, [Package Bulk Data](../Runtime/Assets/BulkData.md).
- [ ] Remove obsolete compatibility-local streaming helpers after every caller
  uses the Core handle.

#### Acceptance Gate

- Compatibility inspection owns no package-sized allocation and reconstructs no
  complete ObjectStream.
- Full integrity validation and ordinary loading remain byte-for-byte and
  failure-semantics compatible.
- Lasting ownership and metadata-only integrity rules live in Runtime
  documentation rather than only in this plan.

## Related Code

- `Engine/Source/Runtime/Core/Public/Misc/FileHelper.h`
- `Engine/Source/Runtime/Core/Private/Misc/FileHelper.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetCompatibility.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetPackageCodec.h`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetPackageV7Codec.cpp`
- `Engine/Source/Runtime/AssetRegistry/Public/AssetRegistry/ObjectStream.h`
- `Engine/Source/Runtime/AssetRegistry/Private/AssetObjectStreamReader.cpp`
- `Engine/Source/Runtime/Engine/Public/Asset/PackageResource.h`
