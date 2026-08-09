# Versioning

Summary: Define serialized type, object, package, and compatibility version contracts.

Modules: AssetCore, CoreDObject

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
authored-package Archives report the actual source format, currently DAST v4.
Ordinary and bundle saves report and emit v4. Property snapshots are
process-local and unversioned. Struct
`PostDeserialize` receives the Archive purpose and source format version instead
of deriving compatibility from the engine release number.

DAST v4 owns the package-local custom-version table, canonical GUID ordering,
discovery freeze, reader bounds, unknown-version rejection, and exact retained
closure/payload semantics.

Version registration is part of serializer discovery/emission parity. A format
or custom version first observed during emission is a late-discovery failure;
unsupported versions are rejected before destination mutation or output
publication.

## Authored Package Version Policy

Supported readers, the ordinary writer, and explicit migration writers are
separate policies backed by one private, statically composed codec table. Each
codec has an immutable string identity, wire version, and complete reader,
writer, and mutation capability set. Shared code parses the magic/version
preamble once, resolves a codec, and fails closed before codec parsing when no
reader exists. Header reads, validation, inspection, compatibility probes,
reference projection, live loading, serialization, relocation, reference
rewrite, redirector creation, and cook canonicalization do not branch on a
version enum. The repository currently registers only the bounded production
v4 codec; read-only entrypoints never select a writer or dirty authored content.

A frozen writer constructs its Archive context from its own codec identity.
The v4 writer therefore always reports DAST v4 to serializers and emits v4,
independently of the selected ordinary or migration writer policy. The
reader-policy cache identity is an explicit generation, not a wire-version
alias; changing the supported-reader contract requires changing that identity.

Registry and reference-cache fingerprints include the source DAST version.
Changing package bytes or versions invalidates the corresponding projection;
full validation bypasses cheap timestamp/size reuse. When a future exact
migration edge is registered, a migration plan records the source version and
fingerprint, policy identity, codec identities, and one exact registered edge
to its target writer, and fails
closed on stale input, missing dependency closure, compatibility findings, or
retained-data risk. Publication is bundle-atomic and journal-compensated.

## Early-Development Asset Compatibility

Until Durin makes an explicit external compatibility commitment, the repository
keeps one authored `.dasset` format and schema baseline. A format change first
adds a temporary exact-edge, lossless migration to `DurinAssetTool`; developers
review a dry-run, apply it explicitly to the complete tracked corpus, and verify
the current baseline with `DevTool asset baseline`. Once all tracked packages
are current, the previous reader, migration edge, fixtures, and compatibility
branches are removed in the same bounded effort. Audit, registry discovery,
ordinary loading, and editor startup never rewrite authored packages.

External-project compatibility windows, release deprecation policy, and
downloadable migration bundles require a separate release-level decision; the
repository baseline alone does not establish such a promise.

## Release Convention

Durin follows Semantic Versioning for the engine release identifier. While the public API and long-lived content compatibility policy are still developing, releases remain under major version zero. Development builds use a prerelease channel such as `0.1.0-dev`; release tags use the corresponding stable form such as `v0.1.0` when that release is ready.
