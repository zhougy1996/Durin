# Derived Data Cache Storage

Summary: Specify Durin CB manifests, versioned value attachments, selective local cache queries, integrity checks, byte budgets, and retain-only publication.

Modules: DerivedDataCache

Last reviewed: 2026-10-11

Build ownership and completion belong to the [build protocol](DerivedDataBuild.md).
This contract owns persistent storage, not asset assembly policy.

## Manifests, values, and query limits

Persistent cache requests use `records/v4/<bucket>/<prefix>/<action-hash>.bin`
manifests and `values/v1/Content/<prefix>/<raw-hash>.bin` content blobs. Values
share storage across records and buckets. `DDBM` schema 2 wraps a canonical Durin
CB object in the 16-byte binary header and an XXH3-128 integrity trailer.
The CB object has exactly `Bucket`, binary `Key`, `OutputSchema`, unsigned
`OutputVersion`, and `Values`, `Metadata`, `Messages` arrays. Each value object
contains object-ID `Id`, unsigned `RawSize`, and `Attachment`; the attachment
contains `Version=1`, `Algorithm="xxh3-128"`, `Namespace="values/v1"`, and binary
16-byte `Hash` over raw content (low then high 64-bit word, little endian).
Metadata entries contain `Id` and nested `Object`; messages contain unsigned
`Severity` and `Text`. Unknown fields/types/identity versions, non-canonical
tables, and logical limits are rejected. Nested metadata shares manifest storage;
logical metadata budgets exclude the enclosing CB field name. The envelope is
bounded to 8 MiB of CB, depth 40, and 700,000 fields; family metadata keeps its
own 4 MiB/depth-32/4,096-field limits. It contains no value
payloads. Decoding validates ordering, IDs, schema, metadata, messages, and total
logical raw output limits before requesting blobs. Full-record Get resolves and
validates every value before returning; missing required blobs are rejected and
follow the normal build fallback path.
`records/v3` is cold-invalidated without migration or deletion; existing value
blobs remain reusable. This is a Durin format, with no UE wire-compatibility claim.
`FCacheBlobIdentity` versions blob addressing independently of action/source
identity and codec envelopes. Version 1 is supported only for trusted local
storage: its non-cryptographic hashes detect accidental corruption. A shared or
untrusted backend requires a new cryptographic digest version and namespace,
including verification against raw bytes, before it can be admitted. Unsupported
attachment versions never alias a current path or alter existing action hashes.

Synchronous Get defaults to `FullRecord`. `MetadataOnly` returns the manifest,
including all value descriptors, without opening blobs. `SelectedValues` accepts
unique non-null required IDs in any order, resolves only those blobs, and leaves
other values as references. An empty selection is equivalent to metadata-only.
Unknown required IDs or missing selected blobs return `MissingValue`; an absent
manifest remains a normal miss. Invalid or duplicate selections fail before I/O.
All modes validate the entire manifest against logical output limits; selection
narrows physical reads and resident data, not the output schema's validity bounds.
Partial records cannot convert to complete build outputs. Build execution always
requests a full record and retains its existing rejected-record rebuild fallback.

Get cooperatively checks borrowed cancellation at entry, between manifest/blob
reads, and after decoding. It returns `Canceled` without publishing a partial
result; a synchronous filesystem read or Zstd call already in progress finishes
before the next check. Optional borrowed `FCacheQueryStats` receives successful
read bytes, validated raw bytes, values read, elapsed nanoseconds, and peak
accounted payload/codec storage on success, miss, cancellation, or error. The
working-set metric includes estimated Zstd context bytes and excludes descriptor,
allocator, and other process memory; it is not RSS. Encoded and working-set
budgets apply to the manifest plus requested values only.

Engine's editor-only `ReadTexture2DCachedMips` takes an opaque cache key, selected
mip indices, target, budgets, and cancellation. It performs a cache-only selected
value query with no source resolver or build fallback, validates the complete
Texture2D metadata/layout and deterministic error messages, and returns selected
immutable mip buffers with their original indices, format, and original mip count.
Indices are unique, bounded, and returned in ascending order. The result is a
separate `FTexture2DCachedMips`, never an incomplete `FTexturePlatformData` or a
successful partial build output. Missing required mips and target/schema failures
are explicit errors. Blob encoding and storage namespaces remain unchanged.

Each immutable `DDBV` blob schema 1 stores codec format (raw or Zstd level 3), raw
XXH3-128 identity, raw size, encoded payload size, payload, and an envelope hash.
Small, incompressible, or compression-workspace-limited values may use the raw
format. Envelope and frame bounds are checked before inflation; raw identity is
checked afterward, even when the envelope hash is valid. Raw values retain views
into blob allocations; compressed values own independently inflated allocations.
An explicit empty value is resident and has a blob with zero raw payload bytes.
Codec format and parameters do not participate in action identity.

The filesystem backend atomically stores bounded bytes with path containment.
Put publishes every complete blob before atomically publishing the manifest.
Concurrent same-content writers may replace a blob with another valid encoding
of identical raw content; Put also repairs damaged blobs by atomic replacement.
A failed write never publishes a manifest referencing that incomplete write.
Existing manifests remain visible when a replacement fails. Already published
blobs from a failed request may remain orphaned.

Raw, encoded, and payload/codec working-set limits are separate. Raw limits count
logical output values, metadata, and message bytes. Encoded limits count the
manifest plus each requested blob (conservatively counting duplicate references).
Persistence workspace counts distinct retained source buffer capacities, resident
messages, the manifest, candidate/final encoding buffers, and estimated Zstd
context memory. Manifest encoding preflights eight times its bounded size
estimate for CB child/container copies, independently of its encoded-byte bound.
Read workspace counts the retained manifest, already resolved
values, the current encoded blob, inflation storage, and decompression context.
These byte budgets exclude descriptor/container/control-block overhead and are
not a process RSS limit. Build policies pass their raw and working-set budgets
through to cache requests. Store failures retain a successful local output.

Automatic reclamation is disabled: neither Get nor Put deletes blobs or
manifests. This retain-only policy protects live references and concurrent
readers/writers. A future collector must use exclusive ownership or a publication
epoch/lease protocol covering the blob-before-manifest interval, retained readers,
and the complete set of live manifests; a naive reference scan followed by deletion
is unsafe. Until such a collector is implemented and tested, cache clearing must
occur only with all readers, writers, and editors stopped. Orphan-retention,
publication-failure, and concurrent read/write tests cover the current policy.

Legacy namespaces are cold misses with no compatibility read or rewrite path.
The schema-2 record codec remains available for explicit in-memory codec users
and tests; normal cache Get/Put no longer uses whole-record compression.
