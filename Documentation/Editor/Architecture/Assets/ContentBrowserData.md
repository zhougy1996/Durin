# Content Browser Data Providers

Summary: Define UE-shaped browser provider registration, opaque item ownership, operation dispatch, and incremental updates.

Modules: ContentBrowser, DurinEd, AssetTools, Engine, AssetRegistry

Last reviewed: 2026-11-13

## Ownership And Registration

`DContentBrowserDataSubsystem` is an editor subsystem. It retains available and
active `DContentBrowserDataSource` instances with strong object references.
The generic subsystem constructs no concrete sources. Asset and File each have
an editor-owned provider bootstrap subsystem that declares a dependency on the
data service, creates/registers its source and activates it. Reverse dependency
shutdown retires provider sources before the data service. Other modules may
register source instances whose outer is the editor or its data subsystem.
Available membership and active admission remain separate; registration alone
does not activate a third-party source. No source-class factory registry is used.
Source interface version 9 includes narrow operation contexts, virtual provider
initialization and explicit outcomes. All consumers must rebuild with this ABI.

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
membership reconciliation runs immediately through owner-thread registration/withdrawal notifications. Queries and Tick also reconcile mount changes.

Module providers retain ownership of their registrations and code until their
callbacks and opaque payloads are released. Unregistration closes dispatch; it
does not grant arbitrary DLL reload. ContentBrowser does not opt into dynamic
DLL reload. Editor/module retirement deinitializes services before releasing
registrations.

## Items, Payloads And Filters

`FContentBrowserItemData` carries a weak source identity, activation generation,
mount-registry revision, flags, kind, virtual path, item name, display name and shared immutable
`IContentBrowserItemDataPayload`. Only the source interprets that payload.
`GetOwnerDataSource` returns null off the owner thread, after source withdrawal,
deactivation/reactivation, editor shutdown or mount replacement. A new source
with the old name never inherits earlier handles.

`FContentBrowserItem` retains only its constituent ItemData descriptions and
opaque payloads. Logical folders merge contributors by virtual path, independently
of their physical storage. Its Edit, Save, Duplicate, Rename, Copy and Move methods dispatch
through owners; the generic layer has no asset/file execution switch.
Copy, move and rename prepare every participating source before writes, validate
opaque destination claims and execute partitions in stable source-name order.
Directory claims may merge; file/file and file/directory ownership collisions
reject the whole preparation. Unsupported contributions reject before mutation.
Earlier committed effects survive independent failures in later partitions.

`FContentBrowserItemData` supports the source/flags/path/name/display/payload
constructor. The kind-based constructor and duplicated kind field are removed.
Immutable worker captures can construct unbound descriptions; sources capture
admission when binding them on the owner thread.
Logical description getters remain readable after withdrawal; attribute access and
operations still require current admission. Item aggregate accessors forward to
its primary contribution. Native source payloads hold only their native storage
records and operation policy: Asset file/folder payloads are distinct from File
file/folder payloads, and no payload stores a complete generic item snapshot.

`GetItemAttribute`, `GetItemAttributes` and `GetItemPhysicalPath` delegate to the
owning source. Asset and File implementations interpret their own concrete
payloads directly. Unknown attributes, unavailable paths and stale admission clear
the output and return false. Dispatch retains a strong owner while invoking the
source and rechecks admission afterward, including callback-driven reactivation.
Aggregates read the first supported value and merge available attribute maps.
A physical path is optional and does not establish an item's logical identity.

Attribute values own UTF-8 strings, booleans, signed/unsigned 64-bit integers or
doubles, plus optional display-name/tooltip/suffix/importance metadata. Durin
retains string type/class names to match the qualified-string asset-definition
registry and its existing string-based UI; unsigned sizes preserve its native
package extent/file-size types. Clock values retain signed file-clock ticks.
The shared keys `ItemTypeName`, `ItemTypeDisplayName` and `ItemDiskSize` use UE's
names; package/bulk sizes, extension and clock fields additionally expose Durin
metadata. File sources do not advertise package-only attributes.

