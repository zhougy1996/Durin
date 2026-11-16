# Content Browser Data Providers

Summary: Define UE-shaped browser provider registration, opaque item ownership, operation dispatch, and incremental updates.

Modules: ContentBrowserData, ContentBrowserAssetDataSource, ContentBrowserFileDataSource, ContentBrowser, DurinEd, AssetTools, Engine, AssetRegistry

Last reviewed: 2026-11-17

## Ownership And Registration

`ContentBrowserData` owns the generic source, item, query, snapshot, operation and
service contracts. Its direct dependencies are Core, CoreDObject and DurinEd;
it has no native-provider, AssetRegistry, AssetTools, Mona or rendering dependency.
`ContentBrowserAssetDataSource` and `ContentBrowserFileDataSource` independently
own their reflected sources, bootstrap subsystems, discovery, opaque payloads and
native operation policy. Each depends on Data and native services, never on its
sibling or the UI. `ContentBrowser` owns presentation and depends on Data without
concrete provider dependencies. MainFrame wires optional asset maintenance
callbacks through the host context. Providers and the data service can operate
without loading the UI module.

Storage-neutral filesystem, mount projection and path inspection helpers live in
Core. The shared `FContentChange`/`FContentChangeBatch` value contract lives in
Core's `Misc/ContentChanges.h`; AssetRegistry retains its scoped projection and
bounded journal. This lets Data implement generic operation fencing without a
registry or native-provider dependency. AssetTools owns ObjectTools and the native
asset delete model; File owns private ordinary-file/mixed-folder selection,
fingerprints and partial I/O execution. Providers publish actual changes and
report native diagnostics independently of bool results. Test-only value construction
and snapshot fixtures use public provider contracts or owner-module friend access;
production queries require a live data service and have no standalone-source or
injected-snapshot dispatch branch. Provider fixtures register and activate sources
through the normal subsystem collection and data-service APIs.

`DContentBrowserDataSubsystem` is an editor subsystem. It retains available and
active `DContentBrowserDataSource` instances with strong object references.
The generic subsystem constructs no concrete sources. Asset and File each have
an editor-owned provider bootstrap subsystem that declares a dependency on the
data service, creates/registers its source and activates it. Reverse dependency
shutdown retires provider sources before the data service. Other modules may
register source instances whose outer is the editor or its data subsystem.
Native providers prepare update state and subscriptions before their final
optional registration. Registration broadcasts synchronously and may retire the
source; initialization performs no further setup after that callback boundary.
The initializing source is retained during callbacks, and a later initialization
cannot overwrite a subscription left by an interrupted one.
Available membership and active admission remain separate; registration alone
does not activate a third-party source. No source-class factory registry is used.
Source interfaces include narrow operation contexts, virtual provider
initialization, public operation dispatch and explicit outcomes. All consumers must rebuild with this ABI.

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
membership reconciliation runs immediately through owner-thread registration/withdrawal
notifications. Committed Core mount events invalidate native caches/discovery;
the subsystem coalesces a full-refresh event. No browser layer
polls the mount-registry revision.

Module providers retain ownership of their registrations and code until their
callbacks and opaque payloads are released. Unregistration closes dispatch; it
does not grant arbitrary DLL reload. The data, native provider and UI modules do not opt into dynamic
DLL reload. Editor/module retirement deinitializes services before releasing
registrations.

## Items, Payloads And Filters

`FContentBrowserItemData` carries weak provider ownership, flags, kind, virtual
path, item name, display name and shared immutable
`IContentBrowserItemDataPayload`. Only the source interprets that payload.
`GetOwnerDataSource` resolves that weak object on the owner thread. Source
activation and mount versions are not item identities; reactivation of the same
live provider does not permanently revoke old descriptions. A replacement object
with the old name never inherits earlier ownership. Dispatch checks provider
availability and native write permissions where operations execute.

`FContentBrowserItem` retains only its constituent ItemData descriptions and
opaque payloads. Logical folders merge contributors by virtual path, independently
of their physical storage. Its Edit, Save, Duplicate, Rename, Copy and Move methods dispatch
through owners; the generic layer has no asset/file execution switch.
Source operations follow UE bool aggregation: a successful contribution makes
an aggregate action successful. Bulk copy/move partition by owner and call public
source overrides independently. There is no cross-provider preflight or generic
transfer cancellation. Providers enforce their own storage restrictions, report
failures independently and preserve earlier successful writes. Directory merge
and no-overwrite behavior are native responsibilities.

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
source and rechecks ownership afterward. Operation dispatch also checks active
provider admission across callbacks.
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
keeps the source-qualified identity of a contribution, including folder owners.
It adds no activation lifetime or mount-version fields.
Directory aggregation uses typed keys. Private native records stay inside provider discovery and storage utilities.
`StableId()` and the generic native metadata fields have been removed.

