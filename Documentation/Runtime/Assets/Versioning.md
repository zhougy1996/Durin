# Versioning

Summary: Define serialized type, object, package, and compatibility version contracts.

Modules: Core, CoreDObject, Engine

Last reviewed: 2026-08-30

Durin's engine release version is defined once in `Engine/Build/Build.version`. The current development version is `0.1.0-dev`. CMake validates that file, exposes the numeric core as the workspace project version, and generates the header consumed by Core's public version API.

Runtime code should use `GetEngineVersion()` for numeric components and `GetEngineVersionString()` for display. Do not duplicate the engine version in module sources, window titles, API integration metadata, or packaging scripts.

## Independent Version Domains

The engine release version identifies a Durin release for users, logs, diagnostics, source tags, and distributable binaries. It does not define serialization compatibility. Formats that evolve independently keep their own versions, including:

- `.dasset` package files
- transient object graphs
- DurinHeaderTool schemas and tool output
- shader metadata, cache records, and variant keys
- editor layout and user-setting schemas
- project descriptor schemas

Changing the engine release version alone must not rewrite assets or invalidate caches. A format version changes only when that format's reader or writer contract changes. A saved-by engine version may be added later for diagnostics, but readers must make compatibility decisions from the relevant format and custom schema versions.

## Archive Version Context

`FArchiveVersionContext` carries named format versions separately from optional
GUID-keyed custom versions. Object-graph Archives report object-graph v2;
authored-package Archives report the actual source format, DAST v6 or v7.
Ordinary and bundle saves report and emit v7. Property snapshots are
process-local and unversioned. Struct
`PostDeserialize` receives the Archive purpose and source format version instead
of deriving compatibility from the engine release number. During authored
loading it also receives the complete source custom-version context, allowing a
detached reflected struct to perform the same bounded conversion as an
object's pre-publication `PostLoad`.

The DAST logical object stream inside v6/v7 owns the package-local custom-version table, canonical GUID ordering,
discovery freeze, reader bounds, unknown-version rejection, and exact retained
closure/payload semantics.

Reflected schema evolution uses these existing GUID-keyed records directly.
An explicitly annotated `_DEPRECATED` route owns one stable domain GUID, an
exclusive `DeprecatedBefore` bound, and its domain's `LatestVersion`. Current
saves discover and emit `LatestVersion` automatically. Missing tags resolve to
the domain's `BeforeCustomVersionWasAdded` value (`-1` by convention); they do
not resolve to the current version. A source value above the runtime domain's
`LatestVersion` fails before object publication. Engine release version changes
are neither required nor consulted.

Version registration is part of serializer discovery/emission parity. A format
or custom version first observed during emission is a late-discovery failure;
unsupported versions are rejected before destination mutation or output
publication.

## Authored Package Version Policy

Supported readers and the ordinary writer are separate policies backed by one
private, statically composed codec table. Each codec has an immutable string
identity, permanent nonzero `FormatId`, wire version, and complete reader,
writer, and mutation capability set. Shared code validates the `DURF` preamble
through Core's bounded two-phase envelope validation and Engine's explicit
immutable descriptor registry. Once that registry has selected DAST, the
package codec table resolves only its format version. It fails closed before
codec parsing when no reader exists. Header
reads, validation, inspection, compatibility probes,
reference projection, live loading, serialization, relocation, reference
rewrite, redirector creation, and cook canonicalization do not branch on a
version enum. The repository registers bounded v6 and v7 readers and selects
only v7 for ordinary writing and mutation. Read-only entrypoints never select a
writer or dirty authored content. Legacy DAST prefixes, including v4 and v5,
are unsupported and fail before object-stream parsing or publication. DAST v6
is a canonical-resave input rather than an ordinary output.

DURF selects DAST object packages and the read-only legacy DABK v2/DBLK v2
compatibility containers by permanent GUID. DAST v7 ordinary writers place
authored and cooked BulkData in package-owned raw `.dbulk` segments; raw
segments are not DURF formats. Their versions describe storage grammar only.
The reflected asset slot selects and validates its payload schema. DDC/Cook
keys advance with family schema or producer changes so old raw values cannot
enter a new decoder.

A frozen writer constructs its Archive context from its own codec identity.
The v7 writer therefore always reports DAST v7 to serializers and emits v7. The
reader-policy cache identity is an explicit generation, not a wire-version
alias; changing the supported-reader contract requires changing that identity.

Registry and reference-cache fingerprints include the source DAST version.
Changing package bytes or versions invalidates the corresponding projection;
full validation bypasses cheap timestamp/size reuse. Ordinary load decodes the
selected current-format package once and validates its serialized classes and
fields against one captured reflection catalog before constructing any object
skeleton. Unknown classes, fields, or signatures fail the complete load unless
an exact declaring-type/name/signature/custom-version `_DEPRECATED` route
claims the field. Canonical byte
comparison belongs to the construct-free audit path, not ordinary load.

## Early-Development Asset Compatibility

Until Durin makes an explicit external compatibility commitment, the repository
keeps one authored `.dasset` format and schema baseline. A format change first
inventories real source content and gets a separately scoped child plan. If
conversion is required, that plan adds the smallest exact, lossless offline
converter needed for the proven source format, rewrites the complete tracked
corpus explicitly, verifies it with `DevTool asset check --baseline`, and removes the
converter and obsolete reader in the same bounded effort. Engine does not
retain a general migration graph, structure-upgrader registry, partial
compatibility objects, or data-loss save permission between transitions.
Audit, registry discovery, ordinary loading, and editor startup never rewrite
authored packages.

External-project compatibility windows, release deprecation policy, and
downloadable migration bundles require a separate release-level decision; the
repository baseline alone does not establish such a promise.

## Release Convention

Durin follows Semantic Versioning for the engine release identifier. While the public API and long-lived content compatibility policy are still developing, releases remain under major version zero. Development builds use a prerelease channel such as `0.1.0-dev`; release tags use the corresponding stable form such as `v0.1.0` when that release is ready.
