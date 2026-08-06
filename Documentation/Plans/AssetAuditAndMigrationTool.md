# Asset Audit and Migration Tool Plan

Summary: Unify read-only asset auditing and explicitly authorized repository-wide migration, then use it to keep the early-development asset baseline current without retaining historical compatibility code.

Last reviewed: 2026-08-07

Status: Active
Completed:

## Current Status

Durin currently ships a read-only `DurinAssetAudit` native host behind
`DevTool asset audit`. The host streams package metadata, compares it with the
current reflection catalog, emits report schema v1, and never loads or saves
package objects. AssetCore writes DAST v3 and accepts DAST v2 through v3.

The tracked authored corpus contains 18 `.dasset` packages and all 18 currently
use DAST v2. Production has no registered asset-specific structure upgrader.
Unknown fields and incompatible signatures fail closed, and the editor rejects
assets that do not match the current authored baseline.

No migration command or project-wide write transaction exists. Until this plan
is implemented, converting v2 packages requires loading and explicitly saving
them through existing asset workflows, and removal of the v2 reader cannot be
proved safe by a repository-wide gate.

## Goal

Provide one asset maintenance tool with a shared scanner, compatibility model,
and reporting contract:

- `asset audit` remains strictly read-only;
- `asset migrate` produces a deterministic migration plan without writing;
- `asset migrate --apply` is the only command that may rewrite authored
  packages;
- successful migration is followed by a fresh audit before the command reports
  success; and
- repository and CI gates prove that only the current authored baseline remains,
  allowing obsolete readers and migration handlers to be deleted promptly.

The first production use migrates every tracked DAST v2 package to v3 and then
removes v2 package support.

## Scope

- Replace the audit-only native host with one asset-tool host used by the
  existing `DevTool asset` command group.
- Share mount discovery, fingerprinting, reflection capture, compatibility
  classification, deterministic ordering, cancellation, and report rendering
  between audit and migration modes.
- Add a migration registry whose steps declare exact source and target format
  or schema versions, stable handler identity, applicability, and risk.
- Support dry-run planning, explicit apply, atomic publication, rollback,
  post-write verification, and machine-readable results.
- Migrate the complete tracked Engine and Sandbox authored corpus from DAST v2
  to v3.
- Add repository validation that rejects non-current package formats and
  incompatible authored schemas.
- Remove v2 reading and the completed v2-to-v3 migration step after the tracked
  corpus and tests use v3.
- Document the early-development compatibility policy as a lasting runtime and
  development contract when implementation completes.

## Non-Goals

- Silent migration during engine or editor startup.
- Automatic writes from `asset audit`, the editor compatibility window, asset
  registry discovery, or ordinary package loading.
- A long-term compatibility promise for external projects, plugins, cooked
  products, or released engine builds.
- Best-effort repair of corrupt packages, unknown fields, unavailable classes,
  incompatible signatures, or migrations classified as data-loss risks.
- Retaining every historical reader or migration step in the shipping runtime.
- Editing source import files, derived-data caches, cooked outputs, redirect
  destinations, or version-control history.
- Starting DAST v4 encoding work; the tool is reusable infrastructure and the
  v2-to-v3 corpus migration is its initial proof.

## Design Decisions and Invariants

### One tool, capability-separated modes

- The canonical native executable is `DurinAssetTool`; it replaces the
  misleading audit-only executable name rather than adding a second program.
- DurinDevTool remains the user-facing entrypoint and exposes sibling commands
  `asset audit` and `asset migrate`.
- Audit and migration use the same discovery snapshot and compatibility
  records. Migration extends those records with proposed steps and outcomes;
  it does not maintain a second definition of compatibility.
- The old `DurinAssetAudit` target and executable are removed when all tracked
  invocations, tests, and documentation move. No compatibility alias is kept
  while Durin has no external tool contract.

### Mutation requires explicit authorization

- `asset audit` is structurally incapable of saving: its execution path never
  constructs mutable package graphs or calls a publication API.
- `asset migrate` defaults to planning and is read-only. The exact `--apply`
  flag selects the write path; there is no implicit confirmation or startup
  prompt.
- Selection filters narrow the operation but never broaden it beyond the
  current project's Engine, project, and configured auto-scan mounts.
- A plan records each package's virtual path, physical path, size, modification
  time, content hash, source version, target version, migration chain, and risk.
  Apply rejects a package whose fingerprint changed after planning.
- Unknown, unsupported, stale, corrupt, or data-loss-risk records make apply
  fail before any destination is published.

### Early-development compatibility policy

- While Durin has no released external project compatibility commitment, the
  authored packages tracked at repository `HEAD` are the sole supported content
  baseline.