Filters are value-owned. `CompileFilter`, `EnumerateItemsMatchingFilter`,
`EnumerateItemsUnderPath` and `EnumerateItemsAtPath` use virtual paths. The
subsystem stores each source's immutable compiled filter directly, without an
admission wrapper. Native enumeration recompiles the raw virtual scope against
current mounts; withdrawal removes the source from service enumeration.

The Asset source owns an immutable catalog cache, explicitly invalidated by
package/catalog events and mount replacement, deriving asset rows and logical folders. The File source owns
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
returned values before launching presentation work. Worker captures contain
ItemData descriptions and retain immutable compiled filter values. Capture and published snapshot types distinguish raw
contributions from aggregated Items. Publication follows one explicit binding
path, with provider admission checks after binding callbacks; it has no fallback
that interprets an aggregate as an unbound description. The optional entry-status
callback is a deterministic native-I/O test seam, unused by ordinary requests.
There is no completed native snapshot supplied as an extra filter.

Owner-thread publication checks current source membership. There is no activation
counter, custom Session or copied project/mount identity in a capture. Async work
holds immutable inputs, and its owner controls cancellation and publication.
Preparation retains local input values and source/service ownership across provider
calls. Cancellation is registered before compilation/enumeration, and query lifetime
is checked before any later member access. Binding retains policy by value.
Each pending capture owns its cancellation token. Ordinary item events do not
cancel it; full refresh invalidates all pending captures.
Folder scopes normalize trailing separators and the root consistently. Binding
callbacks recheck cancellation before committing rows. Completed tasks are detached
before publication callbacks, so a reentrant refresh retains its new owned task.
Snapshot values use retained ownership and contents, without publication versions.
The model has one asynchronous worker/frame-pump path. Tests drive that same path;
waiting helpers and request-identity observations live in test fixtures, with no
production request counter or synchronous mode. Presentation freezes paths and
filter settings, collects provider values before sorting, and commits a range only
while its receiver and inputs remain current.
Each view request retains owner-thread item deltas received while its baseline
is queued or capturing. Publication binds the baseline, replays those deltas into
local shared records and commits the merged collection. Removed and moved old
identities cannot reappear from a late baseline. Events raised while binding the
baseline are included before replay; events raised during delta replay remain
queued for the next frame. Navigation replacement and source or
mount withdrawal revoke old publication rights. The temporary delta queue belongs
only to that request; views retain no public revision cursor or persistent journal.
Notification callbacks recheck receiver lifetime after provider calls.
Independent views keep separate cancellation/publication rights. Model destruction
drains owned worker bodies; prepared values remain readable after query destruction.

## Operations And Request State

Asset and File sources derive directly from `DContentBrowserDataSource`, own their
native execution and both contribute folders. AssetTools shares admission,
feedback and directory creation utilities; Core owns path
safety helpers. Native copy/move/save return bool directly. No generic
`OperationResult`, prepared-transfer context or cross-provider claim ledger remains.

Source feature version 18 uses UE bool aggregation: true means at least one
contribution succeeded. Item Copy/Move call single-item source actions; service
bulk calls public bulk overrides. Default bulk copy/move OR single-item results.
Save/rename/copy/move/edit/duplicate capabilities also OR supported contributions.
Rename and Duplicate aggregate successful admitted outputs. Duplicate succeeds
when the copy exists; copies remain Dirty until an explicit Save. Saving failure
leaves that copy available for retry. Independent failures retain earlier work; transfer has no
global preflight, rollback or cancel.

Capability queries do not reserve destinations or replace execution checks.
`CanRenameItem` accepts an optional name: absence checks permission; a proposal
adds name/target validation. Native copy never replaces existing files. File
moves verify bytes before source removal and stay within the source mount; Asset
uses object-based RenameAssets across writable mounts. Selected ownership, companions,
reparse points, final paths and mount boundaries remain provider responsibilities.
Save flags retain UE loaded/dirty predicates and reject unsupported flags;
`SaveOnlyIfLoaded` retains resident packages/exports during execution.

