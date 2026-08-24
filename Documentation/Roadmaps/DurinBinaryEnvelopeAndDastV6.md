# Durin Binary Envelope and DAST v6 Roadmap

Summary: Introduce the reusable DURF envelope, ship DAST v6, and replace the tracked DAST v5 asset baseline through one bounded offline conversion.

Last reviewed: 2026-08-25

Status: Active
Completed:

## Current Status

DAST v5 remains the only supported authored package format and the ordinary
writer. Its codec table, construct-free inspection, transactional load and
mutation paths, object-stream golden model, DTRL payload trailer, and DTRF
footer are production foundations rather than transitional stubs.

The binary-envelope investigation has now been selected for implementation as
DAST v6. The first child plan is active and owns the common `DURF` v1
foundation. No reader, writer, package, companion, registry baseline, or
tracked asset has changed yet.

The current repository conversion surface is bounded and observable: Git
tracks 30 `.dasset` packages, two `.dabulk` companions, and seven
`.dasset.hex` compatibility fixtures across the Engine and Sandbox projects.
This inventory is an initial planning fact, not the migration manifest; the
cutover plan must regenerate and freeze the complete manifest immediately
before conversion.

## Outcome

Every authored and cooked `.dasset` uses one DAST v6 byte contract beginning
with the shared `DURF` v1 preamble. Generic code can validate bounded front
matter and resolve the permanent DAST format identity without interpreting
asset semantics. AssetCore retains construct-free inspection, deterministic
save, complete codec capabilities, failure-atomic load and mutation, companion
ownership, and exact baseline enforcement. The repository contains no DAST v5
package, reader, writer, trailer/footer authority, or temporary converter after
the cutover.

## Scope

- A Core-owned, format-neutral `DURF` v1 parser, finalizer, immutable registry
  model, bounded validation policy, diagnostics, golden bytes, and independent
  reference parser.
- One permanent DAST `FormatId` and canonical debug name shared logically by
  legacy DAST v5 dispatch and the new DAST v6 envelope.
- The DAST v6 format-owned header, section directory, Public Summary, Import,
  Export, Payload Directory, and retained Name, Type, Schema, Value semantics.
- Complete DAST v6 capabilities for header inspection, validation, reference
  extraction, compatibility probing, live loading, writing, relocation,
  reference rewriting, redirector creation, Cook package construction, and
  authored companion transactions.
- An exact offline v5-to-v6 converter used only by the cutover plan, conversion
  of the complete tracked corpus and compatibility fixtures, and Engine plus
  Sandbox baseline validation.
- Promotion of implemented envelope and package rules into the owning Core,
  AssetCore, asset lifecycle, versioning, and content-storage documentation.

## Non-Goals

- Converting DABK, DBLK, TXPL, DMSH, DSKM, DANM, THPL, IBLP, or another payload
  or container format to `DURF` in this roadmap.
- Adding `PayloadTypeId` to DBLK entries or changing payload, schema, builder,
  DDC, or cooked-container version domains.
- A runtime migration graph, transparent editor rewrite, background resave, or
  retained multi-version authored-package policy.
- Package GUID identity, UUID-derived package paths, more than one public
  Export, cross-package access to private Exports, or append-in-place
  publication.
- A global mutable codec registry, static-constructor registration, or a new
  dynamic plugin loading and unload lifecycle.
- Authenticity, signing, encryption, or recovery from a damaged front header;
  `HeaderHash` is an integrity check only.

## Program Decisions and Invariants

- The selected successor is DAST v6 under `DURF` header version 1. DAST v5 and
  v6 are versions of one logical DAST format identity, not separate
  `FormatId` values.
- The DAST `FormatId` is a randomly generated, nonzero GUID constant checked
  into source with one permanent canonical debug name. It is opaque and is not
  derived from that name, an asset class, a payload type, or the old `DAST`
  mnemonic.