Type labels read provider attributes, with generic kind labels for unsupported
attributes. File-image thumbnail requests obtain extension, optional physical
resource path, size and time through the same boundary; they accept nonnegative
signed or unsigned sizes. Their native image-resource cache remains separate from
browser item identity. Presentation sorting, details, icons, category/type extras and context menus also
read provider attributes. The Python extension queries optional physical paths
for both applicability and invocation. Native discovery and operation inputs use a private native record and convert
that record into descriptions and typed payloads. Generic Item contains only
ItemData contributions, with no native metadata or duplicated description fields.

Typed identity is separate from operation admission. `GetItemDataKey` returns
the normalized virtual path and folder/file type; category and temporary flags
are masked out. `GetItemKey` also includes the source's process-local `FObjectKey`
for files, while folders omit it for logical aggregation. Keys preserve virtual
path case and use no physical/package path. They remain comparable after source
withdrawal without dereferencing the source. `GetContributionKey` additionally
captures source ownership, activation generation and mount revision for a specific
captured contribution. A stable logical key never makes an expired handle callable.
Directory aggregation uses typed keys. Private native records stay inside provider discovery and storage utilities.
`StableId()` and the generic native metadata fields have been removed.

Filters are value-owned. `CompileFilter`, `EnumerateItemsMatchingFilter`,
`EnumerateItemsUnderPath` and `EnumerateItemsAtPath` use virtual paths. The
subsystem compiles a separate source entry containing activation, mount and
source-discovery revisions and optional immutable source-specific filter data.
Executing an obsolete compiled entry cannot revive a withdrawn/reactivated source
or reinterpret its scope under a replacement mount.

The Asset source captures and caches its own immutable registry catalog by query
and mount revision, deriving asset rows and logical folders. The File source owns
filesystem traversal and compiles the physical scope from virtual paths. Both
contribute registered mount roots through root enumeration; other providers may
contribute roots and folders without registering physical storage. List and tree
requests consume these ordinary source contributions, including unsaved asset
folders. Views neither capture the asset catalog nor enumerate native directories.

`IContentBrowserDataCompiledFilter::Capture` is Durin's asynchronous adaptation:
providers prepare immutable values on the owner thread, and their capture code
runs through the existing task runtime without dereferencing reflected sources,
live registries, panels or host callbacks. Sources without worker preparation
continue to implement ordinary owner-thread enumeration; the query captures those
returned values before launching presentation work. The optional entry-status
callback is a deterministic native-I/O test seam, unused by ordinary requests.
There is no completed native snapshot supplied as an extra filter.

Owner-thread publication binds raw contributions to their original sources and
rechecks membership, activation, mounts and path-relevant discovery changes.
Stale list/tree results are retried from freshly compiled input. Per-view revision
cursors retain bounded source/service revision coverage; missing coverage causes
conservative reconciliation. Independent queries and models never share
cancellation or publication rights. Model destruction drains its worker bodies;
providers/modules remain responsible for retaining their code while captured
filter objects or payloads exist.

## Operations And Request State

`DContentBrowserAssetDataSource` owns asset/redirector capabilities;
`DContentBrowserFileDataSource` owns ordinary-file capabilities. Both contribute
folders. Both concrete sources derive directly from `DContentBrowserDataSource`;
the generic abstract source retains unsupported defaults.

Asset independently owns editing, save, duplicate and redirector entrypoints,
using package-only save, transfer and asset-action utilities. File owns its
ordinary-file transfer and editing implementation. Shared mutation policy contains
only admission, feedback and notification plumbing; folder creation contains only
path validation and directory creation. There is no combined native executor.
Payloads retain immutable request-local policy and feedback, not executors or
session maps. Providers prepare single-use transfer contexts and explicit deletion
requests. Native helpers perform package/file ownership checks; generic dispatch
uses source contracts and opaque claims without concrete-provider casts.
The original activation and mount admission identities remain captured in policy,
including on returned items. Captured sources and request state survive callbacks
that dismiss a modal or withdraw a source; retired requests cannot be restored
over a replacement confirmation.
Provider discovery preparation is independent of mutations. Provider query and binding APIs take only the optional `FContentBrowserOperationContext`: editing, mutation policy, feedback and external mutation notification. They do not require browser navigation, reimport, reconciliation or a data-service host.
`FContentBrowserDataQuery` retains only provider-neutral request, binding and
revision state with a weak service binding.

