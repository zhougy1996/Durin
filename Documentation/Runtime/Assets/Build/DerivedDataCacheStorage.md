# Derived Data Cache Storage

Summary: Specify Durin CB manifests, versioned value attachments, selective local cache queries, integrity checks, byte budgets, retain-only publication, bounded diagnostics, and offline maintenance.

Modules: DerivedDataCache

Last reviewed: 2026-10-30

Build ownership and completion belong to the [build protocol](DerivedDataBuild.md).
This contract owns persistent storage, not asset assembly policy.

## Manifests, values, and query limits

Public cache requests use `ICache::Get`/`Put` with an owner and one status-bearing
response per item. Synchronous hit/miss/error results, cancellation predicates,
per-query statistics, and dispatch handles are private backend concerns.
Public requests do not carry storage budgets: backend configuration supplies
`FCacheResourceLimits` for record validation, encoded bytes, and codec working set.
These ceilings apply to both Get and Put; build-owned product validation remains
separate from cache policy.

`DerivedDataCacheRecord.h` defines immutable `FCacheRecord`, its builder, and
`FCacheRecordLimits` independently of Build. A record contains a key, one opaque
Compact Binary metadata object, and ID-addressed values. It exposes no producer
schema, build messages, Build conversion, or wire-format methods. Private backend requests
use configured record limits rather than `FBuildOutputLimits`.

Persistent requests use `records/v5/<bucket>/<prefix>/<action-hash>.bin` manifests
and `values/v1/Content/<prefix>/<raw-hash>.bin` content blobs. Values share storage
across records and buckets. Private `CacheRecordStorage.h` owns `DDBM` schema 3:
a canonical CB object inside a 16-byte binary header and XXH3-128 trailer. The
object has exactly `Bucket`, binary `Key`, opaque object `Meta`, and `Values`.
Each value contains object-ID `Id`, unsigned `RawSize`, and `Attachment`; its
attachment contains `Version=1`, `Algorithm="xxh3-128"`, `Namespace="values/v1"`,
and a binary 16-byte raw-content hash. Unknown fields/types/identity versions,
non-canonical value tables, and record limits are rejected before blob reads.
The cache never interprets fields inside `Meta`.

Metadata shares manifest storage; logical metadata budgets exclude its enclosing
CB field name. A manifest is bounded to 8 MiB of CB, depth 40, and 700,000 fields.
Full-record Get resolves and validates every value; missing required blobs follow
the normal build fallback path. Older record namespaces, including `records/v4`,
are cold misses without migration or deletion; value blobs remain reusable.
This is a Durin format, with no UE wire-compatibility claim.
`FCacheBlobIdentity` versions blob addressing independently of action/source
identity and codec envelopes. Version 1 is supported only for trusted local
storage: its non-cryptographic hashes detect accidental corruption. A shared or
untrusted backend requires a new cryptographic digest version and namespace,
including verification against raw bytes, before it can be admitted. Unsupported
attachment versions never alias a current path or alter existing action hashes.

Both public and private Get requests own an immutable `FCacheRecordPolicy`.
`DerivedDataCachePolicy.h` provides the supported local read-policy subset:
`None`, `QueryLocal`/`Query`, `Default` (query), and `SkipData`. A uniform default
queries the manifest and fetches every value. The builder accepts a base policy
and ID-specific overrides, sorts those overrides, drops redundant entries, and
becomes unusable after Build. Null/duplicate IDs, unsupported flags and builder
reuse are programming errors. Policies own their overrides and can be copied
across asynchronous request lifetimes without retaining caller arrays.

| Policy | Get behavior |
| --- | --- |
| Default | Fetch all values and return metadata. |
| Query with SkipData | Return metadata and descriptors after checking each queried blob exists; do not read or decompress its payload. |
| None plus Default overrides | Fetch the overridden values that occur in the record; leave other values as references. |
| Default plus a None override | Fetch other values and ignore that value's data availability. |
| Default plus a Query/SkipData override | Require that value's blob to exist, but leave its data as a reference. |
| Uniform None | Return a normal miss without storage access. |

