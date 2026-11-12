# Content Browser Data Providers

Summary: Define UE-shaped browser provider registration, opaque item ownership, operation dispatch, and incremental updates.

Modules: ContentBrowser, DurinEd, AssetTools, Engine, AssetRegistry

Last reviewed: 2026-11-12

## Ownership And Registration

`DContentBrowserDataSubsystem` is an editor subsystem. It retains available and
active `DContentBrowserDataSource` instances with strong object references.
Its built-in Asset and File sources belong to that editor. Other modules may
initialize and register source instances whose outer is the requesting editor
or its data subsystem. No source-class factory registry is involved.
The source feature interface version is 2: Stage 3 adds virtual operation entries,
so providers compiled against the earlier vtable must rebuild before registration.

A live registration retains its instance with a native strong reference, so
ordinary GC cannot collect it between registration and subsystem discovery.
Withdrawal removes the registration before releasing that reference. `Shutdown`
is virtual; `BeginDestroy` cleans up an unregistered instance. Registered sources
must retire at an owner safe point before collection, and subclass shutdown must
be idempotent and call its base implementation.

`Initialize(bool)`, `RegisterDataSource`, `UnregisterDataSource`, `Shutdown`
and `SetDataSink` follow the UE source-instance lifecycle. Activation attaches
the subsystem sink; deactivation detaches it before further dispatch. Duplicate
names cannot replace an already active source. Withdrawal requires an owner
thread modular-feature safe point; it invalidates items immediately, while
membership reconciliation occurs on query or `Tick`.

Module providers retain ownership of their registrations and code until their
callbacks and opaque payloads are released. Unregistration closes dispatch; it
does not grant arbitrary DLL reload. ContentBrowser does not opt into dynamic
DLL reload. Editor/module retirement deinitializes services before releasing
registrations.

## Items, Payloads And Filters

`FContentBrowserItemData` carries a weak source identity, activation generation,
mount-registry revision, kind, virtual path and shared immutable
`IContentBrowserItemDataPayload`. Only the source interprets that payload.
`GetOwnerDataSource` returns null off the owner thread, after source withdrawal,
deactivation/reactivation, editor shutdown or mount replacement. A new source
with the old name never inherits earlier handles.

`FContentBrowserItem` retains the presentation snapshot and its constituent
item data. Logical folders merge contributors by virtual path, independently
of their physical storage. Its Edit, Save, Duplicate, Rename, Copy and Move methods dispatch
through owners; the generic layer has no asset/file execution switch.
Unrelated multi-source moves and copies reject before mutation. The two native
adapters explicitly support mixed native batches through the same atomic backend;
their duplicate physical-folder contributions retain one executable identity.
Other providers' logical folder contributions remain distinct. Folder operations
with multiple unrelated owners cannot silently choose one provider.

Filters are value-owned. `CompileFilter`, `EnumerateItemsMatchingFilter`,
`EnumerateItemsUnderPath` and `EnumerateItemsAtPath` use virtual paths at the
public boundary. Native providers resolve physical paths internally. The native
collector retains immutable catalog/filesystem captures, not live source or
registry pointers. Item binding and publication run on the owner thread and
check source admission again; retirement drops undelivered captured items.
The optional immutable extra filter carries a completed native snapshot through
ordinary source enumeration without repeating filesystem collection. Other
providers still enumerate their own data on the owner thread. Virtual identities
also support items without physical storage; view filtering uses virtual paths.

## Operations And Request State

`DContentBrowserAssetDataSource` owns asset/redirector capabilities;
`DContentBrowserFileDataSource` owns ordinary-file capabilities. Both contribute
folders. `DContentBrowserNativeDataSource` shares the native adapter mechanics;
the generic abstract source retains UE-style unsupported defaults.

Both native sources reuse `FContentBrowserRequestContext`, the single existing
mutation/recovery substrate. This replaces the misleading private
`FContentBrowserDataSource` name, rather than adding another operation engine.
Payloads retain request-local policy and feedback. Same-source batches from
different captures combine their admission predicates and execute the common
operation implementation once; one query scope is not a new execution identity.
Pure discovery preparation belongs to `FContentBrowserDataQuery`, which retains
per-view capture state and a weak service binding, independently of mutations.

