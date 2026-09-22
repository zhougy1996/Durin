# Transaction Record Foundation

Summary: Define exact editor transaction object identity, collector-enumerated record references, and root-free focused property payloads.

Modules: CoreDObject, DurinEd

Last reviewed: 2026-09-22

## Scope

`Durin::Editor::FPersistentObjectRef` and
`FFocusedTransactionObjectSnapshot` provide the data and lifetime foundation
for collector-integrated editor history. The focused snapshot is a detached
capture/validation primitive, not an executable history record. `DTransBuffer`
stores complete before/after property records together with executable custom
changes in the single application Undo/Redo order. Object creation and deletion
remain explicit custom changes rather than persistent-reference resurrection.

All capture, resolution, collector traversal, and detached restore operations
run on the game thread under CoreDObject's synchronous collection contract.

## Custom Replay Results

`FTransactionRecord::Apply` calls `ITransactionCustomChange::Replay` and formats
its `FTransactionCustomError` into the record result message. Material parameter replay reports
an unavailable target or a material write error with owned parameter identity and
nested `FMaterialError`; the record boundary renders that cause once.
Material presentation replay distinguishes expired targets from rejected writes,
owning the captured target path and changed/candidate node count. Captured path
storage participates in transaction memory accounting.
Material graph object replay distinguishes expired owners/members from externally
changed membership. Member restoration retains locator or property-snapshot causes
and the failed member index; successful earlier restores are compensated in the
existing reverse order before returning the original typed failure.
Transform gizmo replay retains the first unavailable/rejecting target's label,
index and total target count while preserving its existing all-target application
policy. A later repaired replay does not mutate the earlier failure evidence.
Actor attachment replay distinguishes empty selection, unavailable participants,
parent mismatch, invalid parents, cycles and write rejection. Validation errors
retain participant index/count and expected/actual parent paths where available;
validation finishes before writes and existing compensation ordering is preserved.
Sky box creation and texture replay distinguish unavailable resources/targets,
actor membership, name collision and spawn/destroy rejection. Creation failures
retain the level path and requested actor name; collision rejection leaves the
existing actor and redo history available for a repaired retry.
Sky box placement exposes a compact error classification, owned `Message`,
result actor and changed flag. It formats transaction and direct-replay failures
inside the command boundary.

The static mesh batch change carries typed state-validation and mutation causes.
Errors own level/name/index context, injected mutation phase and a nested
incomplete-rollback cause. Internal state validation and application return typed
results directly, with no error-output parameter. Replay preserves distinct actor
constraints for unsupported class, component graph, attached parent/children and
play transitions.

The outer static mesh plan/execution boundary exposes `Error`, `Message` and an
optional-by-sentinel mutation index alongside business data. It formats the
complete transaction or direct-replay failure before returning. Viewport and
outliner callers consume the message directly; they do not traverse history
causes. The public actor-support query returns a boolean.
`Replay` is the required typed entry point for every custom change. The interface
has no boolean Undo/Redo methods or legacy rejection adapter. Typed replay errors
remain local to that API; record results expose messages. Operation details are
presentation metadata, not failure storage.

## Exact Participant Identity

`FPersistentObjectRef` stores only an `FObjectKey`: one object-array slot and
its generation. A null reference has the invalid slot. A non-null reference
resolves only while that exact generation remains registered and is not
garbage or begin-destroyed.

The reference never falls back to object path, name, Outer path, class lookup,
or a newly allocated object in a reused slot. Rename and reparent therefore
preserve identity, while physical removal invalidates it. An object already
marked as garbage resolves as absent even before physical removal.

Merely storing a persistent reference has no retention effect.
`AddReferencedObjects(FReferenceCollector&)` reports the live object only when
a reachable managed owner explicitly enumerates the reference. Collector
traversal cannot rescue an object already marked as garbage.

## Retention-Neutral Property Payload

`FPropertyValueSnapshotPayload` is the shared property snapshot wire payload.
It owns encoded bytes plus a deduplicated table of exact handles for hard object
references. Weak references remain generation handles encoded in the bytes;
soft references remain asset paths. The payload never calls `AddToRoot`.

`FPropertyValueSnapshot` remains a general detached-value adapter for callers
outside transaction history. Transaction records use the root-free payload and
collector traversal exclusively.

Payload restore validates the requested reflected property shape and resolves
every hard handle. A stale, physically removed, or garbage hard reference makes
restore fail rather than substituting another object. Weak references decode
their original handle and naturally remain invalid when the target is gone.

## Focused Object Snapshots

A focused snapshot owns:

- the target's `FPersistentObjectRef`;
- a top-level member locator containing declaring type, member name, and fixed
  array index;
- one retention-neutral property payload; and
- a deduplicated set of hard value references.

Capture accepts only a live target and a top-level property belonging to its
class hierarchy. The snapshot stores no live value-container address. At restore
time it resolves the exact target, finds the current member, verifies its
declaring type and snapshot compatibility, allocates
`FReflectedValueStorage`, and decodes into that detached storage.

Member capture and focused capture/restore return
`std::expected<void, FTransactionSnapshotError>`; member resolution returns
`std::expected<FProperty*, FTransactionSnapshotError>`. They retain owned member and declaring-type names, array
bounds, expected/actual property kinds and exact target keys; a stale detached
restore preserves the snapshot's original key. Storage and payload failures
retain their CoreDObject causes. Failed capture leaves its output unchanged,
and failed detached restore preserves the caller's existing storage. Errors
remain valid across successful retries. `FormatTransactionSnapshotError` is
used only by presentation or pending outer result adapters.

Detached restore does not mutate a live `DObject`, emit editor notifications,
or bypass `PreEditChangeProperty` and `PostEditChangeProperty`. Executable
`FTransactionObjectRecord` values own both before and after payloads and feed
them through the validated editor mutation pipeline. Their capture, validation
and application APIs return `std::expected<void, FTransactionObjectRecordError>`.
Object-record rejections preserve exact owner identity, member/snapshot/leaf facts,
payload validity and kinds, selected history side, and typed member, path,
draft or mutation causes. Capturing an invalid replacement leaves the previous
record intact; successful history retries do not alter prior error values.
The enclosing `FTransactionRecord` formats these errors at its boundary and
returns `FTransactionRecordResult`, an alias for
`std::expected<void, std::string>`. History consumers handle recovery disposition
and messages rather than inspecting property-specific causes.

`FFocusedTransactionObjectSnapshot::AddReferencedObjects(...)` reports the target
and every distinct hard payload reference exactly once. It never reports weak
or soft values. A reachable `DObject` owner may enumerate a native vector of
snapshots from its own `AddReferencedObjects(...)` override. Removing or
destroying a snapshot removes all of those strong edges in the same logical
operation; there are no per-record manual roots to unwind.

## Hierarchy Boundary

Transaction references follow the ordinary CoreDObject reachability rules. A
directly referenced child keeps its complete Outer chain alive. Referencing an
Outer does not retain children or siblings. Object creation and deletion need
explicit future records; a persistent reference never resurrects a garbage
object.

## Validation

The lasting contract is covered by `CoreObjectTests`,
`CorePropertyValueSnapshotTests`, and the persistent-reference and focused
snapshot cases in `EditorOperationTests`. `EditorPropertyTests` protects the
collector-backed property-history path.

## Related Documentation

- [Editor Transactor Core](Transactors.md)
- [Garbage Collection](../../Runtime/Core/GarbageCollection.md)
- [Reflected Property Editing](ReflectedPropertyEditing.md)
- [Editor Transaction System roadmap](../../Roadmaps/Archive/2026-08/EditorTransactionSystem.md)
