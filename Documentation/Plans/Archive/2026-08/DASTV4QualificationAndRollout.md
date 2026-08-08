# DAST V4 Qualification and Rollout Plan

Summary: Qualify editor workflows, explicitly migrate the tracked authored corpus to DAST v4, switch ordinary saves, and retire the temporary v3 path.

Last reviewed: 2026-08-09

Status: Archived
Completed: 2026-08-09

## Current Status

Completed on 2026-08-09. Ordinary and bundle saves emit deterministic no-delta
DAST v4, the repository baseline accepts only v4, and production has one
authored reader/writer with no v3 dispatch or built-in migration edge. The
lasting v4-only package and version-policy contracts now reside in Runtime
documentation, and the compact-serialization roadmap is complete. Final
qualification passed focused package, Core object, rendering, baseline, tool,
editor restart/hash, documentation, and full-build gates; all 17 tracked
packages remained byte-identical through the final hidden editor lifecycle.

## Goal

Qualify the mixed-version editor path, explicitly migrate the complete tracked
corpus to v4, select v4 for ordinary saves, and remove the temporary v3 reader
and migration edge only after editor restart and hash/baseline gates pass.

## Scope

- Record the authorized 17-package rollout inventory and pre-migration hashes.
- Qualify editor load, render, save, unload, reload, and restart on mixed content.
- Apply one explicit atomic migration to the complete tracked corpus and review
  every changed package.
- Switch ordinary authored saves and the repository baseline to v4.
- Retire the temporary v3 reader, migration edge, fixtures, and compatibility
  branches after the corpus has no v3 package.

## Non-Goals

- Silent migration during discovery, inspection, loading, or editor startup.
- External-project compatibility promises or downloadable migration bundles.
- Reopening the frozen v4 wire, default, provenance, retention, or codec contract.
- Adding custom Struct codecs without new production audit evidence.

## Design Decisions and Invariants

- Content changes require an explicit reviewed migration apply over the complete
  authorized inventory; discovery and qualification remain read-only beforehand.
- The bundle journal, stale-input checks, compatibility/data-loss gates, and
  registry/cache compensation remain the sole publication transaction.
- Ordinary writer activation follows successful mixed editor qualification and
  corpus migration; v3 retirement follows a clean all-v4 baseline.
- Every failure preserves or restores authored bytes, registry/cache state,
  residency, dirty state, reports, and editor reopenability.

## Current Foundations and Gaps

The completed mixed-version plan owns bounded v3/v4 dispatch, transactional live
graphs, deterministic no-delta v4 migration output, and journal-backed rollback.
The tracked corpus is still v3, the ordinary writer and baseline still select
v3, editor render/save/restart has not been qualified against mixed or migrated
content, and the temporary v3 path remains required.

## Implementation Stages

### Stage 0: Freeze rollout authorization and evidence

- [x] Record the exact tracked-package inventory, Git object hashes, format
  versions, dependency closure, and expected migration selection.
- [x] Freeze editor scenarios, visual checks, backup/rollback evidence, and
  acceptance outputs before any authored byte changes.
- [x] Prove dry-run selection is the complete 17-package corpus and changes no
  file, registry/cache, residency, report, or dirty state.

#### Authorized Inventory

The immutable pre-rollout baseline is commit `cd3f4d9a`. Each row below is one
tracked working-tree package, maps to the shown virtual package, has DAST format
version 3, and matches the recorded Git object exactly. Stage 2 apply is
authorized only if every object hash still matches and one fresh unfiltered
dry-run returns this same ordered 17-package set as `Planned`.

