# Runtime Transaction Contracts

Summary: Define runtime-neutral transaction capture, custom changes, exact object references, and object notification boundaries.

Modules: Core, CoreDObject

Last reviewed: 2026-10-20

## Core Boundary

`Misc/ITransaction.h` defines the runtime capture interface published as `GUndo`.
Core forward-declares object and reflected-property types; it does not include
CoreDObject or an editor module to define a transaction or custom change.
The editor owns the active recorder and application history, as described in
[Transactors](../../Editor/Architecture/Transactors.md).

`Misc/Transaction.h` owns `FCommandChange`, its synchronous Apply/Revert result,
`ETransactionOperation`, transaction context and operation identity allocation.
A custom change declares modified objects separately through
`GetModifiedObjects()`. Its collector traversal describes retained dependencies;
that traversal alone never means a dependency was edited. A command may declare
no modified objects when it edits non-object native domain state.

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

The capture interface exposes finalization, object-bound custom changes,
snapshots, arrays and PIE inspection as explicit result-bearing operations.
Failure and unsupported capability are observable errors. Array requests identify
an object and reflected member/index/range, never a borrowed live vector address.
Interface availability alone does not assert a particular recorder supports
every operation; callers must check its result.

## CoreDObject Boundary

`DObject/TransactionObjectReference.h` owns `FPersistentObjectRef` and its
deduplicating reference set. Ordinary references preserve exact slot/generation
identity. Resolve rejects garbage, destroyed or replaced generations; it never
searches for an unrelated object with the same Name. Stored references retain
nothing by themselves; a managed owner must enumerate their live hard edges.
Editor object records and domain commands use this same runtime identity.

`DObject/TransactionSerialization.h` owns the shared transaction-purpose virtual
`Serialize` transfer. `FSerializedObject` holds bytes and exact hard edges;
`FSerializedObjectInfo` composes capture-time `FTransactionObjectId`, exact
target/class/Outer/package references and supported flags. The ID owns original
Name and path text without mixing identity with change flags.
`FTransactionSerializationContext` owns schema, versions, frozen field
exclusions and archive-recorded hard/weak reference positions. Capture uses `FObjectArchive`, includes reflected and native
serialized fields, and preserves weak keys and soft paths without
retaining their targets. These helpers never acquire annotations.

`ValidateSerializedObject` checks identity, field schema and native field versions
without loading historical fields. `LoadSerializedObject` is a transfer operation:
its caller must preflight first and own failure compensation. An error can follow
partial native writes; a thrown native serializer explicitly requires recovery.
Garbage and construction flags are irreversible. The canonical DurinEd record
composes transfer, annotations and observations and coordinates inverse
compensation; CoreDObject has no mixed object-state coordinator.
The [serialization contract](Serialization.md) owns archive filtering and
detached parent-baseline requirements.

`DObject/TransactionObjectAnnotation.h` declares supplemental reconstruction data,
collector traversal, serialization, owned allocation accounting and optional
additional object changes. `DObject/TransactionObjectEvent.h` declares owned
identity/change summaries, Snapshot/Finalized/UndoRedo kinds and shared annotation
ownership. These contracts contain no editor history or presentation service.

DObject supplies safe defaults: no annotation, an empty PostTransacted observer,
and annotation-aware PostEditUndo that forwards to the existing virtual completion
hook. The editor record captures annotations before virtual Serialize at initial,
final and inverse capture; schema-only validation skips that hook. Records own
immutable supplemental snapshots, enumerate their hard references and check
allocation addition for overflow. Editor no-op pruning retains annotated records
even when serialized bytes match.
History keeps annotations in-process; their
explicit Serialize hook is separate from the object's payload and ordinary
package persistence. Hook declarations do not dispatch transaction events by
themselves. Engine types own native reconstruction and reconciliation.