Binding stores immutable restrictions and optional services, not an execution
capability. Empty-context binding shares valid bound payloads; stale/foreign input
rejects. `ComposeOperationContext` updates feedback independently and conjoins
permission restrictions. Native payloads store unscoped values; actual invocations
create one `ScopeOperationContext` fence and retain source/input lifetimes.
Permission callbacks are checked before and after reentry; source publication is
gated by the live provider and current permission policy. Retirement revokes admission immediately,
while external notifications for committed changes remain deliverable.

Diagnostics use feedback or Core logging; actual changes use `FContentChangeBatch`.
Save collects only its own `OnSaved` changes; nested operations publish separately.
Providers keep local success/stop state for transfers and observed directory
effects. Asset staging, persistence/recovery evidence and deletion attempts remain
with their native owners. Creating an incidental parent directory does not make
an asset transfer succeed; its actual directory effect is still published. Backend diagnostics are forwarded without a browser result
ledger. Panels refresh after attempted commands, consume source updates and use
explicit outputs for selection. Query cancellation is independent of mutation.

`CreateFolder` returns `FContentBrowserItemDataTemporaryContext` without writing.
Its `ItemData`, validation and finalization callbacks retain the original directory
and admission identity. Finalization rechecks policy, returns source-owned ItemData
and consumes its callbacks on success; validation failure permits another name.
Discarding the context writes nothing. Validation checks current names, ownership
and permissions; no activation-generation revocation contract is imposed.
Temporary items never enter the published snapshot or ordinary selection.

Deletion uses `DeleteItem` and context-free `BulkDeleteItems`. Base bulk delegates
to single actions and ORs results. Item and service partition immutable inputs by
source and invoke admitted peers independently; a rejected provider does not
veto another provider. The data-source feature version is 18. There is no public
prepare, preview, request, validation, confirmation-context or lease protocol.

Native providers retain their own execution scope. Asset resolves its exact
selection and delegates model scanning, confirmation and object execution to
ObjectTools, with provider admission rechecked after the prompt. File owns its
physical scope and reuses the same private scan and confirmation helpers. The
panel passes its captured selection directly to the service.
Asset uses exact selected exports; File owns ordinary files and mixed folders,
including explicit folder expansion and empty-directory cleanup. Confirmation
never permits newly discovered entries to join an executing selection. Final
checks cover policy, mount/reparse/read-only state, live owners, providers and
fresh bytes. Partial removals retain private committed/failed/unattempted facts
for publication and diagnostics; a destructive attempt is consumed before
feedback can prepare another selection. No provider-wide preflight or rollback
is implied by the aggregate bool result.

## Incremental Updates

Sources call `QueueItemDataUpdate(FContentBrowserItemDataUpdate&&)`.
Added, Modified, Moved and Removed factories carry owned item data; moves retain
the previous virtual path. Native committed-effect batches generate these
updates, including generated copy destinations. Failed/no-effect operations
publish no optimistic update.

Added, Modified and Moved descriptions contain the complete current display
metadata and native payload; consumers must not enumerate a directory to hydrate
an update. Removed descriptions require only the old source-qualified identity.
Sources expose owner-thread `ConvertItemForFilter` and `DoesItemPassFilter` for
individual updates. The default conversion leaves the description unchanged;
the default predicate checks ownership, virtual scope, recursion and item kind.
Specialized filters override these hooks to match their enumeration semantics.
Native producers hydrate only the affected asset or filesystem entry.
Asset labels use the package name for one asset and object names for multiple
assets. Indexed package membership supplies the same labels in enumeration and
events; membership-mode changes emit Modified events for affected siblings only.
Asset sources maintain parent-folder
membership counts from asset deltas, including inactive-source bookkeeping without
publication. Per-item folder checks accept current Asset folders and mounted roots;
empty ordinary folders belong to the File source. Folder transitions are coalesced
per batch and published after file moves, preserving selection migration.

Baseline results retain the immutable compiled provider filters used to produce
them. `ProjectItem` converts and checks one delta against those filters and binds
it with the receiving query's policy, rechecking source admission after callbacks.
This avoids compiling or capturing a new provider range for each item event.
Full-refresh batches use `NotifyItemDataRefreshed`, rather than disguising unknown
coverage as a Modified root folder.

