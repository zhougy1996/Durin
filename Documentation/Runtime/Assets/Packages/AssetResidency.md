# Asset Residency

Summary: Explicit package roots, runtime ownership and editor Standalone retention.
Modules: CoreDObject, Engine, Launch, AssetTools, AssetForgeBuiltins

Last reviewed: 2026-11-10

## Host policy and actual owners

`FAssetRuntimeConfiguration` selects `EAssetResidencyPolicy` independently of the
Authored/Cooked payload domain. Both configuration factories default to Runtime;
Launch explicitly chooses Editor for the editor host before CDO construction.
Policy changes require completing the current runtime's shutdown first.

Runtime loads return observations and do not create an implicit cache root.
Strong reflected/native references, streamable handles and scene/editor owners
keep graphs reachable. A soft reference and its resolved weak cache do not retain
an object. Releasing the last owner permits a later GC; release never promises
immediate destruction. Synchronous and asynchronous request history does not
change this rule.

Editor loading and successful asset publication add an EditorStandalone root to
persistent, non-PIE asset packages. It survives request release and clean saves;
checked package unloading can remove it. Transient/private preview graphs use
their operation/session owners. This represents editing retention explicitly
rather than transferring an incidental load-transaction pin between callers.

CoreDObject's object/package registration remains the sole live identity lookup.
A reachable object keeps its Outer alive; a package retains all its top-level
exports, which retain their ordinary reflected graphs. Consequently, an owner of
one export retains the whole multi-export package. Neither a registry entry nor
a weak load transaction is an additional GC owner.

## Inspectable root reasons

`DPackage::GetResidencyReasons()` exposes the runtime-only root mask. The mask is
projected onto the existing Standalone GC flag; it is not saved or cataloged.

| Reason | Created by | Removed by |
| --- | --- | --- |
| None | Empty package registration or clean runtime loading | No implicit root exists. |
| Unsaved | Explicit MarkAsNewlyCreated/MarkDirty on a registered non-private package | Publication and ClearDirty once neither unsaved condition remains; explicit discard. |
| Explicit | SetStandaloneResidency(true) | Explicit release of package roots or checked unloading. |
| EditorStandalone | Editor host loading/publication | Checked unloading or runtime teardown. |

Saving removes only Unsaved when the captured edit revision is still current;
it preserves independent Explicit/EditorStandalone roots. Repeated dirty edits
advance their existing edit revision. Empty `CreatePackage` registration alone
does not declare a new unsaved asset; creation tools make that declaration after
validating their result. Factory operations temporarily retain their package
while constructors and user callbacks run. Prepared graphs stay private and
operation-owned; publishing a new unsaved graph enables its unsaved root.

`SetStandaloneResidency(false)` explicitly clears all package root reasons.
Ordinary consumers use strong-owner/handle release instead. Tools restore exact
prior reasons when an unload attempt fails, including editor and unsaved roots;
a surviving strong reference does not manufacture a new runtime cache pin.

## Failure, replacement and tools

Failed loading withdraws the failed generation's registration and marks its
graph garbage before returning. Physical collection is scheduled, preserving
the original failure and allowing a same-path retry before GC. Independently
completed dependencies obey their actual owners and are not invalidated with the
failed component. A later GC may collect unowned dependencies and unrelated
clean packages together.

Graph replacement transfers prior Explicit/EditorStandalone reasons, remaps
actual strong/reflected owners and derives Unsaved from the published candidate's
own state. It adds no root to a clean replacement solely because it is new.
Old weak generations cannot withdraw or unload a replacement at the same path.

`UnloadPackage` remains a checked tools operation. It rejects unsaved state unless
explicit discard was selected, checks references, attempts GC and reports InUse
when actual owners keep the package alive. Its global collection may also retire
other unowned runtime packages. Private load transactions retain weak generation
ledgers for retryable tools cleanup and Cook/reload dependency rules; they do not
promise lifetime and have no cache-release adapter.

## UE compatibility boundary

The pointer distinction follows UE's documented
[object-pointer model](https://dev.epicgames.com/documentation/en-us/unreal-engine/object-pointers-in-unreal-engine).
UE describes [RF_Standalone](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/EObjectFlags)
as editing retention without references, and exposes editor
[Standalone restoration](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/UnrealEd/UPackageTools)
during checked unloading. Durin preserves that distinction between editor
retention and runtime ownership.

The package-level reason mask, automatic Unsaved roots, all-export package graph
and exact restore procedure are selected Durin policies. Public UE documentation
does not establish source equivalence for these details; no licensed UE revision
was inspected. This contract claims the documented ownership subset, not complete
UE GC/package/editor implementation parity.

Request state and retention are described in [Asset Streaming](AssetStreaming.md).