- A breaking asset change must include the temporary reader or migration step,
  the repository-wide asset rewrite, validation updates, and deletion criteria
  in one active plan. Compatibility code is temporary implementation
  scaffolding, not a permanent runtime feature.
- The final stage of that plan removes obsolete readers, writers, migration
  steps, fixtures, and branches after the repository gate proves their source
  version count is zero. Keeping them requires a newly documented external
  compatibility requirement, not speculation about future users.
- CI accepts only the current package format and current supported per-asset
  schemas. It prevents an old package copied from another checkout from silently
  restoring compatibility debt.
- Git history is the recovery and historical-development mechanism. The current
  engine does not need to open authored assets from an older commit because that
  commit already contains its matching engine implementation.
- When Durin first promises compatibility to an external project or release,
  this policy must be revisited before that promise ships. Version support
  windows and deprecation periods then belong in a separate selected plan.

### Migration chain and ownership

- AssetCore owns package-format migration orchestration, snapshots, serialized
  field payloads, reference fixups, risk classification, and publication.
- Engine owns asset-class schema steps because it owns their authored meaning.
  A step may use reflected construction and `PostLoad`, but must report every
  mutation under a stable handler identity.
- Each migration edge is exact and directed, for example `DAST 2 -> 3` or
  `MaterialSchema 3 -> 4`. The planner constructs a complete ordered chain and
  rejects missing, ambiguous, cyclic, or overshooting paths.
- The writer emits only the single current format. Migration never offers an
  older target and never downgrades content.
- A package is writable only when every finding is either compatible or handled
  by a registered lossless migration. Merely skipping an unknown field is not a
  migration.
- Migration output must be deterministic: identical source bytes, reflection
  catalog, tool build, and options produce identical proposed steps and final
  package bytes.

### Publication and failure semantics

- Planning and full serialization validation complete for the entire selected
  set before the first tracked destination changes.
- Apply stages complete sibling files, flushes and closes them, then publishes
  through the repository atomic-file contract. A transaction manifest and
  sibling rollback copies cover the multi-file publish window because the
  shared atomic helper guarantees only one file at a time.
- If staging, publication, or verification fails, the tool restores every
  already-published destination and reports the rollback result. It never
  reports success for a partially migrated selection.
- After publication, the tool discards loaded objects, takes a new physical
  snapshot, reruns the streaming compatibility audit, and verifies current
  versions, fingerprints, and zero unhandled findings.
- Cancellation is honored during discovery, planning, and staging. Once commit
  publication begins, cancellation is deferred until publish or rollback reaches
  a consistent terminal state.
- Temporary files, rollback copies, and transaction manifests are immediate
  siblings on the same volume and are removed after a verified success.

### Reports and exit behavior

- Existing audit report schema v1 and its deterministic ordering remain stable.
  Migration uses a separately versioned schema so write-specific fields do not
  weaken audit consumers.
- Human output groups planned, migrated, skipped, blocked, failed, and rolled
  back packages and ends with counts plus the report path when one is requested.
- Exit `0` means the requested read-only operation completed or the full apply
  and post-audit succeeded. Policy rejection uses `3`, operational or rollback
  failure uses `1`, command-line misuse uses `2`, and cancellation uses `130`.
- Reports never claim that source control was staged or committed. The tool
  lists changed authored paths for the caller to review.

## Current Foundations and Gaps

| Area | Current foundation | Gap closed by this plan |
| --- | --- | --- |
| Discovery | Auto-scan mount enumeration and stable virtual-path ordering | Shared snapshot API instead of program-local audit enumeration |
| Compatibility | Streaming header/object/field probe and report schema v1 | Migration applicability and lossless-chain planning |
| Package I/O | v2/v3 reader, v3 writer, atomic single-package save | Explicit batch transaction, rollback, and post-apply audit |
| Structure changes | Load reports and upgrader registration exist | Production registration lifecycle and tool-owned execution contract |
| CLI | `DevTool asset audit` invokes `DurinAssetAudit` | Unified `DurinAssetTool` plus plan/apply migration commands |
| Editor | Read-only compatibility audit UI | No writable editor workflow; it continues consuming shared audit records |
| Baseline | 18 tracked packages, all DAST v2 | All tracked packages v3 and CI-enforced current-only baseline |
| Retirement | Runtime accepts v2 through v3 | Evidence-gated removal of v2 and completed migration code |

## Implementation Stages

### Stage 0: Freeze the command, policy, and corpus contract

Dependencies: existing read-only audit and v3 writer.

- [ ] Record a checked inventory of every tracked `.dasset`, its mount, package
  format, schema findings, and content hash.
