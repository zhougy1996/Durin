# Durin Binary Envelope Evolution

Summary: Record a candidate long-lived self-describing file preamble and DAST package-header direction without changing the current formats.

**Status:** Selected for implementation as DAST v6; no current-format change has landed.

**Last reviewed:** 2026-08-25

## Scope And Verdict

Durin currently uses one bounded DAST v5 envelope for every authored and cooked
`.dasset`, separate DABK and DBLK containers for authored and cooked bulk
ownership, and asset-family payload formats such as TXPL, DMSH, DSKM, DANM,
THPL, and IBLP. Those formats are implemented and remain authoritative.

The unresolved long-term problem is format identification and front-header
evolution. Continuing to allocate a new mnemonic four-byte magic for each
standalone Durin codec does not scale to extension modules, gives generic tools
no common probing contract, and encourages file-format identity, asset type,
payload identity, and payload schema to become conflated.

The leading candidate is one shared `DURF` binary-envelope preamble followed by
a stable 128-bit `FormatId` and a format-owned header. DAST would retain one
format identity for all object assets; DBLK and DABK would retain separate
format identities and authority. Reflected asset types would retain permanent
serialized qualified names, while payload codecs may use their own stable type
identities instead of new magic values. Sharing the preamble must not merge the
formats' schemas, services, validation, or publication rules.

This investigation by itself did not select DAST v6, change a reader or writer,
promise backward compatibility, or authorize tracked-asset conversion.

The implementation selection made after this evidence was recorded is tracked
by the [Durin Binary Envelope and DAST v6 roadmap](../Roadmaps/DurinBinaryEnvelopeAndDastV6.md).
Its first active child is the
[Durin Binary Envelope Foundation plan](../Plans/DurinBinaryEnvelopeFoundation.md).
Those documents own selected scope, decisions, stages, and acceptance gates;
this investigation remains the evidence and candidate-design record until the
validated change lands.

## Verified Current Behavior

### P1 — Format discovery has no common Durin binary preamble

DAST v5 begins with `DAST` and format version 5. Its bounded public summary
contains the main asset class, asset-versus-redirector kind, redirect
destination, canonical dependencies, and object count. Five fixed directory
entries then locate the Name, Type, Schema, Object, and Value sections. The
object stream uses 32-bit offsets and lengths and ends before the separately
validated DTRL trailer and DTRF footer.

DABK, DBLK, and asset-family payload codecs own other magic values and headers.
Generic code must already know which family it is probing or accumulate every
magic value. Adding another standalone codec therefore creates another global
identifier and another probing branch even when its schema and ownership are
otherwise private.

**Impact:** the current formats are deterministic and bounded, but the
identification convention has no scalable plugin-facing namespace or common
minimum header inspection path.

### P1 — Several distinct identities are easy to conflate

Current code correctly separates many of these concepts, but a future header
must make the distinction explicit:

- a container format selects a byte grammar and codec;
- a DAST main-asset type selects reflected construction and serialization;
- a payload type selects a domain decoder;
- a payload id identifies one logical payload slot;
- a content hash identifies exact bytes;
- a schema version selects one readable layout;
- a builder version changes derived output semantics and cache keys.

Using a new four-byte magic as an asset-type or payload-type enum collapses
these domains and makes independent evolution harder.

### P2 — The DAST v5 front matter is deliberately frozen rather than extensible

DAST v5 requires five 9-byte directory entries, one-byte section kinds,
32-bit extents, and an exact canonical section set. This is appropriate for the
current 256 MiB object-stream limit, and unknown sections correctly require a
new format version. It is not a defect in v5.

A successor intended to remain stable longer should nevertheless define
explicit total header size, exact file size, directory-entry size, 64-bit
extents, required-versus-skippable extensions, and front-header integrity from
its first version. Those fields cannot be retrofitted by reinterpreting frozen
v5 bytes.

## Candidate Identity Model

The following identities are candidates for the next format family. They are
separate persistent domains and are never aliases for one another.