- Core owns only the common envelope grammar, exact little-endian encoding,
  integrity validation, descriptor types, and explicit immutable registry
  validation. AssetCore owns DAST format/version policy and every DAST semantic
  operation. Callers pass the registry they intend to dispatch; there is no
  process-global mutable authority.
- `DURF` v1 uses the investigation's fixed 64-byte preamble, including exact
  `PreambleBytes`, `HeaderBytes`, `FileBytes`, required-feature bits, and
  XXH3-128 `HeaderHash`. Hashing treats the stored hash field as zero and never
  treats integrity as authenticity.
- DAST v6 uses the selected 32-byte format header and 48-byte canonical section
  entries. Its initial writer emits contiguous extents with no implicit gaps
  or padding. Readers reject overlap, gaps, trailing bytes, overflow, and
  nonzero reserved fields.
- Public Summary and Import are entirely inside `HeaderBytes`. Directory entry
  extents and hashes are validated before a section body is interpreted. The
  initial required sections are Public Summary, Import, Name, Type, Schema,
  Export, Value, and Payload Directory.
- Unknown required features, summary fields, or sections fail closed. Unknown
  skippable data is extent- and hash-validated before it is ignored. Any
  operation that would publish replacement bytes must preserve such data
  canonically and byte-exactly or reject before mutation; silent loss is never
  permitted.
- The package path continues to derive from the mounted physical filename.
  `MainExportIndex` selects the single public asset. External hard references
  target one-based Imports; private objects use one-based Export indexes; soft
  references remain paths and do not become Imports.
- The DAST v6 front directory is the only section-location authority. The v5
  DTRL entries become the Payload Directory section and DTRF is removed. DABK
  companion validation and package-plus-companion publication remain separate
  AssetCore responsibilities.
- Using 64-bit extents does not remove allocation or inspection limits. Child
  plans select and test explicit header, file, section, Import, Export, string,
  and object limits from corpus measurements plus documented headroom.
- The ordinary writer changes only during the final cutover. Audit, catalog,
  loading, and editor startup remain read-only throughout the transition.
- The cutover follows the early-development compatibility policy: inventory
  real content, introduce the smallest exact converter for proven v5 input,
  convert and validate the complete baseline explicitly, then remove the
  converter and obsolete v5 reader in the same child plan. There is no long-
  lived dual-format baseline.

## Current Foundations and Gaps

### Landed foundations

- A statically composed AssetCore package codec table with complete capability
  validation and a single ordinary-writer policy.
- Deterministic DAST v5 object-stream discovery/emission, exact golden models,
  construct-free header and compatibility inspection, and checked reader
  bounds.
- Failure-atomic package load, save, relocation, redirector fix-up, reference
  rewrite, registry publication, and package-plus-DABK transactions.
- Core canonical little-endian readers/writers, `FGuid`, XXH3-128 hashing, and
  existing bounded binary-format tests.
- Read-only compatibility audit and `DevTool asset baseline` workflows for
  repository projects.

### Remaining gaps

- Core has no shared Durin binary preamble or explicit 128-bit format registry;
  AssetCore dispatch currently keys DAST only by the legacy magic and version.
- The candidate common and DAST headers have no production encoder, parser,
  independent golden implementation, mutation corpus, or frozen allocation
  limits.
- DAST v5 conflates dependencies with public-summary data, lacks explicit
  Import/Export sections and 64-bit extents, and retains a second EOF directory
  authority.
- No DAST v6 codec implements the full AssetCore capability surface or proves
  exact conversion of current logical values and external payload descriptors.
- The repository has no frozen v5 migration manifest, temporary exact
  converter, converted fixtures, or v6 baseline evidence.

## Milestone Map