Record query permission is combined from the base and value policies. Overrides
apply to values present in the manifest; an override is not a list of required
IDs. A typed asset consumer checks its own required values after querying.
Missing queried blobs return `MissingValue` without a partial successful record;
a missing manifest remains a normal miss. Presence checks use the same file-type
and bucket-containment checks as payload reads. SkipData does not validate blob
contents. Every queried manifest is validated against generic record limits,
even when most data is skipped. A record containing unresolved references cannot
convert to a complete build output. Build execution uses the default policy and
retains its rejected-record rebuild fallback. Put still accepts complete records;
this data-selection change does not alter persistence formats or replacement rules.

Get cooperatively checks borrowed cancellation at entry, between manifest/blob
reads, and after decoding. It returns `Canceled` without publishing a partial
result; a synchronous filesystem read or Zstd call already in progress finishes
before the next check. Optional borrowed `FCacheQueryStats` receives successful
read bytes, validated raw bytes, values read, elapsed nanoseconds, and peak
accounted payload/codec storage on success, miss, cancellation, or error. The
working-set metric includes estimated Zstd context bytes and excludes descriptor,
allocator, and other process memory; it is not RSS. Encoded and working-set
budgets apply to the manifest plus distinct requested blob identities and raw
sizes only. Repeated value IDs that reference the same supported identity and
size share one immutable decoded allocation and perform one read/decode per
request, including empty values. `ValuesRead` counts distinct requested blobs;
`DecodedBytes` counts raw bytes actually validated once for each such blob.
Logical output limits still count every value descriptor, including repeated
references and unselected values. Raw views retain their encoded allocation;
compressed values retain one inflated allocation per distinct identity/size.

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
Put visits each distinct versioned identity/raw-size pair once. Before encoding,
it reads existing content under the request's remaining encoded/workspace bounds
and validates both its envelope and raw identity. Valid content is reused without
compression or blob publication. Corrupt or absent content is encoded and atomically
published; a stored encoding that exceeds the remaining encoded bound may be
replaced by a bounded encoding of the same content. Concurrent same-content writers
may replace a blob with another valid encoding of identical raw content; damaged
content retains the same atomic repair path.
A failed write never publishes a manifest referencing that incomplete write.
Existing manifests remain visible when a replacement fails. Already published
blobs from a failed request may remain orphaned.

Raw, encoded, and payload/codec working-set limits are separate. Raw limits count
logical value bytes and the opaque metadata object. Encoded limits count the
manifest plus each distinct requested blob identity/raw-size pair.
Persistence workspace counts distinct retained source buffer capacities, the opaque metadata buffer, the manifest, candidate/final encoding buffers, and estimated Zstd
context memory. Manifest encoding preflights eight times its bounded size
estimate for CB child/container copies, independently of its encoded-byte bound.
Read workspace counts the retained manifest, already resolved
values, the current encoded blob, inflation storage, and decompression context.
These byte budgets exclude descriptor/container/control-block overhead and are
not a process RSS limit. Optional synchronous `FCacheStoreStats` records successful
read/write bytes, per-identity encoding/reuse/publication counts, and the peak
retained payload/codec workspace after manifest construction. Its peak excludes
transient CB assembly; the separate eight-times-estimate admission preflight still
bounds that assembly. Async Put ignores borrowed store-statistics pointers.
Build policies pass their raw and working-set budgets
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
The historical schema-2 whole-record codec is available only through the private
`CacheRecordFixtureCodec.h` test fixture boundary. Public record APIs expose only generic records and values. Build interpretation lives
in its private adapter; manifests, compression, attachment namespace/versioning,
and historical fixture formats remain private.

## Bounded diagnostics

`GetCacheDiagnostics` snapshots process-local cumulative local-backend counters.
Totals cover structured Get hits/misses, corrupt/missing records, backend failures,
cancellation, invalid/over-budget requests, Put/store failures, physical read/write
bytes, decoded and encoding-input bytes, encoding/reuse counts, and summed Get/Put
nanoseconds. Build continuations separately count rejected schemas/records and
fallback attempts when local production is allowed. Async dispatch counts starts,
summed admission-to-start queue nanoseconds, and capacity rejection. Oversized
batches count one rejection without iterating unbounded caller input. Injected test
backends own their physical I/O metrics; dispatch/fallback observations still apply.