| Identity | Candidate representation | Meaning |
| --- | --- | --- |
| Envelope magic | One `DURF` value | Recognizes a Durin binary file, not its semantic type. |
| Header version | `uint16` | Defines only the shared fixed `DURF` header. |
| Format id | Stable nonzero 128-bit GUID | Selects DAST, DABK, DBLK, or another standalone byte grammar. |
| Format version | `uint32` | Selects the selected format's readable wire contract. |
| Reflected asset class | Permanent serialized qualified UTF-8 name | Selects reflected construction from the main Export without requiring a class GUID. |
| Payload type id | Stable nonzero 128-bit id | Selects a payload codec in a typed container entry or standalone payload. |
| Payload id | Existing stable GUID | Identifies one logical payload slot owned by a package. |
| Content hash | Explicit algorithm and 128-bit value | Verifies exact bytes; it is not a type or slot identity. |
| Schema version | Domain-owned fixed-width integer | Selects a payload or logical schema layout. |

`FormatId` allocation should be generated and registered with a permanent
canonical debug name. An implementation plan must choose between generated
random GUID constants and namespace-derived deterministic GUIDs. A 64-bit hash
alone is not a sufficient authority because collision handling would require a
second identity anyway.

`FormatId` is an opaque 128-bit identity, not a padded four-byte code. A format
registry may display names such as `DAST`, `DABK`, or `DBLK` in tools and logs,
but those names are debug metadata rather than another persisted dispatch key.
`DURF` remains the only mnemonic four-byte value in the common envelope.

## Candidate Common Preamble

Every standalone Durin-owned binary would begin with the same candidate
64-byte, explicitly encoded, little-endian preamble:

| Offset | Bytes | Field | Candidate rule |
| ---: | ---: | --- | --- |
| 0 | 4 | `Magic` | Bytes `DURF`; this is the only shared mnemonic magic. |
| 4 | 2 | `HeaderVersion` | Version of this common header, initially 1. |
| 6 | 2 | `PreambleBytes` | Exactly 64 for version 1; permits a later header version to change its fixed prefix deliberately. |
| 8 | 16 | `FormatId` | Stable codec identity resolved through a generated registry. |
| 24 | 4 | `FormatVersion` | Version interpreted only by the selected codec. |
| 28 | 4 | `RequiredFeatures` | Unknown set bits reject before format-specific parsing; initially zero. |
| 32 | 8 | `HeaderBytes` | Complete contiguous front matter, including this preamble and format-owned header/directory/summary bytes. |
| 40 | 8 | `FileBytes` | Exact physical file extent; truncation and trailing bytes reject. |
| 48 | 16 | `HeaderHash` | XXH3-128 over `[0, HeaderBytes)` with these 16 bytes treated as zero. |

The common reader would validate only the preamble, checked extents, registered
format identity, supported format version, required features, and exact header
hash. It would then pass the immutable byte span to the selected format codec.
It would not interpret DAST object tables, DBLK payload entries, compression,
asset paths, or domain schemas.

`HeaderHash` is an integrity check rather than an authenticity mechanism. A
format remains responsible for complete section, payload, and container hashes
and for transactional decode and publication.

## Candidate DAST Successor Header

After the common preamble, a DAST successor could use this fixed 32-byte
format-owned header. Offsets below are relative to the start of this DAST
header; persisted offsets contained by it are absolute file offsets.

| Offset | Bytes | Field | Candidate rule |
| ---: | ---: | --- | --- |
| 0 | 4 | `PackageKind` | Ordinary asset or redirector; unknown values reject. |
| 4 | 4 | `PackageFlags` | Unknown required flags reject; no placement or runtime-residency state. |
| 8 | 8 | `DirectoryOffset` | Absolute offset of the canonical section directory inside `HeaderBytes`. |
| 16 | 4 | `SectionCount` | Bounded count checked before allocation. |
| 20 | 4 | `SectionEntryBytes` | Exact entry size for the selected DAST format version. |
| 24 | 8 | `Reserved` | Must be zero. |

The package path would continue to derive from the mounted physical filename;
this header does not silently select UUID-based authored identity. A future
package GUID must have independently specified copy, duplication, merge,
catalog, redirector, and repair semantics before occupying a persistent field.

