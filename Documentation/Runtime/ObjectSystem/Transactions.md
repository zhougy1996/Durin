# Runtime Transaction Contracts

Summary: Define runtime-neutral transaction capture, custom changes, exact object references, and object notification boundaries.

Modules: Core, CoreDObject

Last reviewed: 2026-11-28

## Core Boundary

`Misc/ITransaction.h` defines the runtime capture interface published as `GUndo`.
SaveObject returns void. Modify reports admission to recording. Failure to serialize a whole-object baseline or final snapshot invalidates the entire recording, including nested edits. ContainsObject queries participation independently.
Core forward-declares object and reflected-property types; it does not include
CoreDObject or an editor module to define a transaction or custom change.
The editor owns the active recorder and application history, as described in
[Transactors](../../Editor/Architecture/Transactions/Transactors.md).

`Misc/Transaction.h` owns `FCommandChange`,
transaction context and operation identity allocation. The public `ITransaction::Apply()` runs a complete replay and returns void. Undo calls `Revert(Target)` and Redo calls `Apply(Target)` with the target resolved by the record. An execution fault terminates replay with the transaction and record diagnostic.
Object listeners receive `ETransactionObjectEventType::UndoRedo` for both directions.
A custom change implements HasExpired when its target or dependencies can
become unavailable. A command reporting expiry makes the entire history entry ineligible for that replay.
Both recording interfaces require a live `DObject* Target` and reject null
before appending a command. `Apply` and `Revert` receive a validated pointer.
`HasExpired` can receive null when the recorded generation is unavailable.
Each custom record binds one explicit data owner. Collector traversal declares
additional lifetime dependencies. An edit spanning several owners can append
one command per owner within the same transaction.

`FTransactionContext` owns context/description strings. Its primary-object pointer
is borrowed and must not outlive the retaining transaction or be dereferenced
after explicit object destruction. CoreDObject-aware owners retain an exact
identity and resolve it when returning context; Core does not implement GC
ownership. A primary object is presentation context, not an implicit modified
participant or affected package.

Transaction IDs remain stable for a history entry. Each recording or replay
operation obtains a fresh nonzero process-wide operation ID. BeginOperation
rejects an already active operation; EndOperation rejects an inactive one.
Allocation never wraps into a reused identity; zero reports exhaustion.

The capture interface exposes whole-object recording, finalization, already-applied
custom changes and PIE reference inspection.