| Milestone | State | Dependencies | Deliverable | Entry gate | Exit gate |
| --- | --- | --- | --- | --- | --- |
| 1. DURF envelope foundation | Active | Selected binary-envelope investigation | Core-neutral `DURF` v1 parsing/finalization, explicit immutable registry validation, permanent DAST identity, independent golden parser, bounded mutation/fuzz coverage, and dormant AssetCore envelope dispatch | DAST v6 is selected as the first format and current DAST v5 behavior is characterized | Exact golden bytes agree with the independent parser; duplicate, unknown, unsupported, malformed, overflow, feature, size, and hash cases fail deterministically; current v5 bytes and ordinary-writer behavior remain unchanged |
| 2. DAST v6 package codec | Planned | M1 | A detached DAST v6 codec with the complete AssetCore read/write/mutate/inspect capability set, Import/Export model, front Payload Directory, and no footer | M1 freezes the preamble, DAST identity, registry API, diagnostics, and bounded header contract | Production and independent bytes agree; header-only, full decode, reference, compatibility, mutation, redirector, companion, and failure-atomic tests pass; measured front-header size/cost is accepted; the ordinary writer still emits v5 |
| 3. DAST v6 baseline cutover | Planned | M2 | One bounded conversion plan that freezes the corpus, uses an exact offline v5 converter, switches all policy to v6, converts tracked packages and fixtures, validates both projects, and deletes v5 plus the converter | M2 proves lossless v5 mapping and all v6 capabilities; the conversion manifest includes every tracked package/fixture and package-companion closure; no unrelated binary edits overlap the cutover | Every tracked `.dasset` and supported fixture is v6; Engine and Sandbox baselines plus affected package/Cook workflows pass; no v5 codec, DTRL/DTRF authority, or converter remains; lasting contracts describe v6 as the sole baseline |

## Child Plan Boundaries

| Proposed or active plan | Milestone | Boundary | Activation |
| --- | --- | --- | --- |
| [Durin Binary Envelope Foundation](../Plans/DurinBinaryEnvelopeFoundation.md) | M1 | Common envelope and registry foundation, independent test oracle, permanent DAST identity, and fail-closed dispatch seam; excludes the DAST v6 section codec and asset conversion | Active; begin at Stage 0 |
| DAST v6 Package Codec | M2 | Complete detached v6 package codec and capability parity; excludes changing the ordinary writer and tracked files | Create after M1 exit evidence is recorded |
| DAST v6 Baseline Cutover | M3 | Exact temporary converter, frozen corpus conversion, writer/reader policy switch, project baseline qualification, and removal of all v5/converter code | Create only after M2 passes and the binary-edit window is coordinated |

M2 must not opportunistically switch the ordinary writer. M3 is deliberately
one child plan because the repository compatibility contract requires the
converter, corpus rewrite, baseline validation, and obsolete-reader removal to
remain one bounded transition rather than independent long-lived states.

## Program Validation Matrix

| Area | Required evidence |
| --- | --- |
| Common envelope | Exact minimal and nontrivial golden bytes, independent encode/decode agreement, two-phase bounded header reads, exact physical-size enforcement, zeroed-field header hashing, checked extents, and deterministic diagnostics |
| Identity and registry | Permanent nonzero DAST ID, canonical debug name, duplicate ID/name rejection, unknown format rejection, version/feature mismatch, reverse descriptor order, and no global registration-order dependency |
| DAST topology | Canonical required sections, unique ordered entries, Public Summary equality, bounded Imports/Exports, main/outer/index validation, no gaps/overlap/trailing bytes, no footer, and exact section hashes |
| Serialization and references | Deterministic repeated/reverse-discovery output, exact reuse or proven conversion of Name/Type/Schema/Value data, hard Import/Export union, soft-path preservation, redirector equality, and retained-unknown no-loss policy |
| Transactions and companions | Failure before construction/publication, complete rollback, relocation/fix-up/reference rewrite, external DABK descriptor equality, package-plus-companion atomicity, Cook package construction, and registry/cache invalidation by format |
| Migration | Frozen tracked manifest, v5 source fingerprint, dry-run report, byte/semantic comparison, fixture replacement, zero residual v5 files, removal of the converter and v5 reader, and deterministic `DevTool asset baseline` results for Engine and Sandbox |
| Cost and robustness | Header bytes and parse cost versus v5, pathological bounded counts, corrupted hash/extent corpus, deterministic preamble mutation/fuzz coverage, and plan-owned acceptance budgets before cutover |