| Virtual package | Tracked file | Git object |
| --- | --- | --- |
| `/Engine/Materials/DefaultMaterial` | `Engine/Content/Materials/DefaultMaterial.dasset` | `ca6838e04f409069f5a14cac2af5e5085072032b` |
| `/Engine/Materials/ImportedSurface` | `Engine/Content/Materials/ImportedSurface.dasset` | `38d88d8ef7ed968019d9d41ccb8d26b254bbc9ea` |
| `/Engine/Models/Box` | `Engine/Content/Models/Box.dasset` | `bc0e347c1ac7913f47382bef01bfd2b2fac22ef9` |
| `/Engine/Models/Sphere` | `Engine/Content/Models/Sphere.dasset` | `8fcb01ba24bcce073460715a09ee52f0a4a75bda` |
| `/Engine/Renderer/DefaultStudioEnvironment` | `Engine/Content/Renderer/DefaultStudioEnvironment.dasset` | `3e284dcc00701068addd72f82862533e7ade9314` |
| `/Game/Levels/NewLevel` | `Sandbox/Content/Levels/NewLevel.dasset` | `7debbfdf9d59e30fb9f3c9f1605435899c821c76` |
| `/Game/Models/VintageLighter/Materials/vintage_lighter` | `Sandbox/Content/Models/VintageLighter/Materials/vintage_lighter.dasset` | `afc8930646dc14d5bd4b9d3c5a5b4b65a96838cd` |
| `/Game/Models/VintageLighter/Materials/vintage_lighter_alpha` | `Sandbox/Content/Models/VintageLighter/Materials/vintage_lighter_alpha.dasset` | `1c85d184dcff51af95ba769cb6af2634e790060d` |
| `/Game/Models/VintageLighter/Meshes/vintage_lighter_1k` | `Sandbox/Content/Models/VintageLighter/Meshes/vintage_lighter_1k.dasset` | `b562fc379a5a922d409e82d0d158851fde460e58` |
| `/Game/Models/VintageLighter/Textures/vintage_lighter_diff_BaseColor` | `Sandbox/Content/Models/VintageLighter/Textures/vintage_lighter_diff_BaseColor.dasset` | `8f90516f8be0543531b7b9ee131048d323186de1` |
| `/Game/Models/VintageLighter/Textures/vintage_lighter_diff_Opacity` | `Sandbox/Content/Models/VintageLighter/Textures/vintage_lighter_diff_Opacity.dasset` | `8070d70afa951cafec4ec2009c30d64684078ea6` |
| `/Game/Models/VintageLighter/Textures/vintage_lighter_metal_vintage_lighter_rough_Metallic` | `Sandbox/Content/Models/VintageLighter/Textures/vintage_lighter_metal_vintage_lighter_rough_Metallic.dasset` | `2227a76abbf419c82705eaae9afbc79b3a034b39` |
| `/Game/Models/VintageLighter/Textures/vintage_lighter_metal_vintage_lighter_rough_Roughness` | `Sandbox/Content/Models/VintageLighter/Textures/vintage_lighter_metal_vintage_lighter_rough_Roughness.dasset` | `539a23baf0ae9f867673914cef25e75a5d009e54` |
| `/Game/Models/VintageLighter/Textures/vintage_lighter_nor_gl_Normal` | `Sandbox/Content/Models/VintageLighter/Textures/vintage_lighter_nor_gl_Normal.dasset` | `5dc5a3021e9ba3221b4d477608aa1c3841facad9` |
| `/Game/Models/VintageLighter/vintage_lighter_1k_Import` | `Sandbox/Content/Models/VintageLighter/vintage_lighter_1k_Import.dasset` | `fc5df50581bc3e7da3e53f57889492acdb26f696` |
| `/Game/Textures/TEXCUBE_PureSky_512x512` | `Sandbox/Content/Textures/TEXCUBE_PureSky_512x512.dasset` | `465ebaa7750f4aca6d046fc0369aca26f22a9a82` |
| `/Game/Textures/TEX_StoneHead` | `Sandbox/Content/Textures/TEX_StoneHead.dasset` | `98ae6839088b8d81b739854532a348defde557f2` |

Single-package dry-runs freeze the migration-relevant hard dependency edges as
`NewLevel -> vintage_lighter_1k -> vintage_lighter -> ImportedSurface` and
`vintage_lighter_alpha -> ImportedSurface`. Every other single-package plan is
ready without an additional selected package. The unfiltered complete-corpus
plan contains both mounts, returns `Ready`, and therefore closes every one of
these edges inside the authorized set.