Runtime types are declared directly in `Durin`; editor namespace compatibility
aliases are removed. `DPackage::IsPlayInEditor` identifies a session package
explicitly, without inferring it from a world type. The editor recorder implements
PIE inspection and transient-entry policy as part of the
[transactor contract](../../Editor/Architecture/Transactions/Transactors.md#undo-barriers-and-pie-references).

Array edits call `Modify()` on the owner before mutation. Its transaction snapshot
covers the array and other transactional fields changed in the same action.
See [array edits](../../Editor/Architecture/Transactions/TransactionRecords.md#array-edits).

## CoreDObject Boundary

`DObject/TransactionObjectReference.h` owns `FPersistentObjectRef` and its
deduplicating reference set. Ordinary references preserve exact slot/generation
identity. Resolve rejects garbage, destroyed or replaced ordinary generations;
`FromKey` always selects this exact category. Like UE, a published CDO or a
default-subobject ancestor instead selects a stable root and an Outer/name chain.
Each step checks exact Class and CDO/default-subobject category; CDOs must be the
class's current published default. Ordinary same-name objects and slot reuse
cannot substitute for those categories. Stored references retain nothing by
themselves; a managed owner must enumerate their live hard edges. Collector
traversal visits the root and cached exact leaf without doing name lookup.
Editor object records and domain commands use this same runtime identity.

`DObject/TransactionSerialization.h` owns the shared transaction-purpose virtual
`Serialize` transfer. `FSerializedObject` stores immutable `SerializedData` with signed 32-bit indices into
`ReferencedObjects` (persistent hard references) and `WeakReferencedObjects`
(exact generation keys). An index of -1 represents null.
`FSerializedObjectInfo` composes capture-time `FTransactionObjectId`, persistent
target/class/Outer/package references and supported flags. The ID owns original
Name and path text without mixing identity with change flags.
`FTransactionSerializationContext` owns schema, versions, frozen field
exclusions and archive-recorded hard/weak reference positions. Capture uses `FObjectArchive`, includes reflected and native
serialized fields, and preserves weak keys and soft paths without
retaining their targets. These helpers never acquire annotations.

`CanTransactObject` admits ordinary objects and Transactional nodes of a current
published class-default graph. Native callers explicitly opt template nodes in
with that flag and use the ordinary Modify/SaveObject path. The const-only default
getter, script/reflected mutation rejection, package persistence exclusions and
template Name/Outer/construction guards remain intact. Template replay also
rejects changes to identity or flags. Actor/component template completion does
not capture instance lifecycle annotations or activate runtime registration.

`ValidateSerializedObject` checks identity, field schema and native field versions
without loading historical fields. Callers preflight before using
`LoadSerializedObject` to transfer the data. The load returns void. Archive errors and native
exceptions propagate with their original reason, including after partial writes.
Editor replay reports the transaction ID, direction and record index, then terminates.
An anonymous object can replay against its exact recorded generation while its
name remains empty and its Outer matches. Transferring an anonymous identity
to a different Outer or replacing a named identity with an anonymous one fails
validation.
Construction flags and physical destruction are irreversible. A whole-object record
captures `bIsPendingKill`. When that bit changes during an edit, the transactor
can resolve the retained generation during replay and apply `ClearGarbage()` or
`MarkAsGarbage()` before loading fields. If a whole-object target still cannot resolve with that access, preflight rejects the entire entry.
Loading a historical Name or Outer gives a live occupant a unique sibling name
before restoring the recorded identity. Undoing a recorded deletion also frees
the name before reviving and registering the retained object.
Once liveness changes begin, a required load or application fault is terminal.
The [serialization contract](Serialization.md) owns archive filtering and
detached parent-baseline requirements.

`DObject/TransactionObjectAnnotation.h` declares supplemental reconstruction data,
collector traversal, reference rebinding and owned allocation accounting. `DObject/TransactionObjectEvent.h` declares owned
identity/change summaries, Finalized and UndoRedo event kinds and shared annotation
ownership. These contracts contain no editor history or presentation service.

DObject supplies safe defaults: no annotation, an empty PostTransacted observer,
and annotation-aware PostEditUndo that forwards to the existing virtual completion
hook. The editor record captures annotations before virtual Serialize at initial,
final and inverse capture; schema-only validation skips that hook. Records own
immutable supplemental snapshots, enumerate their hard references and check
allocation addition for overflow. Editor no-op pruning retains annotated records
even when serialized bytes match.
History holds the annotation in memory and passes it directly to the object's
completion hook on replay. Package reload rebinds its retained references. Engine types own native reconstruction and reconciliation.

`DObject/TransactionDiff.h` owns `FDiffableObject`, `FSerializedTaggedData` and
`DiffUtil::GetDiffableObject/GenerateObjectDiff`. Observation owns capture identity
and tagged reflected/native ranges. Its immutable payload and transfer context
are shared with the undo capture from the same Serialize pass; it has no restore
operation or annotation. Capture output and observations retain their exact hard
edges through an enclosing collector owner. There is no mixed runtime object-state
coordinator.

`FGetDiffableObjectOptions::ObjectSerializationMode` selects UE-shaped
`SerializeObject` or `SerializeProperties` observation. The default keeps the
ordinary virtual Serialize path and shared undo allocations. Properties-only
observation uses the same archive/property machinery but bypasses native
Serialize overrides and includes references to generated components. It is not
a historical transfer payload. The properties-only writer records reference paths directly. Ordinary undo
excludes construction-owned references.

`FDiffableObject::TryGetAllocatedSize` accounts its actual retained identity,
shared transfer allocations and tagged-range capacity. It shares the transfer
accounting implementation with `FSerializedObjectCapture`; callers must not count
a second stored copy of the same identity or derive retained string capacity from
a temporary transfer view. Annotation allocations remain the enclosing owner's
responsibility.

`PrepareSerializedObjectReplacement` and `DiffUtil::PrepareObjectReplacement`
prepare detached values using the existing `FObjectReplacementMap`. Replacement updates the indexed reference tables and shares the original
serialized bytes. Live target/class/Outer/package references
rebind explicitly. Expired or removed hard references reject preparation. Removed
weak references clear; expired weak keys remain expired and never resolve by Name.
Soft paths retain their canonical path representation. Preparation validates the
replacement target's schema without loading historical fields.
`FObjectReplacementMap::BuildFromObjects` accepts an explicit native old-to-new
map without discovering or publishing packages. It validates unique source and
destination identities, mapped Class and Outer, and template categories before
replacing the previous map. Callers own graph lifetimes and other reference owners;
package graph replacement continues to use its existing package-pair builder.

`CreateSerializedObjectReplacementParticipant` binds captures and/or observations
to a retained collector owner. Paired values must share their original transfer
state and share the prepared payload/context after publication. Unchanged payload
bytes reuse their allocation. The participant verifies complete mapped native
reference coverage, rechecks source values and replacement schema before commit,
then publishes with nonthrowing swaps under the existing graph protocol. Its
owner and binding storage must remain stable throughout the operation. This
runtime facility does not itself rebind editor history, annotations or custom
commands; those owners must prepare their complete retained state together.
`FSerializedObjectCapture::HasReplacementReferences` checks metadata, hard edges
and weak table entries. A weak-only stored dependency can therefore participate
without becoming a collector-owned hard reference; byte-pattern scanning is not used.

Transaction commands and annotations enumerate writable references through
`AddReferencedObjects(FReferenceCollector&)`. During reinstancing the collector
writes replacement objects into those slots. An exact persistent reference records
the new object key; logical references retain their subobject resolution rules.
Annotations with typed reconstruction data can specialize `FindAndReplaceInstances`
to remap encoded identities that GC traversal cannot see. Successful fixup
preserves the command and annotation instances. The editor owns validation and whole-entry retirement for [instance replacement and package reload](../../Editor/Architecture/Transactions/TransactionRecords.md#controlled-reinstancing).

Ordinary diffing requires the same exact target, class and compatible transfer schema. It
reports actual changed reflected member names and compares the remaining native
stream, including unnamed bytes. Reflected lengths may change without inventing
native changes; overlapping or out-of-bounds ranges fail. Full diff compares Name,
flags, direct Outer and package metadata; owned ancestor path text remains capture
context. Partial diff excludes metadata and compares shared tagged properties.
`ShouldSkipProperty` filters reflected results, never native data. Opaque annotation
changes and editor presence remain distinct from serialized field changes.

Resurrection reports the historical identity and compares the live tagged and
native data with its template in the owning native default graph. References to
corresponding graph nodes compare equal. If the owning graph cannot be matched, the object uses its published
class default. An object without a template compares against empty data.
Template observation is read-only and requires no transactional flag.
Engine annotations separately establish generated-component logical identity
with the shared instance-source matcher before comparing properties-only data.

Annotations may implement a pure `Validate` preflight for retained participants;
the default succeeds. Validation failures and exceptions reject replay before
historical fields load. Actor and component `PostEditUndo` restore their annotations
through void execution hooks. A restore fault propagates to the active replay;
ordinary construction retains its own failure result. Ordinary captures obtain
a fresh immutable snapshot.
`CreateSnapshot` refreshes live supplemental values from an existing annotation;
the default calls the object factory. Specialized annotations preserve recovery
participant scope across refreshes, even when construction removes the original
root or attachment. This refresh never mutates the original snapshot.

## Participant Ownership

Editor recording retains each command's explicit target by exact generation.
Membership, object summaries, replay hooks and the package checkpoint use this target.
Additional collector dependencies receive no completion hooks merely because the
command retains them. An unavailable target fails validation before writes.
Logical default-graph resolution does not grant an opaque native command access
to a recreated participant. Such commands require explicit checked replacement;
otherwise their frozen participant expires before native replay.

Commands and whole-object snapshots retain their recording order. Each explicit
command target is visible to `ContainsObject()`. A command recorded first suppresses later `Modify()` capture for that target. To capture a baseline as well, call `Modify()` before the first write.
Collector-only dependencies remain eligible for ordinary capture. Domains register an already-applied command
through `StoreUndo(Target, Change)` and own the restoration of canceled previews.

`CoreTransactionContractTests` links only Core/CoreDObject and is registered in
both editor and non-editor variants. It covers the runtime boundary independently
of editor history tests and their heavier module closure.