The table retains at most 64 bucket names (each already bounded by bucket validation)
and one overflow aggregate. Snapshots copy bounded values; there are no per-key
histories, latency histograms, payload samples, storage scans, or worker threads.
Diagnostics do not participate in action keys, persisted records, cache policy, or
completion status. Observation allocation failure is ignored without changing a
cache result. Counts/latencies are cumulative sums, not percentiles, and do not
measure process RSS or whole-editor elapsed time.

`LogCacheDiagnostics` writes a bounded `DDCStats` summary. The Editor exit path and
the offline asset/Cook host call it after draining work and before logger shutdown.
Cook logs are under its output directory's `Logs/`; JSON Cook stdout is unchanged.
Other hosts may snapshot or log explicitly at a drained workload boundary. Offline
inspect reports encoded file-size totals separately; it does not read another
process's in-memory statistics or include unrelated root contents/directory blocks.

## Offline ownership and maintenance

Normal structured Get/Put and private storage access acquire shared nonblocking OS
ownership of `<root>/.durin-ddc-v1.lock`. The configured root's shared lease remains
owned by the process while idle. The next cache access after a drained root change
replaces that process lease; request-held leases span the complete record transaction.
Each record Get/Put acquires one lease. All manifest/blob paths and in-flight read
coalescing identities use that lease's canonical root without reacquiring ownership
or consulting the configured root for each file. Independent private storage calls
acquire their own lease at entry.
In particular, a Put cannot expose its blob-before-manifest interval to maintenance.
The persistent lock file is not a PID marker and is never unlinked by maintenance;
OS handle close/process termination releases ownership without stale-lock recovery.
All participating users must implement this protocol. Stop older binaries and
external/manual writers before maintenance; removing/replacing the ownership file
or changing the configured root concurrently with cache work is unsupported.

`MaintainCacheStorage` and the Core/DDC-only `DurinCacheTool` use an explicit absolute
root, independent of process cache configuration. DevTool exposes `cache inspect`,
`cache verify`, and `cache clear` with explicit `--root`; all require exclusive
nonblocking ownership and refuse live roots or competing maintenance. Inspection
may initialize the ownership file on an existing root. Clear requires that marker
from a cache user or prior inspection. No active operation is canceled or waited on.

- Inspect reports `records/` and `values/` file counts/encoded sizes, including
  legacy namespaces; unknown root siblings are counted but never traversed.
- Verify validates current `records/v5` manifests, required blob identities/sizes,
  and every current `values/v1` blob, including orphans. It checks raw content as
  well as envelopes. Missing content, corruption, and unsupported files are explicit
  counts; unsupported/legacy files are preserved and do not produce a valid report.
- Clear preflights the complete recognized tree, refuses links/special files, and
  removes record files/directories before any value files/directories. It preserves
  unknown siblings and the ownership file. Cancellation or crash leaves cold misses
  or retained blobs; rerunning clear is safe. It never follows a recursive delete.

Scans default to 100,000 entries (hard maximum 1,000,000). Verification defaults to
256 MiB per input, 1 GiB total payload reads, and 512 MiB retained payload/codec
workspace; limits can be explicitly increased through command options. Manifest
retention is charged while validating referenced blobs. Entry metadata is bounded
by the entry-count limit; at most 32 relative-path diagnostics are retained.
Limits, filesystem errors and cancellation produce an incomplete/error response,
never a successful partial verification. Filesystem reads/codecs already running
finish before the next cancellation check. Only explicit clear deletes content;
there is no automatic, online, age/quota, or selective orphan reclamation.

For a real workload baseline, keep the same project, inputs, target and DDC root;
record its initial offline inventory, run a cold Editor or Cook workload, retain
its drained `DDCStats` log, repeat warm in a new process, then inspect after both
processes exit. For Cook, reuse one output directory and use `--no-incremental`
when measuring DDC rather than skipping package preparation through Cook hits.
Do not clear an existing user cache for measurement: use a fresh output/cache root.
Summed DDC/queue times can overlap and must not be treated as wall-clock speedup.
See [command interface](../../../Development/Tooling/DurinDevTool.md#offline-local-cache-maintenance)
for invocation and report status.