`DObject/TransactionDiff.h` owns `FDiffableObject`, `FSerializedTaggedData` and
`DiffUtil::GetDiffableObject/GenerateObjectDiff`. Observation owns capture identity
and tagged reflected/native ranges. Its immutable payload and transfer context
are shared with the undo capture from the same Serialize pass; it has no restore
operation or annotation. Capture output and observations retain their exact hard
edges through an enclosing collector owner. There is no mixed runtime object-state
coordinator.

`PrepareSerializedObjectReplacement` and `DiffUtil::PrepareObjectReplacement`
prepare detached values using the existing `FObjectReplacementMap`. Only explicit
archive reference positions are rewritten; opaque native bytes and capture-time
identity text/keys remain unchanged. Live target/class/Outer/package references
rebind explicitly. Expired or removed hard references reject preparation. Removed
weak references clear; expired weak keys remain expired and never resolve by Name.
Soft paths retain their canonical path representation. Preparation validates the
replacement target's schema without loading historical fields.

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
and recorded weak slots. A weak-only stored dependency can therefore participate
without becoming a collector-owned hard reference; byte-pattern scanning is not used.

`ITransactionObjectAnnotation::CreateReplacement` prepares an independent
immutable annotation. `PrepareTransactionAnnotationReplacement` reuses unaffected
annotations, invokes the hook for mapped dependencies, rejects aliases or
remaining old-generation edges, and validates the result. Unsupported annotation
types fail explicitly. Core's `FCommandChange::CreateReplacement` supplies the
same opt-in boundary for native command data without an editor dependency.
Its default reports unsupported reinstancing. These copy hooks are native
adaptations for Durin's checked publication protocol; they are not UE API claims.
The editor owner decides coherent whole-entry retirement when a required hook is
unsupported; unaffected command data can keep its original unique ownership.

Diffing requires the same exact target, class and compatible transfer schema. It
reports actual changed reflected member names and compares the remaining native
stream, including unnamed bytes. Reflected lengths may change without inventing
native changes; overlapping or out-of-bounds ranges fail. Full diff compares Name,
flags, direct Outer and package metadata; owned ancestor path text remains capture
context. Partial diff excludes metadata and compares shared tagged properties.
`ShouldSkipProperty` filters reflected results, never native data. Opaque annotation
changes and editor presence remain distinct from serialized field changes.

This observation API admits live ordinary objects. It does not resurrect expired
identities or infer arbitrary archetypes. Class-default override comparison uses
the existing default graph and default-delta facilities, with their graph-aware
reference equivalence, rather than comparing different objects' exact-reference
bytes. These existing default-comparison facilities do not imply arbitrary
archetype or Blueprint/SCS/UCS matching in transaction observations.

Annotations may implement a pure `Validate` preflight for retained participants;
the default succeeds. Validation failures and exceptions reject replay before
historical fields load. Ordinary captures obtain a fresh immutable snapshot.
`CreateSnapshot` refreshes live supplemental values from an existing annotation;
the default calls the object factory. Specialized annotations preserve recovery
participant scope across refreshes, even when construction removes the original
root or attachment. This refresh never mutates the original snapshot.

## Participant Ownership

Editor recording freezes each command's explicit modified participants as exact
references, deduplicates them and retains them through GC. ContainsObject,
object summaries, replay hooks and affected-package enumeration use those
participants. Additional collector dependencies remain lifetime dependencies;
they receive no completion hooks merely because the command retains them.
Expired participants fail validation before replay mutates any other participant.

A bound single-object custom record claims that object's general capture, so a
later Modify does not create a redundant serialized record. A prior serialized
record remains ordered before a later custom record. Composite commands declare
their actual modified objects and keep executable domain ownership explicit;
their participant membership alone does not suppress generic object capture.
Domains must avoid overlapping serialized/custom ownership of the same field.
Recording an already-applied command and executing the initial command remain
separate operations; cancellation does not imply rollback.

`CoreTransactionContractTests` links only Core/CoreDObject and is registered in
both editor and non-editor variants. It covers the runtime boundary independently
of editor history tests and their heavier module closure.