The main reflected type is the class recorded by `MainExportIndex`; it is not a
second fixed-header identity. The required Public Summary retains that class's
permanent serialized qualified name as a construct-free projection and full
validation requires exact equality with the main Export. Renaming a C++
spelling preserves the serialized name or uses an explicit compatibility alias.

DAST therefore has its own header after the common preamble. The common
preamble selects the DAST codec and validates generic front-matter bounds; the
DAST header alone defines package kind and section topology. Import, Export,
and reflected class semantics remain DAST format data rather than common-header
features.
DABK, DBLK, and other `FormatId` values select different format-owned headers.

### Candidate Section Directory

Each DAST section entry could be 48 bytes:

| Offset | Bytes | Field | Candidate rule |
| ---: | ---: | --- | --- |
| 0 | 4 | `Kind` | Numeric registered section kind; no four-byte magic. |
| 4 | 4 | `Flags` | Includes required-versus-skippable semantics. |
| 8 | 8 | `Offset` | Absolute 64-bit byte offset. |
| 16 | 8 | `Size` | Exact stored byte extent. |
| 24 | 16 | `SectionHash` | XXH3-128 of the exact stored section bytes. |
| 40 | 8 | `Reserved` | Must be zero. |

Entries would be unique and canonically ordered by `Kind`. Extents would use
checked arithmetic, remain non-overlapping and inside the file, and either be
contiguous or require explicitly specified zero padding. An unknown required
kind would reject; an unknown skippable kind could be ignored after its extent
and hash were validated. A section that changes the interpretation of a known
required section is not optional and still requires a new DAST format version.

The candidate required section set is Public Summary, Import, Name, Type,
Schema, Export, Value, and Payload Directory. Public Summary and Import are
header-resident and contained completely by `HeaderBytes`; the remaining
sections begin at or after `HeaderBytes`. Rich registry metadata and reference
indexes may become skippable derived sections only when their authority and
stale-data validation are defined. Custom versions remain schema data rather
than a container version.

### Candidate Import And Export Semantics

DAST should adopt explicit Import and Export vocabulary without adopting UE's
`UClass`-as-imported-`UObject` model:

- an Export is one object serialized in the current package;
- an Import is one canonical external hard-reference target required by the
  package;
- only the main Export is currently public to other packages;
- private inner Exports may be referenced only through an in-package
  `ExportIndex`;
- a soft reference retains its canonical asset path and is not an Import or an
  eager-load dependency;
- a reflected class is selected directly from its permanent serialized class
  name and reflection registry, never through an Import entry.

The candidate Import entry is deliberately self-contained so construct-free
dependency scanning does not require Name, Type, or Export sections:

```text
wire-string TargetAssetPath
```

Imports are nonempty, unique, and canonically sorted by exact normalized path
bytes. One-based `ImportIndex` values refer to that frozen order. The field's
HardRef logical type continues to carry its expected reflected class, so an
Import does not duplicate `ExpectedType`. The canonical Import section is the
only stored hard-dependency authority; public header inspection derives its
dependency list from these entries.

The candidate Export entry preserves the current object-table semantics:

```text
ExportIndex OuterExportIndex  // zero only for the package graph root
NameId      ClassName
NameId      ObjectName
uint32      ExportFlags
```

Exports are canonically ordered with stable one-based indexes. The package
graph root remains the first structural record, and `MainExportIndex` selects
the public main asset. The Value section contains one length-delimited block per
Export in the same index order, so Export entries do not duplicate
`SerialOffset` or `SerialSize`.

Hard object references use one explicit union:

```text
Null
ExportIndex  // object in this package
ImportIndex  // external package main asset
```

The loader creates all Export skeletons before resolving Imports and applying
Value blocks. Loader scheduling dependencies are derived from Imports and are
not a second authored Dependency table.

### Counts And Offsets

The successor should not copy the UE-style fixed-header pattern of a dedicated
`NameCount/NameOffset`, `ImportCount/ImportOffset`, and
`ExportCount/ExportOffset` pair for every table. One generic section directory
already provides the only universal facts needed to locate or skip a section:
kind, flags, offset, size, and hash.