Provider `RenameItem` and `DuplicateItem` output source-owned `ItemData`.
Source feature version 9 identifies explicit bulk save/copy/move/delete and rename
results plus prepared transfer entrypoints. Generic Item aggregates successful
rename outputs and retains failed original identities in the effect ledger.
`FContentBrowserItemDataTransferContext` owns captured validation, destination
claims, cancellation and a single execution callback. Validation grants no later
permission; execution repeats admission, then consumes the context even on failure.
No-op rename is explicit and publishes no mutation. Duplicate keeps its separate
creation/output and save contract.

Copy/move/rename/delete return `FContentBrowserOperationResult`, without implicit
boolean conversion. Effects carry old/new contribution identities, committed,
reverted, failed or unattempted status, diagnostics and structured recovery paths.
Cancellation has its own completion status and can coexist with committed effects.
Independent failures continue; cancellation, admission loss and uncertain native
recovery stop remaining work. No operation compensates earlier committed source
partitions. Native tools retain package/companion staging authority; browser code
never infers reverts or recovery paths from diagnostic text.

Ordinary file copy uses no replacement; move verifies copied bytes and source
identity before removing the source. Failed removal reports both retained paths.
Asset transfer works in package units through AssetTools; destination names are
exact, while the separate Duplicate command still chooses a free suffix. Direct
managed-companion transfer rejects. Folder copy rejects unsupported custom
companions before writes. Shared directories merge and existing files fail without
replacement. Partial results refresh views, preserve survivors and migrate selection
for committed identities. Every recovery location is logged for inspection.

The UE save flags support all four combinations: dirty predicates use the
loaded-dirty package policy; no dirty predicate permits canonical saves.
`SaveOnlyIfLoaded` first requires residency and retains loaded exports across
execution so the operation cannot turn ordinary unloading into an implicit load.
Unknown flag bits reject. No additional AssetTools save-mode enum was introduced.

`BulkSaveItems` and aggregate `Item.Save` return `FContentBrowserOperationResult`,
without implicit boolean conversion. Each effect records the original contribution
identity, optional committed identity, status and diagnostic. Native save utilities
use package identities; the source maps each package outcome to its contributing
items. Multiple exports in one package cause one physical save. Dispatch retains
sources, orders partitions by source name and items by virtual path, and rechecks
activation/membership before execution. Admission loss marks the remaining work
unattempted; independent capability failures do not prevent other packages or
source partitions from saving. Already committed packages are retained.

Save status distinguishes complete, partial and failed. Registry publication failure
records both a committed effect and a failed reconciliation effect, so it cannot
hide the committed file. The current AssetTools boolean save API does not expose
typed recovery details after an unsuccessful write without a commit notification;
that case stops the remaining batch for inspection, reports the native diagnostic
and requests reconciliation. It does not claim rollback, populate invented recovery
paths or continue through unknown effects. Only actual commit notifications publish
modified-item updates; a batch publishes its accumulated changes once. Panels show
partial-save diagnostics, refresh after committed effects and preserve selection.

`CanCreateFolder` and `CreateFolder` take the proposed virtual path. Creation
returns `FContentBrowserItemDataTemporaryContext` without writing. The context
stores a single temporary `ItemData`, exposed by `GetItemData`; its source-owned
`ValidateItem` and `FinalizeItem` callbacks retain the original directory and
admission identity. Finalization returns source-owned `ItemData`; only the Item/UI
layer aggregates it for presentation. Inline names cannot change the directory.
Cancellation simply releases the context. Finalization rechecks policy and only
then creates content. Validation failures allow a corrected name to be retried;
successful finalization consumes that context's callbacks. Activation or mount
changes invalidate the captured native context before mutation.

Deletion remains an intentional UE signature adaptation:
`PrepareDeleteItems` returns a move-only `FContentBrowserDeleteContext`;
`BulkDeleteItems` requires that context and the exact owner-bound item scope.
Execution validates every source partition before destruction, then rechecks source identity, admission and the existing confirmation
fingerprints. Providers delete owned descendants and File removes empty directories; no recursive directory removal callback exists. Stale confirmation is replaced explicitly, destruction consumes the
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
unrelated directory do not restart a capture. Each update retains its queue revision, so a capture ignores older events delivered in the same frame batch. Coverage markers reuse those two batches and add no retained service mutation journal. Registry reconciliation suspension
still prevents publication, including when a provider revision changes.