- [ ] Add command-parser and report fixtures for the selected `audit`,
  `migrate`, and `migrate --apply` grammar.
- [ ] Define migration report schema v1 and add its JSON Schema beside the
  existing audit schema.
- [ ] Characterize existing audit output, cancellation, policy exits, missing
  project handling, and read-only behavior before renaming the host.
- [ ] Record any asset-specific findings that make a v2 package unsafe to
  round-trip; resolve them in the owning later stage rather than weakening the
  gate.

#### Acceptance Gate

- The tracked corpus inventory is deterministic and confirms the exact v2
  migration set.
- Command, report, exit-code, authorization, and early-development retirement
  policies have executable tests with no open design choice blocking Stage 1.

### Stage 1: Unify the native host and preserve read-only audit

Dependencies: Stage 0.

- [ ] Extract mount snapshotting and fingerprint capture from
  `AssetAuditMain.cpp` into an AssetCore-owned service usable by both modes.
- [ ] Introduce the `DurinAssetTool` target and mode dispatch, then route
  `DevTool asset audit` to it.
- [ ] Update DurinDevTool executable resolution, help, schemas, focused tests,
  build metadata, and operational documentation.
- [ ] Remove `DurinAssetAudit` after repository references reach zero.
- [ ] Prove the audit mode still performs no package construction, dependency
  loading, registry-cache writes, source mutation, or package publication.

#### Acceptance Gate

- Existing audit golden reports and policy/exit tests pass unchanged through
  `DurinAssetTool`.
- A filesystem snapshot proves audit makes no authored or cache changes.
- The old executable, target, and tracked references are absent.

### Stage 2: Build deterministic migration planning

Dependencies: Stage 1.

- [ ] Add exact-edge package-format and asset-schema migration registration,
  validation, stable identities, and ordered chain resolution.
- [ ] Add a migration planner that consumes the shared discovery snapshot and
  compatibility records without writing.
- [ ] Register the bounded DAST v2-to-v3 envelope migration and classify any
  object/schema transformations separately.
- [ ] Implement selection filters and deterministic human/JSON migration-plan
  output through `DevTool asset migrate`.
- [ ] Fail closed for missing chains, ambiguous handlers, cycles, stale
  fingerprints, unknown fields, unavailable classes, corrupt packages, and
  non-lossless findings.
- [ ] Add malformed-input, ordering, cancellation, determinism, mixed-version,
  dependency, and no-write tests.

#### Acceptance Gate

- Dry-run reports all 18 tracked v2 packages as either losslessly migratable or
  identifies a concrete owning fix; it changes no file.
- Repeated dry-runs produce byte-identical reports and migration chains.
- Every rejected risk has a stable diagnostic and no path to apply.

### Stage 3: Implement transactional apply and verification

Dependencies: Stage 2 and the atomic file-publication contract.

- [ ] Add isolated package construction, migration execution, mutation
  accounting, current-writer serialization, and in-memory output validation.
- [ ] Implement full-set preflight and staging before publication.
- [ ] Implement the transaction manifest, same-volume rollback copies,
  publish/rollback state machine, interruption recovery, and stale-input checks.
- [ ] Expose writes only through `DevTool asset migrate --apply` and keep
  planning as the default.
- [ ] Unload migrated objects and require a fresh streaming post-audit before
  deleting rollback state or returning success.
- [ ] Add injected staging, publish, verification, cancellation, and rollback
  failures, including a failure after at least one destination was published.

#### Acceptance Gate

- Successful apply produces current deterministic bytes and a clean post-audit.
- Every injected failure leaves the complete selected corpus byte-identical to
  its pre-apply snapshot or reports an explicit recoverable transaction state.
- Audit and dry-run remain demonstrably read-only after writable code exists in
  the same executable.

### Stage 4: Migrate the repository baseline to DAST v3

Dependencies: Stage 3; every tracked package planned as lossless.

- [ ] Run a final dry-run against Engine and Sandbox mounts and review the
  package-by-package plan.
- [ ] Apply the complete migration as one tool transaction.
- [ ] Review asset diffs, deterministic hashes, migration report, and fresh
  compatibility audit.
- [ ] Update versioned fixtures and focused expectations to v3 without retaining
  v2 copies as production compatibility fixtures.
- [ ] Commit the tool-produced authored changes with the Stage 4 plan handoff.

#### Acceptance Gate

- Every tracked `.dasset` reports DAST v3, current fingerprints, and zero
  incompatible, unsupported, failed, or stale findings.
- A clean checkout reproduces the same audit result and all focused package,
  Engine asset, editor workflow, and cook tests pass.