Each table section owns its bounded element count as the first value in that
section. Counts are interpreted only after the containing extent and hash have
been validated. The Public Summary retains the small counts needed by cheap
construct-free tooling, including Import and Export counts. A count is not
added to every generic directory entry because Public Summary, Value, and
Payload Directory do not share one meaningful element-count semantic.

This keeps the fixed DAST header stable when a later format adds a new section:
the header gains neither another offset nor another count field.

### Candidate Public Summary

The summary could use canonically ordered, unique length-delimited fields:

```text
uint16 FieldKind
uint16 FieldFlags
uint32 ByteSize
byte   Value[ByteSize]
```

Its required fields would contain `MainExportIndex`, the serialized main Export
class name, and bounded Import and Export counts. The class name is a fast
projection that must equal the selected Export's class; it is not a second type
authority. A redirector destination is derived from its single Import and must
still equal the reflected redirector field. Unknown required fields would
reject and unknown skippable fields would be retained or ignored according to
an explicitly selected canonical-resave rule. Arbitrary asset-family tags
would not become header authority merely because this field format can carry
them; the catalog should continue deriving rich searchable metadata through
construct-free inspection and a source fingerprint.

### Footer Decision

The leading candidate has no DAST footer. `FileBytes` defines the exact physical
extent, the front directory locates every required section, every entry carries
its exact extent and hash, the header hash protects the directory, and gaps or
trailing bytes reject. Together with Durin's detached whole-file construction
and atomic publication, a second EOF directory would duplicate authority.

The current DTRL payload trailer becomes the required Payload Directory section
described by the front directory. DTRF is not carried forward merely for
historical symmetry. A footer should be reconsidered only if a concrete future
requirement selects append-in-place publication, tail-only range discovery, or
recovery from a damaged front header. None is part of the current asset
publication contract.

## Payload And Container Direction

The common preamble would eliminate new per-format magic allocation without
forcing one universal payload schema:

- all `.dasset` packages use the DAST `FormatId`, regardless of asset class;
- all authored bulk containers use the DABK `FormatId`;
- all cooked bulk containers use the DBLK `FormatId`;
- a standalone domain payload uses `DURF` plus its registered `FormatId`;
- an embedded DBLK payload may omit a nested preamble when its directory entry
  already carries `PayloadTypeId` and `SchemaVersion`.

The last rule avoids wrapping every embedded payload merely to repeat
information already protected by its owning container. A DBLK successor may
add `PayloadTypeId` to its entry contract, but that is an independent DBLK
format change and must not be smuggled into a DAST header revision.

Existing TXPL, DMSH, DSKM, DANM, THPL, IBLP, and other payload magic values
remain valid in current schemas. A future conversion may replace them with
registered payload type identities, but generated DDC and cooked data should be
invalidated and rebuilt rather than treated as authored-asset migration.

## Version And Extension Rules

- `HeaderVersion` changes only when the shared 64-byte header grammar or
  semantics change.
- `FormatVersion` changes when DAST, DABK, DBLK, or another selected codec
  changes a required byte contract.
- `SchemaVersion` changes when one domain payload or reflected schema changes.
- `BuilderVersion` changes DDC identity when output semantics change without
  necessarily changing readable payload bytes.
- Adding a truly skippable summary field or section may retain the DAST format
  version only when old readers can validate and ignore it without changing
  known semantics or canonical output.
- `PreambleBytes`, `HeaderBytes`, and `SectionEntryBytes` provide explicit
  extents; they never authorize reinterpreting frozen bytes in place.
- Unsupported header or format versions fail before format-specific
  construction, mutation, fallback, or publication.

## Migration Boundary

No current `.dasset`, `.dabulk`, `.dbulk`, DDC object, or source asset should be
rewritten for this investigation.

When an implementation trigger is selected, the plan must inventory real
tracked authored content first. Generated DDC objects and cooked packages are
invalidated and rebuilt. If authored DAST bytes change, the early-development
compatibility policy requires the smallest exact offline converter for the
proven source format, explicit corpus conversion and baseline validation, and
removal of the obsolete reader and converter in the same bounded effort. A
generic runtime migration graph is not the default outcome.