Native recovery with unknown projection coverage produces scoped modified-folder
events for mounted roots; a modified folder invalidates that logical subtree.
Mixed native batches publish each delta through its actual owning adapter; an
unsupported asset delta is never converted into a fictitious folder move.
The public source API does not add UE's deprecated blanket refresh request.
Operation contexts retain external mutation notifications for independent consumers. Asset registry subscriptions provide asset deltas; immediate operation notification does not replay those same deltas.

Asset sources subscribe to `OnAssetQueryChanged` and pump query synchronization
on their owner Tick. The committed snapshot/journal precedes notification;
reentrant events queue behind the current broadcast. Initial persistence of a
live query row is a modification, not a second addition. Source shutdown removes
subscriptions; inactive sources reject updates until fresh activation.

File sources own one cancellable background metadata snapshot per editor. At
most one scan runs per source; steady-state starts are spaced by at least half
a second of editor time. Workers capture owned mount paths and cancellation only, without
source, editor, registry or UI access. Owner publication verifies project,
mount, activation and local-mutation epochs. Withdrawal/shutdown cancels and joins
work before module retirement. Missing task scheduling leaves discovery dormant
in minimal test hosts. Incomplete scans invalidate root coverage and never invent
removals. Package/bulk changes reconcile the asset registry on the owner thread;
companion ownership stays with Asset. File revisions retain bounded path coverage
for asynchronous capture fences. This is metadata polling because Durin has no
portable directory-watcher API: same-size rewrites preserving timestamps require
explicit reconciliation. It is not equivalent to an OS change-event stream.

## Panel Boundary

The host supplies a weak `DataSubsystem` binding. Each panel owns its query state,
selection, navigation, capture cancellation, temporary creation and confirmation;
no panel owns a mutation request context. Queries publish through source
enumeration, and menus, shortcuts, clipboard and drag/drop use item capabilities
and data-service batch routing with virtual destinations.

`DEditorEngine::Tick` dispatches `FTickableEditorObject` callbacks once for its owning editor, before tool/World/presentation work. The data service ticks with zero browser views and zero Worlds; browser and query code never tick it. Explicit test hosts may call Tick directly. Every view tracks its own service revision, so
hidden or later-pumped browsers reconcile changes even after a frame batch expires. If rows refresh after a mutation but before its next owner publication, selection trimming waits for the pending move/delete events; it resumes on the next view update.
Internal drags retain the original owner-bound items. Transfer commands resolve
current items with the destination view's policy and require the original source
identities to remain valid, preventing withdrawal/reactivation from rebinding old
clipboard or drag state. Single-asset drags retain the existing external payload
format used by other editors.

Creation, import, details and asset-definition registration remain independent of
data sources. Panel and extension lifetime details are described by the
[Content Browser contract](ContentBrowser.md).

Browser selection, anchors, rename state, pending reveals, extension selection
contexts and browser thumbnail references use `FContentBrowserItemKey`.
`StableId()` is removed. Folder keys omit the owner; file keys include it.
Source-qualified move/removal updates repair matching selections without changing
another source's same-path file. A merged folder retains its original logical
selection while other contributions survive. Clipboard and drag captures resolve
by logical key and compare full contribution keys, including activation and mount
admission. Image decoder cache keys remain native resource paths and asset
thumbnail fingerprints remain Registry identities; neither is a browser row key.

ItemData owns only flags, virtual path, name/display name, source admission and
opaque payload. Generic Item aggregates those descriptions and forwards provider
attributes. `AssetPath` is an optional native thumbnail/interop resource attribute;
it is separate from a row's virtual path and logical key. Asset thumbnails compare
Registry resource identities and recapture dependencies when that resource changes.
File images query optional native resources, and external asset drag payloads are
created only when the source supplies an asset resource identity.

All merged folder contributions are retained. `AreItemsEquivalentForOperations`
is a conservative source-owned admission predicate: the default only recognizes
the same current contribution. Native sources additionally recognize their shared
physical folder backend. Generic commands validate every such alias and execute
the single backend once; native bulk requests normalize aliases after retaining
all request policies. Native folder deltas are published for both participating
sources so file and asset selections can follow the committed subtree move.
Unrelated merged folders still require Stage 3's per-contribution result dispatch.