### Stage 5: Enforce the baseline and delete v2 compatibility

Dependencies: Stage 4.

- [ ] Add a repository/CI asset-baseline command that fails when any tracked
  authored package is not the single current format or current schema baseline.
- [ ] Remove `MinimumAssetVersion`, v2 header branches, v2 type-signature
  acceptance, v2-only tests, and the completed v2-to-v3 migration edge.
- [ ] Replace historical acceptance tests with explicit rejection tests for
  every non-current package version.
- [ ] Search runtime, editor, tooling, tests, and documentation for retired v2
  branches and remove stale compatibility claims.
- [ ] Move lasting tool operation and early-development compatibility rules to
  their owning documentation domains.
- [ ] Complete the required focused validation, full `all` build, editor smoke
  test where applicable, plan handoff, and plan lifecycle updates.

#### Acceptance Gate

- The engine reads and writes only DAST v3, CI rejects every other package
  format, and no v2 migration or reader code remains.
- The unified audit/migration tool remains ready to host the next temporary
  exact-edge migration without carrying the completed edge.
- All validation passes from a clean checkout and the lasting documentation no
  longer depends on this active plan for operational truth.

Each completed stage ends with a compact handoff recording its baseline commit,
working set, key symbols and decisions, open questions, and validation outcome.
Each stage lands as an independent local commit under the repository handoff
rules.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Parser and help | Audit/migrate grammar, explicit `--apply`, invalid combinations, stable exits |
| Read-only boundary | Audit and migrate dry-run preserve authored files, caches, and registry state |
| Discovery | Engine/project/auto-scan mounts, missing mounts, stable ordering, path validation |
| Compatibility | Current, unknown field, signature mismatch, unavailable class, unsupported format, corrupt graph and I/O failure |
| Migration registry | Exact edges, complete chain, missing/ambiguous/cyclic chain rejection, stable handler IDs |
| Determinism | Repeated plans, reports, and serialized outputs are byte-identical |
| Concurrency and freshness | Mid-run source changes become stale and prevent apply |
| Transaction | Stage failure, partial publish failure, verification failure, rollback and recovery |
| Cancellation | Immediate during scan/stage; deferred to a consistent state during publish/rollback |
| Corpus | Every tracked package moves v2 to v3 and passes a fresh audit |
| Retirement | Current-only CI gate passes; old versions reject; v2 implementation search reaches zero |
| Integration | Focused AssetCore, Engine asset, DurinDevTool, editor audit/workflow, cook, and package tests |
| Final repository | Required full `all` build and applicable editor startup smoke test per build documentation |

## Definition of Done

- Developers use one asset tool for both compatibility inspection and explicit
  migration, with shared discovery and classification semantics.
- Audit and default migration planning are provably read-only; only `--apply`
  can mutate authored packages.
- Migration is deterministic, lossless-only, stale-safe, transactional, and
  followed by a clean independent audit.
- All 18 currently tracked packages are DAST v3.
- CI enforces the single current authored baseline.
- DAST v2 reading, fixtures, migration handlers, and compatibility branches are
  removed after the repository rewrite.
- Lasting runtime and development documentation states the early-development
  baseline and the process for changing it.
- Required validation, stage handoffs, local commits, and plan lifecycle fields
  are complete.

## Deferred Follow-ups

- DAST v4 layout and compact serialization; it should consume this tool but
  remains owned by its roadmap and a future bounded implementation plan.
- External-project compatibility windows, long-term migration bundles, release
  deprecation policy, and downloadable standalone tooling after Durin makes an
  external compatibility commitment.
- Editor-side writable batch migration UI. The editor audit window remains
  read-only until a separate workflow demonstrates a need beyond the CLI.
- Cooked-output migration; cooked data remains disposable and should normally
  be regenerated from current authored packages.

## Related Documentation

- [Build and Run](../Development/Build/BuildAndRun.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Versioning](../Runtime/Assets/Versioning.md)
- [File I/O](../Runtime/Core/FileIO.md)
- [Compact Asset Serialization Roadmap](../Roadmaps/CompactAssetSerialization.md)

## Related Code

- `Engine/Source/Programs/DurinAssetAudit/Private/AssetAuditMain.cpp`
- `Engine/Source/Programs/DurinAssetAudit/CMakeLists.txt`
- `Engine/Source/Runtime/AssetCore/Public/AssetCompatibility.h`
- `Engine/Source/Runtime/AssetCore/Public/AssetSystem.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetCompatibility.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Tools/DurinDevTool/durin_dev_tool/asset.py`
- `Tools/DurinDevTool/schemas/asset-audit-v1.schema.json`
