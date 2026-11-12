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
The source feature interface version is 5: flags-only descriptions, removed
generic storage fields and the source-owned operation-equivalence predicate
require consumers to rebuild alongside the item attribute/physical-path hooks
extend the vtable and ItemData owns its name/display/flag description. Providers
and consumers compiled against an earlier interface/layout must rebuild.

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
mount-registry revision, flags, kind, virtual path, item name, display name and shared immutable
`IContentBrowserItemDataPayload`. Only the source interprets that payload.
`GetOwnerDataSource` returns null off the owner thread, after source withdrawal,
deactivation/reactivation, editor shutdown or mount replacement. A new source
with the old name never inherits earlier handles.

`FContentBrowserItem` retains only its constituent ItemData descriptions and
opaque payloads. Logical folders merge contributors by virtual path, independently
of their physical storage. Its Edit, Save, Duplicate, Rename, Copy and Move methods dispatch
through owners; the generic layer has no asset/file execution switch.
Unrelated multi-source moves and copies reject before mutation. The two native
adapters explicitly support mixed native batches through the same atomic backend;
their duplicate physical-folder contributions retain one executable identity.
Other providers' logical folder contributions remain distinct. Folder operations
with multiple unrelated owners cannot silently choose one provider.

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
folders. `DContentBrowserNativeDataSource` shares the native adapter mechanics;
the generic abstract source retains UE-style unsupported defaults.

Both native sources reuse `FContentBrowserRequestContext`, the single existing
mutation/recovery substrate. This replaces the misleading private
`FContentBrowserDataSource` name, rather than adding another operation engine.
Payloads retain shared immutable request-local policy and feedback, not a mutation
executor or deletion-session container. Providers create execution contexts for
individual commands; only pending deletion and temporary creation retain execution
state across calls. Same-source batches from different captures combine their
admission predicates and execute the common operation implementation once; one
query scope is not a new execution identity. The original activation and mount
admission identities remain captured in policy, including on returned items.
Provider discovery preparation is independent of mutations.
`FContentBrowserDataQuery` retains only provider-neutral request, binding and
revision state with a weak service binding.

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