#### Frozen Qualification and Rollback Protocol

| Scenario | Required check and retained evidence |
| --- | --- |
| Mixed startup | Use isolated temporary v4 copies plus the unchanged tracked v3 corpus; open `NewLevel`, then the Vintage Lighter mesh, both materials, representative textures, and the engine default material/environment. Retain audit output and editor log proving both versions loaded with no implicit tracked write. |
| Visual parity | Capture matched before/after views with the same document, camera, framing, environment, and material assignment. Geometry, material slots, base color, opacity, metallic, roughness, normal response, and studio environment must show no missing/checker/error fallback or visible version-dependent change. |
| Save/reload | No-op save, unload, and reopen the representative level, mesh, material, and texture graph. Ordinary saves must remain v3 during Stage 1; tracked hashes, registry/reference projections, residency, dirty flags, and load reports must return to their pre-scenario state. |
| Restart | Run a normal editor close/reopen and the documented unattended `--hidden-window --exit-after-ticks=<positive-count>` lifecycle. Retain clean-exit logs, post-restart audit, hashes, and absence of mutation-recovery sidecars. |
| Failure compensation | Exercise existing load/save/migration phase injection around the same mixed graph. Retain the failing diagnostic plus before/after hashes and registry/cache, residency, report, and dirty-state assertions. |
| All-v4 repeat | After the reviewed Stage 2 apply, repeat every scenario against the complete migrated corpus and compare the same visual captures and state evidence. |

The baseline commit and Git objects above are the durable backup. Migration
apply must use the package-bundle journal; a failed apply is accepted only when
automatic compensation restores every pre-apply content hash and runtime-state
snapshot and leaves no recovery locator or sidecar. Do not restore Git objects
while a journal exists: first complete journal recovery and review its result.
A technically successful apply that fails package or visual review is reverted
as a separate reviewed content transaction from the baseline Git objects, then
re-audited before another apply is considered.

The acceptance evidence for Stages 1 and 2 is fixed as: command line and exit
status, schema-versioned audit/migration JSON, exact pre/post content hashes,
registry/reference cache fingerprints, residency/report/dirty-state assertions,
editor logs, matched visual captures, and a recursive absence check for
migration manifests, staged/rollback files, and recovery locators.

#### Acceptance Gate

- The reviewed inventory, dry-run, hashes, and rollback protocol identify the
  exact atomic content change and no authored byte has changed.

#### Stage 0 Handoff

- Baseline: `cd3f4d9a` (`fix(asset): decouple baseline from migration writer`).
- Working set: this plan plus read-only audit/migration commands and Git/content,
  registry/reference-cache, and recovery-sidecar snapshots. No tracked
  `.dasset` changed.
- Key decisions: only the exact 17-row inventory above is authorized; the
  unfiltered migration plan is the apply selector; Stage 1 uses isolated v4
  copies and may not change the tracked corpus; Stage 2 must revalidate every
  recorded Git object and dry-run result immediately before apply.
- Dependency closure: `NewLevel -> vintage_lighter_1k -> vintage_lighter ->
  ImportedSurface` and `vintage_lighter_alpha -> ImportedSurface`; all edges
  terminate inside the complete selected corpus.
- Open questions: none for Stage 1. Visual evidence uses matched editor views;
  unattended restart uses the documented hidden-window lifecycle and logs.
- Validation: audit reported 17/17 `Ready`, `Compatible`, `Current` v3 packages
  with no findings; dry-run reported `Ready`, 17 planned lossless
  `durin.package.3-to-4` steps, zero blocked/failed/migrated packages, and no
  changed paths. A same-invocation before/after snapshot of 17 authored files
  plus four registry/reference cache files found no length, timestamp, or
  SHA-256 change; Git status and recovery-sidecar sets stayed empty, and all 17
  working-tree Git objects still matched `HEAD`.

### Stage 1: Qualify the mixed-version editor path