The subsystem detaches its pending batch before broadcasting `OnItemDataUpdated`;
reentrant updates enter a later batch. Membership/mount changes and explicit
`NotifyItemDataRefreshed` requests coalesce into `OnItemDataRefreshed`. Views keep
pending invalidations while paused or hidden. There is no service revision,
frame-snapshot getter or reader cursor. Scoped item events preserve unrelated
caches; full refresh invalidates owned caches and pending requests explicitly.

Asset sources subscribe to `OnAssetQueryChanged`. Package/catalog invalidation
queues coalesced owner-thread publication through the existing deferred executor;
queries also synchronize before returning values. There is no query Tick API or
provider-driven query polling. Registry publication broadcasts after releasing
its state lock. Snapshots commit before callbacks, and reentrant events remain
queued behind the active broadcast. Persisting a live row modifies it rather
than adding it twice. Source shutdown removes every subscription.

Operation contexts and hosts retain one `NotifyContentMutation(FContentChangeBatch)`
feedback channel for actual effects; unknown scope uses `bFullRefresh`. MainFrame
adapts committed batches to the Transactor event. External notifications survive
provider retirement, while source projection remains fenced. Asset deltas come
from registry events rather than operation-result replay.

Folder and asset actions compose `FContentMutationPolicy` callbacks through small
admission, feedback and notification helpers; they do not inherit an operation
base. Fix-up keeps the UE-style `IAssetTools::FixupReferencers` void contract:
the Browser bool reports request handling, while feedback reports failures.
Its host notification reads committed package changes from the existing catalog
journal; asset rows still arrive through query events. Empty work and rejection
before any effect emit no mutation batch. A journal gap or a newly fenced projection after
failed publication retains the conservative full-refresh fallback.

File sources own one cancellable background metadata snapshot per editor. At
most one scan runs per source; steady-state starts are spaced by at least half
a second of editor time. Workers capture owned mount paths and cancellation only, without
source, editor, registry or UI access. One discovery state owns its task and
cancellation token. Mount events and committed local mutations cancel and join
the old task before replacing that state and rebuilding its baseline.
Completion detaches the task before callbacks; publication checks that the
source is active and still owns that discovery state. Shutdown drains work before module retirement. Missing task scheduling leaves discovery dormant
in minimal test hosts. Incomplete scans invalidate root coverage and never invent
removals. Package/bulk changes reconcile the asset registry on the owner thread;
companion ownership stays with Asset. Views merge published item updates and
overlay them on pending baselines; the File source retains no discovery revision
or change journal.
This is metadata polling because Durin has no
portable directory-watcher API: same-size rewrites preserving timestamps require
explicit reconciliation. It is not equivalent to an OS change-event stream.

## Panel Boundary

The host supplies a weak `DataSubsystem` binding. Each panel owns its query state,
selection, navigation, capture cancellation, temporary creation and confirmation;
no panel owns a mutation request context. Queries publish through source
enumeration, and menus, shortcuts, clipboard and drag/drop use item capabilities
and data-service batch routing with virtual destinations.

The engine pumps deferred metadata events before editor/view work. The data service
runs without browser views or Worlds. Each view owns its event subscriptions and
invalidations. Selection trimming waits for pending move/delete events when rows
refresh before owner publication, then resumes after migration.
Internal drags retain the original owner-bound items. Transfer commands resolve
current items with the destination view's policy and require the original source
objects to remain available. Activation changes alone do not create new item
identities; destination policy is applied when the command executes. Single-asset drags retain the existing external payload
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
by logical key and compare source-qualified contribution keys. Image decoder cache keys remain native resource paths and asset
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
AssetTools publishes native directory facts with their origin through
`OnContentDirectoriesChanged`. Each active provider subscribes and creates only
its own contributions; no provider looks up a sibling by name or writes sibling
updates. Origin-owned publication is not replayed to the origin, and subscribers
resolve the owning editor through the outer chain, supporting both editor-owned
and data-subsystem-owned sources while excluding other editor hosts. Notifications
carry Mutation or Discovery provenance. Only mutations retire File's in-flight
discovery state and rebuild its baseline; peer discovery facts update projections
without restarting discovery. This prevents multiple File sources from feeding
full-refresh scans back into one another. Other editor services reconcile disk
changes through their own catalog and filesystem discovery streams. Subscriptions
are removed before source shutdown; callbacks retain the source during dispatch.
Unrelated merged folders use per-contribution result dispatch, retaining independent successes and failures.