The candidate v5 conversion remains construct-free: canonical v5 dependency
paths become Import entries in the same order, Object records become Export
records with the same local indexes, and external hard-reference dependency ids
become `ImportIndex` values without changing Value payload semantics. DTRL
entries become the Payload Directory section. Name, Type, Schema, and Value
section grammars remain eligible for byte-exact reuse.

## Risks, Assumptions, And Open Questions

- **P1 decision:** whether `FormatId` values are random generated GUID constants
  or deterministically derived from permanent canonical names.
- **P1 decision:** whether the next DBLK entry carries `PayloadTypeId`, or the
  owning asset class and fixed payload-slot id remain sufficient dispatch.
- **P1 decision:** whether any future workflow justifies more than one public
  Export or cross-package references to inner objects; the candidate forbids
  both.
- **P1 risk:** permitting skippable sections without a format bump can break
  canonical resave if exact retention semantics are not defined.
- **P1 risk:** a header-resident Import section needs strict path, entry-count,
  and total-header byte limits so pathological dependency sets cannot make
  header inspection unbounded.
- **P2 decision:** acceptable `HeaderBytes`, section-count, object-count, and
  total-file limits despite the use of 64-bit extents.
- **P2 risk:** hashing every section improves corruption localization but can
  add redundant validation work when a complete source fingerprint is already
  available.
- **P2 assumption:** ordinary readers start from the front preamble; a future
  tail-only discovery requirement would reopen the no-footer decision.
- **Assumption:** metadata remains compact and uncompressed; large or
  streamable data continues to live in bounded bulk containers.
- **Assumption:** sharing a preamble does not create a shared provider,
  residency state, placement enum, or publication transaction across authored,
  DDC, and cooked authorities.

## Evidence And Validation Gates

Before advancing this investigation to an implementation plan, require:

- byte-exact candidate golden files and an independent reference parser;
- deterministic repeated and reverse-discovery encoding;
- header-only reads that touch only the bounded front matter;
- unknown required/optional feature, summary-field, and section cases;
- duplicate format/type registration and identity-mismatch tests;
- canonical Import sorting and deduplication, invalid Import indexes, and
  rejection of cross-package private-Export targets;
- canonical Export topology, main-Export bounds, summary-class equality, and
  invalid Outer or internal-reference indexes;
- exact equality between Public Summary Import/Export counts and their
  validated section models;
- truncation, trailing data, nonzero reserved bytes, extent overflow,
  overlap, gap/padding, excessive counts, and bad-hash tests;
- fuzzing of the common preamble before codec dispatch;
- construct-free type, dependency, redirector, and object-count projection;
- exact failure-atomic loading and retained-unknown canonical resave behavior;
- a measured header-size and parse-cost comparison against DAST v5;
- tracked-content inventory and a proven lossless v5 conversion strategy if
  authored bytes must change.

The implementation trigger is a selected DAST successor, DBLK successor, or
plugin-facing standalone format that would otherwise add another mnemonic
magic or require incompatible v5 front matter. A new asset family alone is not
a trigger because DAST already identifies its reflected type without a new
package magic.

## Related Documentation

- [Asset Packages](../Runtime/Assets/AssetPackages.md#file-format)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md#versioning)
- [Versioning](../Runtime/Assets/Versioning.md#authored-package-version-policy)
- [Serialization](../Runtime/Core/Serialization.md)

## Relevant Implementation

- [DAST version policy](../../Engine/Source/Runtime/AssetCore/Private/Asset/PackageVersionPolicy.h)
- [DAST v5 codec](../../Engine/Source/Runtime/AssetCore/Private/AssetPackageV5Codec.cpp)
- [DAST object-stream writer contract](../../Engine/Source/Runtime/AssetCore/Private/Asset/PackageObjectStreamWriter.h)
- [DAST trailer and footer](../../Engine/Source/Runtime/AssetCore/Private/Asset/PackageTrailer.h)
- [Public package-header projection](../../Engine/Source/Runtime/AssetCore/Public/Asset/PackageInspection.h)
- [Cooked payload descriptor](../../Engine/Source/Runtime/AssetCore/Public/Asset/CookedAsset.h)