- [x] Exercise representative v3/v4 dependency graphs through editor open,
  render, save, unload, reload, and hidden-window restart.
- [x] Verify read-only startup/audit paths and ordinary saves do not migrate
  packages, and injected failures preserve the previous editor/runtime state.

#### Acceptance Gate

- Mixed content renders and survives save/reload/restart without implicit
  migration, stale cache reuse, registry drift, or state leakage.

#### Stage 1 Handoff

- Baseline: `86ffe66e` (`docs(asset): freeze v4 rollout authorization`).
- Working set: `AssetSystem.cpp`, package save/mixed-version tests,
  `EditorTextureSmokeTests.cpp`, the EditorRendering test linkage, the Runtime
  package contract, and this plan. The temporary qualification package was
  removed; no tracked `.dasset` changed.
- Key symbols and decisions: `ValidateOrdinarySaveVersion` guards both
  `FAssetManager::SavePackage` and `SavePackagesAtomically`. Existing packages
  whose registry format differs from `OrdinaryAssetPackageWriterVersion` fail
  with `UnsupportedVersion` before serialization, staging, publication,
  registry mutation, or dirty-state clearing. New packages remain writable in
  the ordinary version; explicit migration remains the only format transition.
- Editor/render qualification: an isolated real texture/material/mesh graph was
  explicitly migrated to v4, opened through `FMaterialEditorModule`, rendered
  through `FStaticMeshSceneProxy`, unloaded/reloaded, and rendered again with
  the same texture binding. Material Editor save returned false, preserved v4
  bytes and registry version, and left the document dirty.
- Restart qualification: a temporary lossless v4 copy of `TEX_StoneHead` was
  added beside the unchanged 17-package v3 corpus. Three consecutive documented
  hidden-window editor lifecycles exited cleanly. The first refreshed obsolete
  registry/reference cache headers; the next two were warning-free, and the
  final restart changed none of 18 package or four cache SHA-256 hashes. The
  post-restart audit reported all 18 packages `Ready`, `Compatible`, and
  `Current`; the temporary package and empty directory were then removed.
- Failure/state qualification: existing mixed decode/dependency and atomic
  bundle phase injection remains green. The new single/bundle save test proves
  v4 bytes, v3 peer bytes, registry contents/revision, residency, load report,
  and dirty flags survive rejected ordinary saves with no staging sidecars;
  manager restart then reloads the same clean v4 value.
- Open questions: none for Stage 2. Immediately before apply, recheck every
  Stage 0 Git object and require the same unfiltered 17-package dry-run.
- Validation: `AssetPackageTests` passed 126/126; `EditorRenderingTests` passed
  34/34; `EditorAssetWorkflowTests` passed 78 with one skipped and no failures;
  the mixed audit and three hidden-window editor runs passed. Full build,
  restored 17-package baseline/hash, and documentation gates are rerun on the
  final Stage 1 working set before commit.

### Stage 2: Migrate the tracked corpus atomically

- [x] Apply the authorized complete-corpus migration once through the bundle
  journal and review every changed `.dasset` plus the migration report.
- [x] Verify every package is deterministic clean v4, dependency-closed, and
  byte-identical after a no-delta v4 resave.
- [x] Repeat editor load/render/unload/reload/restart qualification on the
  migrated corpus.

#### Acceptance Gate

- All 17 tracked packages are reviewed deterministic v4 outputs, the v3 corpus
  is empty, and editor/runtime qualification passes with no sidecars remaining.

#### Post-Migration Review