Mutation predicates check editor capability, source generation and mount
revision before and after caller policy callbacks. Capabilities reserve no
permission for later I/O. Execute paths revalidate through the existing AssetTools
and filesystem operations, preserving companion policy, staging, rollback,
committed-effect notifications and recovery diagnostics.

The UE save flags support all four combinations: dirty predicates use the
loaded-dirty package policy; no dirty predicate permits canonical saves.
`SaveOnlyIfLoaded` first requires residency and retains loaded exports across
execution so the operation cannot turn ordinary unloading into an implicit load.
Unknown flag bits reject. No additional AssetTools save-mode enum was introduced.

`CanCreateFolder` and `CreateFolder` take the proposed virtual path. Creation
returns `FContentBrowserItemDataTemporaryContext` without writing. Its source-owned
`ValidateItem` and `FinalizeItem` callbacks retain the original directory and
admission identity; inline names cannot change that directory. Cancellation simply
releases the context. Finalization rechecks policy and only then creates content.

Deletion remains an intentional UE signature adaptation:
`PrepareDeleteItems` returns a move-only `FContentBrowserDeleteContext`;
`BulkDeleteItems` requires that context and the exact owner-bound item scope.
Execution rechecks source identity, admission and the existing confirmation
fingerprints. Stale confirmation is replaced explicitly, success consumes the
capability, and cancellation/destruction releases it. There is no unconfirmed
delete entrypoint.
Once native destruction begins, its execution session retires after the attempt,
including partial failure; a retained modal cannot retry that physical deletion.
`IContentBrowserDeleteRequest::GetPreview` exposes immutable presentation data;
panels do not inspect native fingerprints or execution sessions. A changed preview
requires another confirmation. Providers without a detailed preview use the
panel's generic selection label and still supply their validated capability.

## Incremental Updates

Sources call `QueueItemDataUpdate(FContentBrowserItemDataUpdate&&)`.
Added, Modified, Moved and Removed factories carry owned item data; moves retain
the previous virtual path. Native committed-effect batches generate these
updates, including generated copy destinations. Failed/no-effect operations
publish no optimistic update.

The subsystem queues accepted updates and publishes a shared read-only frame
batch during its owner-controlled `Tick`. Readers do not consume each other's
events; `GetItemDataUpdates` remains valid until the next Tick. The UE-shaped
`OnItemDataUpdated` multicast migrates each panel's selection and rename state;
panel shutdown removes its subscription. Membership/mount
changes also advance `GetRevision`. A reader that misses frame coverage must
reconcile its own snapshot rather than assume retained journal history.
Views filter covered pending/published updates by virtual scope; changes in an
unrelated directory do not restart a capture. Coverage markers reuse those two
batches and add no retained mutation journal. Registry reconciliation suspension
still prevents publication, including when a provider revision changes.

Native recovery with unknown projection coverage produces scoped modified-folder
events for mounted roots; a modified folder invalidates that logical subtree.
Mixed native batches publish each delta through its actual owning adapter; an
unsupported asset delta is never converted into a fictitious folder move.
The public source API does not add UE's deprecated blanket refresh request.
Legacy host mutation notifications remain only at the request-context adapter
boundary for asset-registry reconciliation and independent external consumers.

## Panel Boundary

The host supplies a weak `DataSubsystem` binding. Each panel owns its query state,
selection, navigation, capture cancellation, temporary creation and confirmation;
no panel owns a mutation request context. Queries publish through source
enumeration, and menus, shortcuts, clipboard and drag/drop use item capabilities
and data-service batch routing with virtual destinations.

Browser updates drive the service at most once per editor frame; explicit headless
callers may drive `Tick` directly. Every view tracks its own service revision, so
hidden or later-pumped browsers reconcile changes even after a frame batch expires.
Internal drags retain the original owner-bound items. Transfer commands resolve
current items with the destination view's policy and require the original source
identities to remain valid, preventing withdrawal/reactivation from rebinding old
clipboard or drag state. Single-asset drags retain the existing external payload
format used by other editors.

Creation, import, details and asset-definition registration remain independent of
data sources. Panel and extension lifetime details are described by the
[Content Browser contract](ContentBrowser.md).