Validation selection and execution follow the repository
[build and run](../Agents/BuildAndRun.md) and [testing](../Agents/Testing.md)
workflows. Each child plan owns its exact targets, fixtures, corpus commands,
budgets, and recorded evidence.

## Risks and Control Gates

- **Common-layer semantic leakage:** moving DAST paths, section kinds, payload
  placement, or publication policy into Core would merge independent formats.
  M1 exits only with a format-neutral Core API and AssetCore-owned dispatch.
- **Unbounded front matter:** 64-bit extents can hide excessive allocations or
  header reads. M1 provides caller-owned bounds; M2 freezes DAST-specific
  limits from measured corpus data before enabling v6 decoding.
- **Canonical extension loss:** accepting an optional section and later
  dropping it during save would corrupt extension ownership. Mutation and save
  fail unless unknown data can be preserved byte-exactly and canonically.
- **False migration confidence:** a synthetic converter can miss real reflected
  or external-bulk cases. M3 freezes Git-tracked packages, fixtures, and
  companions and validates both project baselines before removing v5.
- **Half-cut baseline:** keeping both ordinary writers/readers after conversion
  weakens the early-development contract. M3 cannot exit while v5 or the
  converter remains reachable.
- **Redundant integrity cost:** header and section hashes may duplicate a whole-
  file fingerprint. M2 records header-only and full-parse cost separately and
  the cutover requires explicit accepted budgets.
- **Binary merge conflicts:** `.dasset` is not semantically mergeable. M3 begins
  only with a coordinated binary-edit window and an isolated manifest; it does
  not absorb unrelated asset edits.

## Completion Criteria

- Milestones M1 through M3 pass their exit gates with evidence recorded in
  their child plans.
- DAST v6 is the only supported and emitted `.dasset` format, and every tracked
  package plus supported compatibility fixture matches that baseline.
- Engine and Sandbox baseline enforcement, affected package/Cook tests, and
  package-plus-DABK workflows pass after conversion.
- The temporary converter, DAST v5 reader/writer, DTRL/DTRF code, and obsolete
  v5-only tests are removed or rewritten as v6 contract coverage.
- Common-envelope, asset-package, lifecycle, versioning, and content-storage
  rules are authoritative in their owning documentation rather than this
  roadmap or the investigation.
- DABK, DBLK, and payload-format adoption are explicitly left unchanged and
  may advance only from a separately selected trigger and plan.

## Related Documentation

- [Durin Binary Envelope Evolution](../Investigations/DurinBinaryEnvelopeEvolution.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Versioning](../Runtime/Assets/Versioning.md)
- [Serialization](../Runtime/Core/Serialization.md)
- [Content Version Control](../Development/VersionControl/ContentVersionControl.md)
- [Build and Run Workflow](../Agents/BuildAndRun.md)
- [Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Serialization/BinaryFormat.h`
- `Engine/Source/Runtime/Core/Public/Misc/Guid.h`
- `Engine/Source/Runtime/Core/Public/Hash/XxHash.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageCodec.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV5Codec.cpp`
- `Engine/Source/Runtime/AssetCore/Private/Asset/PackageVersionPolicy.h`
- `Engine/Source/Runtime/AssetCore/Private/Asset/PackageTrailer.h`
- `Engine/Source/Runtime/AssetCore/Private/Asset/PackageObjectStreamReader.h`
- `Engine/Source/Runtime/AssetCore/Private/Asset/PackageObjectStreamWriter.h`
- `Engine/Tests/Native/CoreTests/Private/BinaryFormatTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageObjectStreamWireContractTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTrailerTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Source/Programs/DurinAssetTool`