| Virtual package | Bytes | Git object | SHA-256 |
| --- | ---: | --- | --- |
| `/Engine/Materials/DefaultMaterial` | 10869 | `f71445f6a935961d3a418762b45e7ab4ace0f521` | `00b5143a8e3f50474a916ec3efc1951908d10c2bfc72a7954844ea9351d0a2fd` |
| `/Engine/Materials/ImportedSurface` | 10869 | `a41ea04a80c4d071f70452a3ddbf14384767c45a` | `68db0ad016d12d75b157e8063e0169755f0cc34252eeb7de9b91373449cc07e3` |
| `/Engine/Models/Box` | 1103 | `5a26ce43a04cbcb5079d7f5dd4f9c392ece1d09d` | `06e46eafe1e22efed76f06de5e52fb73c524432339991aec7384ac0a063f38c6` |
| `/Engine/Models/Sphere` | 1109 | `6deb234d7a040624d487bfbe6bd295f9aff26d2c` | `b7596ac5dc2fa6aa54a06b25a7c6061a88a138303fe216fc02d3809549997c71` |
| `/Engine/Renderer/DefaultStudioEnvironment` | 497 | `04cc6fe156af249fbdc6c4bf34367df55c90f9b4` | `38a1333ea6c6a849a4232a5fd8da76b6d945a8b311f8cde140ccd787f9a3e43b` |
| `/Game/Levels/NewLevel` | 2315 | `b5def496d8c6478afede71c0268eab42501a5d4d` | `25bcfed521158ba284839ea70077a0406ecc0c78267b4922e652bb448ac6a856` |
| `/Game/Models/VintageLighter/Materials/vintage_lighter` | 4383 | `1f035ec93cf94ce0844ce62b24413351d6c6bb19` | `1be286a04cdef319d5dac750aface4d2e6648f91093270e626e1c5643cecd894` |
| `/Game/Models/VintageLighter/Materials/vintage_lighter_alpha` | 5068 | `5fbf61aac54f1dc5af9861fc965699d9f830c654` | `ffe7f5710605add66d96c6729d43f09a816e9dc08d8cb3ed72808fa5a8150dbc` |
| `/Game/Models/VintageLighter/Meshes/vintage_lighter_1k` | 1333 | `d6700d02ee1d13f820fd5f13229d7ac6b8ea7aab` | `9416da393bcbf721b6b960ea0c4e86633b85f773a2b0adf7fbaf5dbbd9620404` |
| `/Game/Models/VintageLighter/Textures/vintage_lighter_diff_BaseColor` | 1300 | `a2ae53485999e08c37316d3b3ffd3401fde2343a` | `71d49e709a65c78df8ad2da252c63bf75c5ebad161440499d430d71c36d16c06` |
| `/Game/Models/VintageLighter/Textures/vintage_lighter_diff_Opacity` | 1361 | `762ec54814ceb1908a5ed2bdc73de7fa55d263b4` | `4df60ff0edc740d819924577d16db359adb329b78b9bf69bac812978e70d084c` |
| `/Game/Models/VintageLighter/Textures/vintage_lighter_metal_vintage_lighter_rough_Metallic` | 1387 | `da6aca364c7075babdec4b25e99e4fb05d990144` | `aa8a31b4bd18f7d7ab18c6be9000eb33d9f6f0a85d8836c80a519e09e4e366c0` |
| `/Game/Models/VintageLighter/Textures/vintage_lighter_metal_vintage_lighter_rough_Roughness` | 1386 | `b2d637d80ed8df466025465425c0ad25cda0e0a0` | `a520c8736e5df7d3404f464b57495b9688e7d4d9bf74fffdce8c7b5ac88acfbb` |
| `/Game/Models/VintageLighter/Textures/vintage_lighter_nor_gl_Normal` | 1300 | `a5a5e5e4858f98923445aa4c89da8cab2bd48ad7` | `59c9ff899c711f7f8cb3a1dcb19e8c41db8dd751d6943c97016ec3475dcdae96` |
| `/Game/Models/VintageLighter/vintage_lighter_1k_Import` | 3702 | `a2da57cec2a86ea15cb6a15227232d142b0c9272` | `8678d0f1c7ccb751699cbf8363c9c9e15312443168aa09a66aa89b23cddc7864` |
| `/Game/Textures/TEXCUBE_PureSky_512x512` | 1260 | `dc7c9c25cbf30833d7ce0f2f9b6a22faed644dc0` | `7b0f08f11f4f784dceb5f64eab8f8a5cc8f52a55518cb6165aeffdbd5f7ec106` |
| `/Game/Textures/TEX_StoneHead` | 1256 | `5128ad36d6f0fd926d29d65e86ac36e2dfb19a10` | `dda376e6e487ed1a497a9214304186ff50cff123158bdc211993a1615a463d82` |

