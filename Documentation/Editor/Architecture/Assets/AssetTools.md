# Editor Asset Tools

Summary: Define editor creation, duplication, rename/reference handling, package save/reload policy, feedback routing, and the native persistence boundary.

Modules: AssetTools, DurinEd, AssetMaintenance, MainFrame, Core, CoreDObject, ApplicationCore

Last reviewed: 2026-11-16

## Ownership

`IAssetTools` owns creation, explicit-target duplication, unique naming and the
object rename, referencer fix-up and standalone import entrypoints. `ObjectTools`
owns batch duplicate naming, deletion and presentation policy. Package saving belongs to
`DEditorLoadingAndSavingUtils` and `FEditorFileUtils` in `FileHelpers.h`;
`DPackageTools` owns unload/reload policy. These services share the AssetTools
module without making DurinEd depend on AssetTools.

AssetMaintenance's PlanAssetCanonicalResaves/ApplyAssetCanonicalResaves own explicit canonical resave batches.
Canonical compatibility inspection and resave are not a mode of ordinary saving.
The native package writer, graph replacement and reload mechanisms retain their
typed errors, write effects and retirement ownership.

Asset editor Save/Discard continues through each session's document model over
these persistence services. The shared `FTextureEditorToolkit` implementation
handles both 2D and volume texture sessions, retaining separate family factories,
toolkit IDs and per-document preview selections. Volume axis/slice controls and
2D Source/Built controls remain family-specific; close, save, reload, resource
remapping and command ownership belong to the shared controller. Controller
consolidation changes no package format or persistence-service ownership. See
[editor ownership](../Framework/AssetEditors.md#editor-instance-and-toolkit-ownership).

Texture toolkit logical close cancels only unobserved compilation while its
owner is still available, releases previews and clears retained texture state.
An externally retained closed toolkit performs no asset compilation cancellation
when eventually destroyed; a reopened editor's work belongs to that new session.

## Creation And Duplication

`CreateAsset(AssetName, PackagePath, AssetClass, Factory, CallingContext,
bOverwriteExisting)` takes an asset name and logical destination directory.
It constructs Public, Standalone, Transactional assets in a prepared package,
validates the factory product and publishes the graph before returning it Dirty.
AssetClass may be inferred from an explicit factory. CallingContext is an FName
passed to FactoryCreateNew; it is not an arbitrary DObject context or flags tail.

Creation rejects occupied destinations unless overwrite is explicitly requested.
Same-class replacement prepares a separate graph, preserves sibling assets and
validates the original package revision before replacing it. Failed factories,
stale identities, incompatible classes and unclaimed native strong owners leave
the original graph intact. Replacement does not force destruction of an active
document or discard the old graph to make space. Creation performs no saving.

Factory discovery returns immutable class-default metadata. Mutable execution
uses a transient invocation instance, or the caller's explicitly configured
instance retained for that operation. FactoryCreateNew receives feedback and
CallingContext; FactoryCreateFile receives filename, parameters, feedback and a
cancellation output. Factory code keeps domain diagnostics locally and reports
them through feedback; callers never execute or const-cast discovered defaults.

`DuplicateAsset(AssetName, PackagePath, OriginalObject)` duplicates a live exact
top-level asset into an explicit new identity. It does not choose another target
when that identity is occupied. Object-graph duplication remaps inner references
and preserves cross-package references. Redirectors and unpublished/incomplete
sources reject. Success returns a live Dirty, NewlyCreated object.

`CreateUniqueAssetName` sanitizes the name and uses UE-style integer suffixes,
including zero-padded suffixes and all-numeric names. It checks catalog, residency
and physical occupancy without reserving the result. `ObjectTools::DuplicateObjects`
uses that policy, retains source lifetimes and returns successful objects through
OutNewObjects. A dialog request requires a registered destination-prompt host.
MainFrame uses a native folder chooser; the chosen directory still passes normal
mount and collision validation. A missing host never implies consent.

Durin has no source-control provider, so duplication follows UE's disabled-provider
branch and leaves copies unsaved. Browser duplicate/copy does not force a save.
An explicit save failure retains the copy for ordinary retry, without another
duplication or graph discard.

## Rename And Referencer Fix-Up

`RenameAssets(span<const FAssetRenameData>)` is the synchronous editor rename
and move entrypoint. Data owns a weak Asset, NewPackagePath directory, NewName,
OldObjectPath, NewObjectPath and bOnlyFixSoftReferences. Constructors accept a
live object plus directory/name, or explicit soft object paths; the manager fills
missing identity fields. Normal form produces Directory/NewName.NewName.
Explicit paths can retain a non-leaf object name. Physical rename requires a
top-level object; soft-only repair also accepts subobject paths and retains
matching descendant suffixes. Deferred/dialog and localized rename are not exposed.

The private manager retains input objects and package graphs, checks complete
reference discovery and writable mounts, then prepares each item independently.
Dirty/new objects keep their live authored values and hard-pointer identity.
Moving one export into another package uses checked reparenting and export-index
maintenance; its siblings stay in their original package. A sole unsaved object
without referencers or external stores can carry its package to the new identity.
Case-only changes update display spelling without creating a redirector.
Occupied targets reject before identity mutation. Preparation callbacks cannot
silently replace an edited source; the source revision, mounted destinations and saved source fingerprint are
revalidated before identity mutation.

Disk referencers load while the old identity still resolves. Hard pointers remain
valid across the move, soft references update exact object identities and suffixes,
and affected writable referencers save with their current authored values.
Unrelated resident packages are inspected for matching soft values, never saved
as an implicit global batch. Saved sources retain aliases so unavailable/read-only
referencers remain resolvable. Rename saves the new destination before old aliases;
a save failure leaves the live new identity and authored values available for retry.
An already applied identity does not produce another rename event on retry.
Retry includes clean but projection-fenced alias packages; clearing Dirty after a
physical commit does not establish that Registry publication succeeded.
Owned payloads prepare before identity mutation and retain source files until the
new package and references are committed; failed preparation removes new copies.
Uncertain/partial persistence still requires native diagnostics and inspection.

Engine publishes `OnAssetRenamed(const FAssetData&, const FSoftObjectPath& Old)`
after committing live identity, outside Registry locks. The new record includes
primary ID and bundle metadata; disk reconciliation does not repeat this event.
Document/recent-asset/material-profile consumers match the exact old asset and
preserve descendant suffixes. Package-only workspace keys move only when that
package actually moved; Level Editor separately tracks its moved Level asset.
Primary Asset catalog observers update their selected exact asset record.
AssetTools' `OnAssetPostRename` reports committed identity changes through a
synchronous span of owned normalized request values. Save diagnostics remain
separate from identity events.

`FixupReferencers` accepts live redirector objects and returns void;
`IsFixupReferencersInProgress` reports the synchronous operation. Delete and Leave
FixedUpRedirectors are supported; checkout prompts have no provider work in this
branch. Missing discovery, nested identity work and failed reference saves reject
with feedback. Loaded Dirty referencers repair and save their current values.
Individual redirector removal retains sibling exports and checks exact persistent
incoming references, including read-only referencers. Native owners and live hard
references can veto deletion. Whole selected alias packages with one destination
package use private staging, external-store revalidation/application/verification,
reference closure checks and native retirement before deletion. Browser selections
carry exact object paths through the provider. Required aliases survive failures.

The byte-level package relocation and alias fix-up implementations are private
mechanisms. Native characterization uses explicitly named MutationTesting helpers;
production consumers cannot invoke the removed relocation/fix-up request APIs.
External stores still own package-valued occurrences: an unambiguous pure-alias
package can be rewritten during rename/fix-up, while a mixed package must retain
its alias if a store cannot distinguish the selected export.

## Deletion

`ObjectTools::DeleteAssets` loads exact selected exports, including redirectors,
without resolving them to their destinations. `DeleteObjects` and `DeleteItems`
use `FAssetDeleteModel`; object counts describe retired selected objects, while
folder counts describe actual directory removals. `DoDelete` reports acceptance
of an action, independently of persistence success and the deleted-object count.
A failed or completed attempt cannot run again. Diagnostics retain write failures.

The model advances through Waiting, StartScanning, Scanning, UpdateActions and
Finished on the owner thread. Pending assets retain their exact objects. Native
read-only traversal supplies weak referencers, reference counts and property
descriptors; strong/function owners remain separate blockers. Only the model's
exact strong registrations are excluded. Open documents, open levels, roots,
loading packages, external references and unsafe files still block deletion.

Selected Dirty assets may be deleted with explicit loss of their edits. Selecting
one export preserves unselected siblings and their live identities; the surviving
package is persisted and its prior Dirty state is retained. An unused redirector
may be deleted alone; deleting a target still checks incoming aliases and other
references. No deletion path forces garbage collection. Retired package identities
are withdrawn before deferred collection so a failed physical removal can be
reloaded safely.

Confirmation belongs to the model's host. MainFrame installs the interactive
prompt; a missing required host rejects. Passing `bShowConfirmation=false` skips
presentation while retaining admission and final-validation checks. File owns
ordinary-file and mixed-folder scope, byte identities and partial I/O facts.
Those records are private implementation details. Companion registration remains
in `AssetTools/AssetDeleteExtensions.h`; Engine retains checked removal primitives.

## Saving And Feedback

`SavePackages(Packages, bOnlyDirty)` filters null, non-asset and unpublished
packages, deduplicates identities and optionally filters clean packages. Empty
filtered work succeeds. It retains packages and exports through writer and
feedback callbacks, continues after independent failures and reports false if
any selected write or Registry publication fails.

`FScopedPackageSaveObserver` receives this lexical batch's actual committed
package paths, including content committed with Registry publication pending.
The observer is reserved before a save prompt and detached while prompting or
writing; nested saves need their own observer and cannot enter the caller's
notification list. Browser observers attach after loading, immediately around
the selected save. Diagnostics do not
stand in for physical persistence evidence. Browser saves translate their own
observed paths into updates; native write effects remain at the writer boundary.

`FEditorFileUtils::PromptForCheckoutAndSave` distinguishes PR_Success,
PR_Failure, PR_Declined and PR_Cancelled. A declined save allows the caller to
continue without saving; cancellation aborts the requested action. Failed
packages are returned separately and earlier file commits remain valid.
Dialog entrypoints use a registered host; missing interaction cancels before
writing. The current MainFrame host confirms the filtered batch through a native
dialog; subsequent retries use ordinary Save. No checkout or source-control
state is fabricated when no provider exists.

The UE-shaped parameters retain checkout and explicit-save intent. With no
source-control provider or external-world save preparation in this branch,
`bAlreadyCheckedOut` and `bIsExplicitSave` cause no additional work. The prompt
host receives those fields; it cannot claim checkout or preparation occurred.

`FScopedFeedbackContext` in Core overrides `FFeedbackContext::Get()` on the
current thread and restores the previous context on lexical exit. It neither
propagates to workers nor changes mutation permission. Feedback adapters capture
their fallback before installing the scope so a missing sink cannot recurse into
itself. Typed errors format at the owning presentation boundary.

## Package Reload And Cleanup

PackageTools unload keeps its dirty and transaction-buffer policies. Reload
filters invalid/unsaved inputs, checks the retained identity after any prompt,
coordinates transaction history and waits for native reload retirement before
returning. Interactive Dirty reload requires a positive host decision;
AssumePositive and AssumeNegative are explicit caller policies. Protected native
owners can still reject replacement. Editor document Save/Discard continues to
use native Engine mechanisms and document participants from DurinEd.

Failed newly created packages can be explicitly unloaded with dirty loss allowed
after their caller releases ownership. That cleanup is separate from restoring
saved content through reload. IAssetTools exposes no DiscardPackage,
RecoverPackages or save request API.

See [asset mutation](../../../Runtime/Assets/Packages/AssetCatalogAndMutation.md)
for relocation/deletion and native recovery facts, and
[asset residency](../../../Runtime/Assets/Packages/AssetResidency.md) for checked
unloading and GC ownership.