#### Stage 2 Handoff

- Baseline: `fcf03266` (`fix(asset): block implicit package version saves`).
- Working set: the 17 frozen authored `.dasset` packages above;
  `AssetPackageArchive.cpp`, `AssetPackageV4Reader.cpp`,
  `AuthoredOverrideLedger.cpp/.h`, `DefaultDeltaPlan.cpp`,
  `PackageTests.cpp`, and this plan.
- Key decisions: associate delta plans by object identity, remap captured internal
  references to canonical v4 object IDs, and restore each object's complete
  authored ledger through one frozen-shape validation and atomic replacement.
  Empty-ledger replacement retains its no-capture fast path.
- Apply history: the first attempt failed before publication because delta and
  Archive object orders differed and left all baseline hashes unchanged. The
  next reviewed transaction exposed an internal-reference class mismatch during
  editor qualification; all 17 packages were restored from the frozen Git
  objects and re-audited before the final corrected bundle transaction.
- Determinism/review: final apply migrated 17/17 with no blocked, failed, or
  rolled-back package. Apply decoded each output and compared two no-delta v4
  writes byte-for-byte before publication. Audit reports 17/17 v4 with no
  findings; a repeat plan reports 17 skipped and no changed paths; dependency
  closure remains the Stage 0 complete-corpus closure; no migration sidecar or
  recovery locator remains.
- Editor/runtime: after batching ledger restoration, `EditorRenderingTests`
  passed 34/34 in 1.423 seconds. Consecutive hidden editor lifecycles completed
  in 3.51 and 2.92 seconds with no error, default-level, class-mismatch, or
  Archive failure log entry; the second changed none of 17 package plus four
  cache length/SHA-256 fingerprints. Final runtime smoke completed in 2.86
  seconds with zero log findings.
- Validation: `CoreObjectTests` passed 91/91. `AssetPackageTests` passed the 125
  Stage 2-applicable tests; the two intentionally excluded repository-v3
  measurement/budget tests remain red until Stage 3 switches the baseline and
  retires v3 fixtures. Full `all` build passed. Open questions: none; Stage 3
  must update the ordinary writer/baseline before treating the all-v4 corpus as
  the repository baseline.

### Stage 3: Activate v4 ordinary saves and retire v3

- [x] Select v4 for ordinary authored saves and update the repository baseline.
- [x] Remove the temporary v3 reader, exact migration edge, obsolete dispatch,
  fixtures, and compatibility branches after the all-v4 gate passes.
- [x] Prove new and repeated saves are deterministic v4 and unsupported versions
  fail before mutation or publication.

#### Acceptance Gate

- V4 is the only authored reader/writer and baseline; no production v3 path or
  tracked v3 package remains.

#### Stage 3 Handoff

- Baseline: `53dd3fd4` (`feat(asset): migrate tracked corpus to DAST v4`).
- Working set: package version policy, Archive v4 integration, `AssetSystem.cpp`
  v4 save/rewrite paths, built-in migration registration, package/render tests,
  DurinDevTool asset baseline and tests, build documentation, and this plan.
  The 17 tracked `.dasset` files did not change.
- Key decisions: ordinary serialization uses the qualified v4 Archive writer in
  `NoDelta` mode so Stage 2's complete authored ledgers remain byte-stable.
  `SelectAssetPackageReader` accepts only v4. Relocation, redirector Fix Up, and
  cook canonicalization decode, rewrite, and canonically re-encode v4 values;
  unsupported registry versions still fail before serialization/publication.
- Retirement: the v3 reader/writer, reader enum and policy fingerprint entry,
  exact `durin.package.3-to-4` edge, v3 measurement and feasibility fixtures,
  mixed-version tests, and obsolete compatibility-byte tests are removed.
  Generic migration planning remains available, but the built-in registry is
  intentionally empty after the repository reaches its terminal version.
- Determinism/runtime: the final ordinary-save test proves two serializations
  and a repeated disk save are byte-identical v4, then injects registry version
  3 and proves rejection preserves bytes, revision, metadata, and dirty state.
  The editor material graph renders, reloads, and saves identical bytes. A
  final documented hidden editor lifecycle completed in 2.15 seconds and changed
  none of 17 tracked package SHA-256 hashes; its log had no error finding.
- Validation: `AssetPackageTests` passed 93/93, `CoreObjectTests` 91/91, and
  `EditorRenderingTests` 34/34 in 1.16 seconds. DurinDevTool asset tests passed
  23/23 with a workspace-local pytest temp root; the unrestricted tool suite's
  system-temp ACL failure is environmental. Asset baseline reports 17 current
  v4 packages, and the full `all` build passed. Open questions: none.

### Stage 4: Complete rollout qualification

- [x] Run focused codec, package, compatibility, registry/cache, editor,
  rendering, baseline/hash, documentation, and full-build validation.
- [x] Move final v4-only policy and retirement contracts into Runtime docs and
  complete the serialization roadmap.

#### Acceptance Gate

- The all-v4 repository passes editor load/render/save/restart, deterministic
  resave, hashes, baseline, focused suites, documentation, and full build.

#### Stage 4 Handoff

- Baseline: `5991a0b9` (`feat(asset): activate DAST v4 package saves`).
- Working set: `Documentation/Runtime/Assets/AssetPackages.md`,
  `Documentation/Runtime/Assets/Versioning.md`, the compact asset serialization
  roadmap, and this plan. No production code or tracked package bytes changed.
- Lasting contracts: DAST v4 is the sole authored reader, ordinary writer, and
  repository baseline. Ordinary and bundle saves use no-delta v4; unsupported
  versions fail before interpretation or mutation. Generic explicit migration
  remains bundle-atomic and journal-compensated, but the built-in migration
  registry is empty until a future exact edge is deliberately registered.
- Validation: `AssetPackageTests` passed 93/93, `CoreObjectTests` 91/91,
  `EditorRenderingTests` 34/34, and DurinDevTool asset tests 23/23. The baseline
  reports 17 current v4 packages. A hidden three-tick editor lifecycle completed
  in 2.45 seconds; all 17 tracked SHA-256 hashes were unchanged and its log had
  zero error findings. The full `all` build passed. Documentation validators
  and final diff checks are recorded in the completion commit. Open questions:
  none.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Authorization | Exact 17-package inventory, hashes, dry-run selection, reviewed apply |
| Editor | Mixed and all-v4 load, render, save, unload, reload, restart |
| Migration | Complete dependency closure, journal compensation, no sidecars |
| Determinism | V4 decode/re-encode and repeated ordinary saves are byte-identical |
| Compatibility | Unknown retention and data-loss consent remain fail-closed |
| Registry/cache | Versioned fingerprints, full/incremental parity, no stale reuse |
| Retirement | No tracked v3 package or production v3 reader/migration branch remains |
| Qualification | Focused suites, baseline/hashes, docs, and full `all` build |

## Definition of Done

- The complete tracked corpus is explicitly migrated and reviewed as v4.
- Editor load/render/save/restart and deterministic resave pass on all-v4 content.
- Ordinary saves and the repository baseline select v4.
- The temporary v3 reader and migration edge are removed.
- Lasting v4-only contracts reside in Runtime documentation and the roadmap is complete.

## Deferred Follow-ups

- External release compatibility and downloadable migration bundles.
- Custom Struct asset codecs only if a production audit establishes need.

## Related Documentation

- [Compact Asset Serialization Roadmap](../../../Roadmaps/Archive/2026-08/CompactAssetSerialization.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Versioning](../../../Runtime/Assets/Versioning.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetMigration.cpp`
- `Engine/Source/Runtime/AssetCore/Public/AssetPackageVersionPolicy.h`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Content/Materials/DefaultMaterial.dasset`
