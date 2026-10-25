# Python Reflection Bridge Plan

Summary: Add a generic runtime Python binding over native reflection, using EditorActorSubsystem as the first production consumer.

Last reviewed: 2026-10-19

Status: Archived
Completed: 2026-10-19

## Current Status

Stages 0-1 completed on 2026-10-18. Stage 1 implementation and validation use
baseline `a7a7d0548` plus the source manifest recorded below. Generic native
classes/structs/enums are published as interpreter-owned heap types, and exposed
editor subsystem objects support identity, inheritance and guarded bootstrap
introspection. Stage 1 has no Actor-specific binding code. Stage 2 completed on
2026-10-19: generic method descriptors, checked values, native dispatch and
readable reflected attributes now work with real CPython. Mutable detached
structs support named construction and field edits. Stage 3 completed on
2026-10-19: generic editor-property helpers and writable attributes share native
validation, notification and transactional editing. Stage 4 completed on
2026-10-19: the production EditorActorSubsystem exposes reflected queries and
all six Actor edits through the generic bridge. The Stage 4 follow-up simplifies
native and script edit returns to pointers, arrays and bool. Stage 5 now provides
native member documentation and deterministic IDE declarations from the published
registry. Stage 5 completed on 2026-10-19 after final Windows integration,
disabled-runtime/restoration, consumer-build and documentation acceptance.
The Stage 2 implementation is organized into independent private translation
units for registry, objects, values, conversion and members; its public script
and native reflection contracts are unchanged.

UE alignment changes two initial decisions: object proxies participate in native
GC reference collection with explicit purge, replacing the initial weak-only
proposal; the selected return/out contract follows UE's single-value/tuple and bool-success
conventions. Durin retains explicit Level targets, per-Host admission and native
edit results. Existing Toolset, MCP and Python job APIs remain supported.

## Goal

Allow scripts to obtain an editor-owned subsystem and call its exposed methods
through normal Python objects. Native reflection remains the authoritative type,
signature, default-value and dispatch model. Validate the generic bridge with
independent fixture types before onboarding Actor operations.

The intended accessor shape is:

```python
import durin

actors = durin.get_editor_subsystem(durin.EditorActorSubsystem)
```

This is a target API, not an available API. The frozen Actor API and complete
examples below become runtime acceptance fixtures in Stage 4.

## Baseline and Ownership

- DurinPython currently wraps Toolset values and session-owned jobs; it does not
  expose engine-object proxies. Its pinned CPython ABI is private and optional.
- CoreDObject already provides DClass/DProperty/DFunction, inherited lookup,
  generated aggregate lifetimes, FFunctionParameterBuffer, reflected defaults,
  and DObject::ProcessEvent. Reuse these instead of a second invocation ABI.
- DEditorEngine currently exposes a typed GetEditorSubsystem<T>(); dynamic lookup
  by a runtime DClass needs a public, validated native boundary.
- DEditorActorSubsystem is reflected as a class, but its editing methods are not
  DFUNCTION entries. Their span/callback/context/result types are not directly
  usable as reflected signatures.
- DurinEd owns editor admission, Actor edits, property-edit notifications and
  history. DurinPython owns CPython types, conversions and script-job binding.
  Neither CoreDObject nor DurinEd acquires a Python dependency.
- Shared API searches and consumer migration cover Engine, Sandbox and
  RoadWeaver, as declared by Durin.dworkspace.

## Frozen Stage 0 Contracts

These are selected implementation requirements, not descriptions of implemented
behavior. Change this plan with rationale before deviating.

### UE reference and deliberate differences

The implementation reference is UE 5.3.2 mirror revision
`c865e168d0935b8e5f4bd865ddcc1c733c8ce7cf`, matching the existing transaction
reference. The following source files were retrieved and inspected during Stage 0:

| UE source under PythonScriptPlugin/Private | Mechanism to reuse conceptually |
| --- | --- |
| `PyGenUtil.cpp`: ShouldExport*, IsScriptExposed*, naming and PackReturnValues | Positive exposure, editor-property access, names and output packing |
| `PyWrapperTypeRegistry.cpp`: GenerateWrappedClassType, GenerateWrappedTypesForReferences | Runtime types, inheritance and signature dependency closure |
| `PyWrapperObject.cpp`: CallFunction*, FPyWrapperObjectMetaData::AddReferencedObjects | Shared call logic, instance factories and native GC edges |
| `PyReferenceCollector.cpp`: AddReferencedObjects, PurgeUnrealObjectReferences | Reference enumeration and explicit object purge |
| `PyUtil.cpp`: IsInputParameter/IsOutputParameter | Reflection parameter roles |

Use CPython heap types, native wrapper factories and descriptor closures carrying
native metadata, not generated executable Python business classes. IDE stubs are
separate. UE Blueprint exposure becomes language-neutral Script exposure here;
Durin has no Blueprint dependency. The initial bridge deliberately uses copied
struct/container values rather than UE's full reference/view container behavior.
Python subclassing of native classes, Python-generated native types, delegates,
callbacks and native-to-Python virtual overrides are outside this release.

### Exposure and names

Freeze these flat authoring specifiers; they are additions for Stage 1, not
currently accepted syntax:

| Declaration | Eligibility and semantics |
| --- | --- |
| `DCLASS(ScriptType)`, `DSTRUCT(ScriptType)`, `DENUM(ScriptType)` | Publish a type; no automatic member or UObject-style constructor grant |
| `DFUNCTION(ScriptCallable)` | Publish this method independently of CallInEditor; no Toolset/MCP registration |
| `DPROPERTY(ScriptReadOnly)` | Readable direct attribute |
| `DPROPERTY(ScriptReadWrite)` | Readable/writable direct attribute, subject to native editor admission |
| `ScriptName = "Name"` | Explicit script name on a type, function, property or enum value |
| `ScriptNoExport` | Exclude the declaration from script access, including editor-property helpers |

ScriptReadOnly/ScriptReadWrite are mutually exclusive. ScriptReadWrite combined
with existing ReadOnly is a DHT error. On native object fields ScriptReadWrite
also requires Edit and excludes NonTransactional; detached struct fields do not
require Edit. ScriptNoExport wins over exposure and
excludes a callable whose signature requires that excluded type. DHT diagnoses
unknown/duplicate/malformed keys; generated registration validates equivalent
handwritten records. Extend EFunctionFlags/EPropertyFlags and typed exposure/name
metadata; do not reinterpret Edit, CallInEditor or serialization flags.

A class with exposed members is eligible without ScriptType. Ancestors and types
referenced by an exposed signature receive dependency wrappers without exposing
unmarked members or enabling construction. Descendant instances preserve actual
runtime type/inheritance through the same dependency wrapping. Enums needed by
signatures expose their declared, non-excluded constants. Methods/properties are
inherited through normal lookup; a native override needs its own ScriptCallable
marker to remain callable. An unmarked or excluded effective override must mask
an inherited descriptor, including calls through cached base bound methods.

Canonical identity is the native qualified type identity, never an alias.
`durin.get_type("Durin::DEditorActorSubsystem")` is the unambiguous lookup.
Convenience exports strip the native D/A/F/E type prefix, preserve type case,
convert members/parameters to snake_case, remove a boolean b prefix only before
an uppercase letter, and expose enum entries in UPPER_SNAKE_CASE. ScriptName
replaces the inferred source name before member/enum case conversion. Reject
keywords, invalid identifiers, reserved wrapper members and ambiguous names with
both native identities in the diagnostic. An ambiguous top-level alias is omitted
for all colliders; qualified lookup remains available. Distinct inherited members
colliding after conversion reject that type's export; a valid override is not a
collision. No silent overwrite or legacy alias support in this release.

Object classes, including subsystem/Actor classes, cannot be constructed or
subclassed from Python. Native creation stays with editor services. Mutable
ScriptType structs allow default construction and named, writable field
initializers; read-only result structs are returned by native code only. Intrinsic
math wrappers expose their reflected fields plus default/named-field construction,
without a handwritten Python class or required arithmetic operators. Names and
GUIDs have value construction from validated strings. Enums require their exact
registered type; do not accept arbitrary integers as enum arguments.

### Native GC, identity and retirement

Replace the original weak-only design with UE-style reference collection. A live
object wrapper enumerates a native GC edge to its target and type metadata.
Use existing native reference-collector/strong-reference facilities, not a new
GC. The factory/cache holds weak Python observations, so the registry does not
immortalize every returned instance. Release edges when wrappers die or purge.
Argument buffers and detached structs/containers enumerate nested object edges;
CPython cyclic traversal/clear must also cover all Python references they own.

Track a wrapper by `(interpreter generation, Host FObjectKey, endpoint work-gate
identity, target FObjectKey)`. Retain a weak Host reference, not an owning session
or Host field. Object keys distinguish address reuse. Equality/hash compare these
identities and stay stable after invalidation; repr/equality/hash never dereference
native storage. Within one live factory entry, repeated wrapping returns the same
Python object; equality remains reliable after cache eviction. None represents a
native null. Type descriptors and reference-free values may be shared across jobs;
values containing objects inherit their Host origin and enforce it recursively.

A later job on the same live Host/endpoint may reuse a proxy; another Host or a
replacement endpoint may not. Each native read/call/write requires an active job,
matching origin, open work gate and valid object. Cached bound methods do not
cache admission. Semantic Actor/Component membership is revalidated separately
from GC liveness. An Actor temporarily removed by undoable history is retained
and can regain membership under the same key after Undo; it is not permanently
purged merely because a structural command removed it.

Before Host/endpoint close, subsystem retirement, irreversible object destruction
or metadata teardown, purge matching Python object edges, invalidate bound access
and remove factory entries. Purging occurs on GameThread at a safe boundary;
physical GC cannot run between conversion and rooted dispatch/result capture.
Synchronous native callbacks may close or retire participants: preserve temporary
roots for call safety, recheck the gate before wrapping results, and never publish
a new usable proxy for a retired participant. Host shutdown is explicit and cannot
be delayed by an imported Python global keeping its scene alive. Closing a single
session cancels its jobs but does not invalidate other sessions on the same Host.

Stop closes jobs/scopes, clears module globals and Python caches, releases all
wrapper/value edges, then finalizes CPython. No pointer or root crosses an
interpreter generation. Module changes are additive at script-pump safe points.
The first release binds types only from native modules that disallow dynamic
unloading, plus process-lifetime intrinsic types. Capture owning-module provenance
from compiled-in registration; unresolved provenance or a dynamically unloadable
owner rejects export with a diagnostic. Stopped providers are not callable even
if their library is mapped. Stop the interpreter before final reflected-module
teardown. Runtime type reinstancing/unload is deferred rather than copying UE's
orphan/reinstance subsystem without matching native support.

### Values, calls and errors

Reuse linked DFunction properties, FFunctionParameterBuffer lifecycle/defaults,
and DObject::ProcessEvent; never call a native thunk directly. Validate full
arguments before native effects. Resolve the effective override and its script
eligibility on every call. Positional/keyword arguments support exact converted
names; duplicate, unknown and missing required arguments fail. ApplyDefaults only
for omitted inputs with Reflected defaults. NativeOnly defaults remain required
Python arguments, including native struct-construction defaults.

| Native kind | Python value and checks |
| --- | --- |
| bool / fixed-width integers | bool / int; bool is not an integer argument; preserve signed/unsigned range |
| float / double | finite int or float within destination range; reject bool, NaN and infinity |
| string / FName / FGuid | str / Name / Guid; Name and Guid inputs also accept validated str |
| reflected enum | Exact enum type and declared value |
| DObject pointer / TObjectPtr | Compatible valid object proxy or None; preserve actual native class |
| DClass pointer | Registered Python native class maps to DClass; native subclass/constructibility checks still apply |
| reflected struct / intrinsic math | Exact typed owned value; default/named-field construction only where allowed |
| vector / fixed property arrays | list or tuple input, fresh list output; exact fixed length; no iterator execution |
| unordered_map | dict with schema-compatible scalar/name/GUID/enum keys; fresh dict output |

Containers nest at most four levels, matching DHT. Detect cycles and conversion
key collisions before writes; cap each conversion at 65,536 visited value nodes,
16 MiB of owned converted payload and a 64-level struct/value recursion limit.
Use existing stricter native limits when applicable. Budget accounting covers
output conversion as well as inputs; an output-budget failure does not undo a
native operation already completed. Byte/blob/bulk-data, soft/weak object value
APIs, arbitrary pointer/reference returns and unbound signatures are unsupported.
Unsupported exposed signatures reject their descriptor with a precise path;
never produce a callable that may fail only after native effects.

Values and container properties are copies; nested field reads are also copies.
Write modified nested structs back to their containing value, then to the object.
Object elements remain proxies. Mutable-reference parameters marked input/output
remain required inputs; const references are inputs only. Pack outputs in native
return-first, out-parameter declaration order: no outputs -> None; one -> that
value; multiple -> tuple. Match UE's bool-plus-out convention: false -> None;
true -> pack only the out values. A lone bool return remains bool. Actor edit
results use a struct and do not use this bool-success convention.

Use TypeError for wrong types/arguments, OverflowError for numeric overflow,
ValueError for invalid enum/name/GUID/value or conversion budgets, ReferenceError
for destroyed/purged/stale targets, and AttributeError for hidden/unknown fields.
Add `durin.ScriptAccessError(RuntimeError)` for wrong/missing job, Host, module,
thread or native admission; retain existing CancelledError for cancellation and
closed endpoint admission. A generic invocation error includes qualified type,
function/property and argument path. Once a valid Actor edit reaches its domain
service, return its Failed/NoOp/Changed result; do not turn domain failure into a
binding exception. A typed native FEditorScriptCallError allows native adapters to
reject query/admission without throwing C++ exceptions; the bridge checks it
before converting default return storage. A nested call cannot replace another
call's error or Host context. No native exception becomes a rollback contract.

### Native script context and authoritative editability

Add a language-neutral `FScopedEditorScriptCall` in DurinEd. Each bridge entry
binds the current job's validated Host/endpoint gate, source/client cancellation
and non-document access policy; RAII restores the previous context. It exists
only during synchronous dispatch, including property helper dispatch, and never
spans a yield. DurinPython supplies it after its job checks. Native callers can
use the same seam; DurinEd never imports Python and arguments cannot fabricate it.
Keep ToolService's existing FToolContext binding unchanged.

Add `DEditorEngine::GetEditorSubsystem(DClass*)` delegating to the existing native
collection; require GameThread, editor-subsystem ancestry and a live registered
instance. The script accessor also checks current Host/gate. It returns an
existing instance, never creates one. A known compatible type without a live
instance raises ScriptAccessError; an incompatible type raises TypeError.

Add Host-owned, RAII-registered target edit-policy callbacks in DurinEd, keyed by
Level identity and provider lifetime. Resolve Actor/Component targets to their
owning Level. A document policy reports Writable, ReadOnly or Unavailable by
reading its live owner state on every edit. Duplicate/conflicting registrations,
expired callbacks or absent authority in document-managed mode reject writes.
LevelEditor registers this adapter beside its FLevelEditorContext lifecycle and
unregisters before destruction; DurinEd does not include its private headers.
Closing/replacing the document invalidates the policy before another call.

Headless use is explicit: add FPythonSessionOptions with an EPythonEditPolicy
value DocumentManaged (default), NonDocumentReadOnly or NonDocumentReadWrite.
Append defaulted options to CreateSession after its existing parameters, preserving
current source callers. Translate this public Python-module option privately to
the language-neutral DurinEd call policy; keep DurinPython's DurinEd dependency
private and its public header free of editor implementation types.
These are not Python arguments and
EToolSource::Native does not itself grant write access. Non-document policy is
used only without a document policy; any registered read-only/unavailable policy
wins. Document-managed sessions may mutate only with a live Writable document
policy; no policy means deny. CPU fixtures explicitly select NonDocumentReadWrite.
Existing editor/MCP entrypoints keep DocumentManaged, with no new remote switch.
Document ownership changes during callbacks are rechecked before publication.

Native wrappers derive FActorEditContext from this Host and policy result, never
from a script bool. Structural operations retain native stopped-PIE/current-Level,
class/hierarchy/reentrancy/transactor validation. Pure queries may inspect the
Host's current editor Level during PIE but never the PIE World. They require no
transaction and return a single synchronous snapshot.

### Property helpers and transactions

`get_editor_property(name)` resolves the same converted script property name and
reads an Edit property, respecting ScriptNoExport;
`set_editor_property(name, value)` additionally requires !ReadOnly,
!NonTransactional, supported kind and writable Host target admission. Direct
attributes require ScriptReadOnly/ScriptReadWrite respectively. All writes to
native objects, including direct writable attributes, go through the same
DurinEd detached-validation/property-edit session and Host transactor. A direct
attribute cannot bypass read-only documents, notifications or undo. No option
suppresses notifications. Pure detached struct field assignments need no history.

For the first production adapter, writable native targets are live Actors and
owned Components in the admitted editor Level. Unclassified/global/asset objects
remain read-only until their native owner provides target policy; exposing a
property does not grant ownership. The bridge changes no runtime setter behavior.

Reuse ScopedEditorTransaction; reflection itself opens no blanket transaction.
Domain structural commands and one-shot property edits join the same Host
recording. Completed edits survive a later exception/cancellation. Short
synchronous scopes are recommended; existing cross-yield recording semantics
remain unchanged.

### Frozen EditorActorSubsystem script API

Add reflected native members to DEditorActorSubsystem using C++ names prefixed
`Script`, ScriptCallable and explicit ScriptName values below. The simplification
follow-up also migrates existing native methods to direct pointer/array/bool
returns. The new members take DLevel*/AActor* and
const vector references instead of non-reflectable references/spans/callbacks;
forward to the existing service. Query methods are const. Edit methods return
Actor pointers, Actor arrays or bool, without script result wrappers. Transform
is required on spawn because its native
construction expression is not a reflected default; name uses std::string = "".
Attach/detach rules get reflected DENUM registration and enum defaults.

| Python method | Reflected native parameters / return |
| --- | --- |
| `get_editor_level()` | `() -> DLevel*`; None only when no editor Level exists |
| `get_all_level_actors(level)` | `(DLevel*) -> vector<TObjectPtr<AActor>>`; native Level order, live published actors only |
| `get_actor_info(level, actor)` | `(DLevel*, AActor*) -> FActorScriptInfo` |
| `spawn_actor(level, actor_class, transform, name="")` | `(DLevel*, DClass*, const FTransform&, const string&) -> AActor*` |
| `destroy_actors(level, actors)` | `(DLevel*, const vector<TObjectPtr<AActor>>&) -> bool` |
| `duplicate_actors(level, actors)` | `(DLevel*, const vector<TObjectPtr<AActor>>&) -> vector<TObjectPtr<AActor>>` |
| `rename_actor(level, actor, name)` | `(DLevel*, AActor*, const string&) -> bool` |
| `attach_actors(level, actors, parent, rule=AttachmentTransformRule.KEEP_WORLD)` | `(DLevel*, const vector<TObjectPtr<AActor>>&, AActor*, EAttachmentTransformRule) -> bool` |
| `detach_actors(level, actors, rule=DetachmentTransformRule.KEEP_WORLD)` | `(DLevel*, const vector<TObjectPtr<AActor>>&, EDetachmentTransformRule) -> bool` |

No mutation infers its Level or participants from selection/focus. Match existing
native naming/conflict, batching, no-op and result order rules; do not add bridge
batch loops or promises of rollback beyond native guarantees. Spawn leaves its
native Configure callback empty. Asset-specific placement, component structural
methods, UI selection/focus and specialized Transform setters remain deferred.

The selected simplification removes edit/history result structs and status/error
enums. Spawn returns Actor or None, duplicate returns an Actor array or an empty
array, and the other edits return bool. Successful no-ops return true without a
new transaction. Domain failures do not carry error strings or history payloads;
detailed recovery state remains native in DTransactor.

FActorScriptInfo contains `actor: Actor`, `name: str`, `actor_class: type[Actor]`,
`parent: Actor | None`, `children: list[Actor]`, `root_component: SceneComponent |
None` and `world_transform: Transform`. Capture direct child Actors in Level order;
use existing native transform behavior for rootless Actors. These snapshots do
not grant structural property writes. RelativeTransform on the root Component is
already Edit and becomes the initial generic transform-edit target.

The generic Python bridge converts direct native return values and the read-only
info snapshot, with no Actor-specific codecs.
Pre-dispatch invalid/stale object arguments raise binding exceptions; live but
invalid domain targets return domain failure for edits and a native script-call
error for queries. Query rejection never masquerades as an empty list.

### Intended script acceptance examples

These scripts run after Stage 4 in the existing real-CPython CPU editor fixture.
The fixture owns a current Level with at least one constructible rooted Actor,
and explicitly selects NonDocumentReadWrite. The same calls in an interactive
editor require a live writable Level document.

```python
import durin

actors = durin.get_editor_subsystem(durin.EditorActorSubsystem)
level = actors.get_editor_level()
if level is None:
    raise RuntimeError("No editor Level")

seed = None
for actor in actors.get_all_level_actors(level):
    info = actors.get_actor_info(level, actor)
    print(info.name, info.actor_class, info.world_transform)
    if seed is None and info.root_component is not None:
        seed = info
if seed is None:
    raise RuntimeError("This example needs a constructible rooted Actor")

with durin.ScopedEditorTransaction("Create and move preview Actor"):
    preview = actors.spawn_actor(
        level, seed.actor_class, seed.world_transform, name="PreviewActor"
    )
    if preview is None:
        raise RuntimeError("Actor creation failed")
    if not actors.rename_actor(level, preview, "PreviewActorRenamed"):
        raise RuntimeError("Actor rename failed")

    root = actors.get_actor_info(level, preview).root_component
    relative = root.get_editor_property("relative_transform")
    translation = relative.translation
    translation.x += 100.0
    relative.translation = translation
    root.set_editor_property("relative_transform", relative)
```

A second fixture script checks the direct return values for the remaining operations:

```python
with durin.ScopedEditorTransaction("Duplicate and attach preview"):
    copied = actors.duplicate_actors(level, [preview])
    if not copied:
        raise RuntimeError("Actor duplication failed")
    duplicate = copied[0]
    def require_success(result):
        if not result:
            raise RuntimeError("Actor edit failed")

    require_success(actors.attach_actors(level, [duplicate], preview))
    require_success(actors.detach_actors(level, [duplicate]))
    require_success(actors.destroy_actors(level, [duplicate]))
```

Native tests drive Undo/Redo after each script and assert same object identity,
scene membership, hierarchy and transform restoration. Failure-path scripts also
assert that a rejected operation never publishes edits; previously completed
operations are not implicitly rolled back.

## Stage 0 Audit and Validation Selection

Audit baseline: `376054b4e`, 2026-10-18. Symbol inventory used
`DEditorActorSubsystem|FActorEditContext|FActorEditResult|GetEditorSubsystem|FFunctionParameterBuffer|EFunctionFlags|EPropertyFlags|CreateSession|IEditorScriptPump|FSubsystemWorkGate`
across Source and Tests under Engine, Sandbox and RoadWeaver. It found 68 Engine
source files and 36 Engine test files, and no matching Sandbox/RoadWeaver files.
Receipt: `Build/PythonReflectionBridge/stage0-symbol-inventory.json` (ignored).
No production symbol changes or consumer migrations are part of this stage.

| Owner / source | Verified baseline and implementation impact |
| --- | --- |
| CoreDObject ObjectMacros.h, FunctionTypes.h, Class/Function registration | No script flags yet; extend native/generated exposure metadata and retain immutable linked signatures |
| DHT function_parser.py, property_parser.py, type model/generation | Function specifiers are currently strict; add explicit grammar, validation and generated fields for new keys |
| CoreDObject ObjectKey/StrongObjectPtr and reference collectors | Serial/index identity and GC rooting exist; reuse and add wrapper enumeration/purge |
| Core ModuleManager | Dynamic unload is opt-in; reject unloadable type owners in initial bridge, check provider state, order final shutdown |
| Engine Subsystem collection and DurinEd EditorEngine | Runtime Find(DClass*) already exists underneath typed editor lookup; add checked editor overload, not a new collection |
| DurinPython PythonRuntime.cpp, PythonABI.h | CurrentEditor is rebound per pump step and cleared for cleanup; extend with scoped native invocation context, heap-type APIs and generation-owned state |
| DurinEd PropertyEditing.h | Begin/Apply/Commit and Host transactor are reusable; script must resolve target authority before opening a one-shot session |
| DurinEd Actor subsystem/mutations | Existing domain admission, result and structural history remain authoritative; only add reflectable wrappers/query/results |
| LevelEditor FLevelEditorContext | bReadOnly is currently private document state; register a live native target-policy adapter, including close/replacement |
| PythonRuntime::CreateSession callers | Module editor startup, PythonToolset session cache and native fixtures need explicit options propagation; existing callers retain DocumentManaged defaults |

Configured registry discovery used `./DevTool test list` for Python, reflection,
CoreObject, EditorOperation, EditorProperty, LevelMutation, world, MCP, Sandbox
and RoadEditor on preset `MacOS-arm64-Debug-DurinEditor`. Concrete selections:

| Change / stage | Validation selection |
| --- | --- |
| Exposure/DHT/CoreDObject, Stages 1-2 | DHT parser/generator pytest suite; CoreObjectTests (including DFunctionReflectionTests), CoreObjectReplacementTests, CorePropertyChangeTests, CorePropertyValueSnapshotTests |
| New generic bridge, Stages 1-2 | Add PythonReflectionIntegrationTests to PythonTests.cmake with real CPython and two independent reflected fixtures; keep disabled-runtime tests Python-free |
| Context/Host/subsystem policy, Stages 1 and 4 | EditorOperationTests, WorldTests subsystem cases; isolate close/retire/reentrancy and A/B/A Host cases |
| Property writes, Stage 3 | EditorPropertyTests plus bridge real-Python normalization/read-only/notifications/Undo cases |
| Actor adapter, Stage 4 | LevelMutationTests Actor/attachment cases, WorldTests Actor subsystem cases, bridge scripts above |
| Compatibility, Stage 5 | PythonToolIntegrationTests, MCPPythonJobTests, MCPCodecTests, MCPIntegrationTests; PythonRuntimeUnavailableTests in the Python-disabled configuration |
| Native consumers, Stage 5 | Owning CoreDObject/Engine/DurinEd/DurinPython plus LevelEditor/MainFrame/DurinMCP and the DurinEditor executable (DurinLauncher build target); compile SandboxGameplayTests and RoadEditorIntegrationTests for cross-project shared-header consumption |

There is no configured standalone Function or Subsystem test target: function
reflection belongs to CoreObjectTests and editor subsystem cases belong to
EditorOperationTests/WorldTests. The current Python-enabled registry excludes
PythonRuntimeUnavailableTests by design; its disabled-configuration registration
is verified in PythonTests.cmake. Registry lookup is discovery, not test execution.
DHT tests use the host DevTool's supported selection after registry/tooling
inspection; no raw native test bypass or broad acceptance run is authorized by
this planning receipt. Before runtime execution, re-resolve affected selections
and apply the build/test workflows below.

Stage 0 validation: changed-document and all-plan validation passed; diff and
example syntax were reviewed. No native build/test ran or is needed for this
contract-only stage. Later stage checklists and runtime gates remain open.
Source provenance/hash receipt:
`Build/PythonReflectionBridge/stage0-ue-source-audit.json` (ignored).

## Stage 1 Implementation and Acceptance

The private PythonReflection registry creates native heap types and maintains
non-owning instance-factory observations. Existing FStrongObjectPtr supplies GC
edges. Object invalidation notifications from CoreDObject and subsystem retirement
purge those edges before physical collection; retirement closes its work gate
before notifying observers. The native call context rejects reconciliation and
restores nested state. LevelEditor now registers live Level document policy through
its context, without a DurinEd dependency on LevelEditor internals.

Exposure records carry ScriptType/ScriptCallable/name/exclusion data, member flags
and generated owning-module provenance. The active/non-unloadable module rule has
one explicit foundation case: CoreDObject is linked for process lifetime and has
no IModuleInterface to query. Core bootstrap metadata is also process-lived.
The process-resident native test fixture uses DurinPython as its test lifetime
owner; production writers always emit the actual declaring module.

Two scope clarifications preserve the selected architecture: get_name/get_class
are generic native object bootstrap helpers, as in UE's object wrapper; they do
not implement per-business-class Python bindings or DFunction invocation. Struct
and enum heap types are registered in Stage 1, but instances/construction and
conversions remain Stage 2. Imported globals are released by interpreter
finalization after wrapper edges and native registries have been purged.

Final source manifest: `Build/PythonReflectionBridge/stage1-source-manifest.json`,
SHA256 `35c3a3852e1df533e705fd1b823ae40f4d4fd330689b8e08967fa5c62e64b27d`. It records
individual non-document input hashes, native baseline, preset and interpreter.
All receipts below are under ignored `Build/PythonReflectionBridge/`.

| Gate | Result / receipt |
| --- | --- |
| Final runtime consumers across workspace projects | `all` passed; `stage1-all-final.log` (initial broad build: `stage1-all-build.log`) |
| DHT parser/generator | 435 passed; `stage1-dht.log` |
| New bridge with real CPython | 12 passed grouped and all 12 passed independently, serial isolation; `stage1-PythonReflectionIntegrationTests-final.xml`, `stage1-isolated.xml` |
| Reflection contracts and persistence | All 8 selected domain targets passed; `stage1-reflection.xml`; the bridge target's earlier 11-case receipt is superseded by the final 12-case receipts |
| Native editor/subsystem lifetime | EditorOperationTests 125 and WorldTests 174 passed on final retirement order; `stage1-EditorOperationTests-final.xml`, `stage1-WorldTests-final.xml` |
| Existing Actor domain | LevelMutationTests 51 passed; `stage1-LevelMutationTests.xml` |
| Existing Python and MCP jobs | PythonToolIntegrationTests 47 and MCPPythonJobTests 14 passed; corresponding `stage1-<Target>.xml` |
| Cross-project native consumers | SandboxGameplayTests 13 and RoadEditorIntegrationTests 8 passed; corresponding `stage1-<Target>.xml` |
| Optional runtime | PythonRuntimeUnavailableTests 1 passed with Python OFF; `stage1-disabled.xml`; ON configuration restored and final bridge/isolation/all gates passed |

The bridge cases cover two independent service types, an unexposed signature
value dependency, an excluded signature dependency, alias collision, native
inheritance, construction/subclass/type-mutation rejection, factory identity,
same-Host sessions, foreign-Host cached methods, explicit destruction, retirement,
GC, Host close, cancellation, cleanup denial, restart, policy conflict/read-only
precedence and reconciliation reentry. The module compiled in both enabled and
disabled configurations; the disabled branch requires no CPython headers/symbols.

Earlier passing receipts remain applicable where relevant inputs did not change:
final edits after the broad regression run affected only wrapper guards/types,
opaque private header declarations, fixture metadata, and retirement notification
order. The changed bridge and editor/world lifetime paths were rerun; unchanged
CoreDObject/persistence, Actor business implementations and existing Toolset/MCP
job paths reuse their passing results. A final parser EOF whitespace cleanup does not change parsed/generated inputs.
The disabled-runtime return branch did not change. Final consumer compilation is covered by the restored ON all build.
Symbol searches cover Source/Tests in Engine, Sandbox and RoadWeaver; the inventory
is `stage1-consumers.json`. No GPU, application-hosted, Windows execution or full
Stage 2 invocation/value support is claimed.

## Stage 2 Implementation and Acceptance

The private generic bridge publishes native method/property descriptors, binds
positional and keyword inputs, applies only reflected defaults, and calls
ProcessEvent with an owned FFunctionParameterBuffer. Two unrelated reflected
services exercise the same bridge without handwritten Python business classes.
Inherited calls resolve the effective native override on every invocation;
unexposed overrides also block cached base descriptors. Unsupported signatures
are omitted with member/parameter diagnostics before becoming callable.

Codecs cover the selected scalars, enums, object/class references, Name/Guid,
struct/math values, fixed/dynamic arrays and maps. Native struct storage uses
native lifetime operations, collector-visible object roots and explicit purge.
Detached field edits publish a fully validated draft by swapping owned storage.
Read-only attributes, nested copy semantics, numeric ranges, conversion budgets,
return/out packing and native error cleanup are exercised by the bridge tests.
Intrinsic math registration now records explicit CoreDObject script provenance.
No public native signatures, generated ABI layout or build graph changed.

Validation used baseline `e42495f2594e87919d6b95f9716bef983bf556d8` plus the seven
native/generator inputs in `Build/PythonReflectionBridge/stage2-source-manifest.json`
(SHA256 `381a3c7f63481a1be673bd6d68f2d2be43ac912b451983e1aea528481c09dd0a`).
The host was Windows x64, preset `Win64-Debug-DurinEditor`, MSVC 14.44 and the
pinned CPython 3.13.15 release GIL ABI. Receipts are ignored local Build artifacts.

| Gate | Result / receipt |
| --- | --- |
| Real CPython generic bridge | 20 cases passed grouped in the final regression selection and all 20 passed independently with serial isolation; `stage2-regression.xml`, `stage2-isolated.xml` |
| Native reflection/property/persistence and existing Python/MCP compatibility | All 10 targets in `@domain=reflection+python` passed with four scheduling slots; `stage2-regression.xml` |
| Production DHT parser/generator | 434 passed, 1 skipped because Windows directory-symlink privilege is unavailable; `stage2-dht.xml`, `stage2-dht.log` |
| Owning modules and consumers | The selected tests built CoreDObject, DurinPython and their required editor/MCP consumers; no foundational header or native ABI change requires an all build |

The ordinary affected selection expands to all Engine tests because the test
fixture header belongs to that project. The bounded reflection/Python set covers
this stage's private bridge and intrinsic registration changes; Actor/property
editing and cross-project application qualification remain later-stage gates.
Initial sandbox runs hit MSVC C1902 and pytest temporary-directory access errors;
the final runs succeeded outside that sandbox, with DHT temporary data scoped to
Build/PythonReflectionBridge. Later documentation-only changes preserve the
recorded native/generator inputs. Python-disabled configuration, GPU/application
execution and other-platform qualification were not repeated for this stage.

The Stage 2 structural follow-up replaces the included implementation fragment
with five private translation units and a self-contained shared declaration
header. Type lookup now uses a borrowed reverse registry index, and property-name
validation/publication share one conversion helper. Value constructors delegate
field initialization to the member layer without depending on descriptor layout.
The ownership, admission, conversion budgets and detached-copy behavior remain
unchanged. This follow-up adds no public native ABI or module dependency changes.
Validation used baseline `0a7ae182e7f3e476a06590d29ec8c7e3fce215bb` and the final
inputs in `Build/PythonReflectionBridge/refactor-source-manifest.json` (SHA256
`f4036ece0903f0f6a1d64c387947be8654be8dcca1e28753eb8ce2e8e6a052b8`).
On the same Windows preset/SDK, all three targets selected by `test affected`
passed (`refactor-regression.xml`), and all 20 bridge cases passed serial process
isolation (`refactor-isolated.xml`). DHT/native reflection inputs and fixtures
are unchanged from the original Stage 2 receipt; only the affected Python
targets need rebuilding. Documentation and all-plan validation also passed.
The remaining Stage 3-5 gates, including disabled-runtime and other-platform
qualification, are unchanged.

Lasting implemented contracts are maintained in the Python adapter and script
guide. Stage 3 acceptance follows below.

## Stage 3 Implementation and Acceptance

The private property adapter resolves eligible reflected fields, converts a
complete detached candidate and composes one native FPropertyEditSession per
write. Helpers and direct writable attributes use the same implementation;
native object storage is never assigned directly by the Python layer. Only
current-Level Actors and owned Components pass native write policy. Proposal
normalization, native notifications, shared transactions, dirty/save checkpoints
and Undo/Redo retain their native behavior.

DurinEd rechecks live script authority immediately before publication, after
proposal hooks. Policy callbacks also revalidate target ownership after resolving
document access. Synchronous script writes reject deferred validation before work
starts. CancelUnchanged ends a rejected unchanged session without discarding an
enclosing recording; changed previews retain ordinary cancellation semantics.
Top-level fixed arrays remain excluded for the atomicity reason recorded in the
stage boundary. No Actor-specific Python binding or LevelEditor dependency was
added. Lasting contracts are in the Python adapter, script guide and reflected
property editing documentation.

Validation used baseline `bc4bdfe1a72d72d6ec6d98434b2c92d51fee1fcf` plus the
native/generator inputs in `Build/PythonReflectionBridge/stage3-source-manifest.json`
(SHA256 `b7a6205776874fd7243fc6c1f3ff7a7fad251c1ec40707fa228e9483d28e53b9`).
The host was Windows x64, preset `Win64-Debug-DurinEditor`, MSVC 14.44 and
CPython 3.13.15 release GIL ABI. Later documentation edits leave these inputs
unchanged. Raw receipts remain local ignored Build artifacts.

| Gate | Result / receipt |
| --- | --- |
| Real CPython property and reflection bridge | All 30 cases passed grouped in regression and independently with serial process isolation; `stage3-regression.xml`, `stage3-isolated.xml` |
| Native property editing, policy and Python/MCP compatibility | All eight targets selected by `@domain=property-editor+python+editor-operation+world` passed; `stage3-regression.xml` |
| Workspace consumers | RoadEditorIntegrationTests: 8 passed; SandboxGameplayTests: 13 passed; `stage3-road.xml`, `stage3-sandbox.xml` |
| Production DHT parser/generator | 434 passed, 1 skipped for unavailable Windows directory-symlink privilege; `stage3-dht.xml`, `stage3-dht.log` |

The affected selection expands to all Engine tests because of the shared fixture
header. The bounded selection covers DurinEd's changed property/policy paths,
the Python bridge and existing job compatibility; workspace symbol searches and
consumer targets cover the additive shared session API. No foundational layout
or build-graph change requires an all build. Python-disabled, other-platform,
GPU and application execution were not repeated; later-stage gates remain open.
Stage 4 acceptance follows below.

## Stage 4 Implementation and Acceptance

Selected simplification follow-up: replace custom native Actor/Component result
bundles with direct object pointers, arrays and bool. Failed creation returns
null, failed duplication an empty array, and other failed edits false. Successful
no-ops remain successful without a new history entry. Keep
detailed transaction disposition/recovery evidence in DTransactor. Remove the
script edit/history result structs and status/error enums; script edits return
Actor, Actor arrays or bool with the same empty/failure values; no per-operation
error strings are retained. Binding/admission exceptions remain at generic bridge
boundaries. Preserve the read-only Actor info query snapshot,
explicit targets, native admission and replay behavior. Migrate all native and
script consumers together; no legacy result facade is retained.

The original Stage 4 implementation added nine reflected ScriptName adapters:
current-Level discovery,
Actor enumeration/info and spawn/destroy/duplicate/rename/attach/detach. The six
edits forward once to existing subsystem implementations; vector-to-span
adaptation preserves native batch and result order. Reflected result/info structs
have read-only fields and preserve optional history diagnostics. Existing status,
error and attachment-rule enum values are unchanged. No Actor-specific Python
class, codec, Toolset or second editing implementation was added.

Native admission derives document authority from FScopedEditorScriptCall and
rechecks it after construction/PostLoad preparation. Real-CPython fixtures cover
both frozen script examples, rootless queries, explicit invalid/foreign Levels,
read-only/headless policies, PIE query/edit separation, batch order, hierarchy
constraints, native normalization/history, same-object Undo/Redo and retained
proxies across structural deletion. Native callbacks revoke policy, retire
candidates or close the Host without allowing prepared edits to publish.

Production use exposed two generic compatibility gaps. DHT now captures default
arguments from complete declaration token ranges, preserving external enum
expressions when Clang truncates an export parameter extent. A regression covers
export serialization with unresolved enumerators and comma-containing strings.
Automatic Python publication preflights the same dependency traversal used for
registration, defers unavailable module graphs with one diagnostic, and retries
at safe points. Explicit lookup still rejects unavailable providers. A lean-host
test verifies startup before Engine activation and publication afterward.

Validation used baseline `3bf7a99384a2b8abd62849b12411f34c4675763e` plus the final
inputs in `Build/PythonReflectionBridge/stage4-source-manifest.json` (SHA256
`0c3aa35fc73dc2342ef96a2d75302f79a8506efd3577637c0b43e26193d537d6`).
The host was Windows x64, preset `Win64-Debug-DurinEditor`, MSVC 14.44 and
CPython 3.13.15 release GIL ABI. Later documentation-only edits leave these
native/generator inputs unchanged. Receipts remain ignored local Build artifacts.

| Gate | Result / receipt |
| --- | --- |
| Production Actor and generic Python bridge | All 40 cases passed grouped and with serial process isolation; `stage4-regression.xml`, `stage4-isolated.xml` |
| Reflection, property editing, World/Actor and Python/MCP compatibility | All 15 targets selected by `@domain=reflection+python+world+editor-operation+property-editor` passed with four scheduling slots; includes all 48 PythonToolIntegrationTests cases; `stage4-regression.xml` |
| Lean-host module activation | Passed independently with serial isolation; `stage4-module-isolated.xml` |
| Workspace consumers | RoadEditorIntegrationTests: 8 passed; SandboxGameplayTests: 13 passed; `stage4-road.xml`, `stage4-sandbox.xml` |
| Complete production DHT suite | 435 passed, 1 skipped for unavailable Windows directory-symlink privilege; `stage4-dht.xml`, `stage4-dht.log` |

The affected selector expands to all tests for the shared fixture and unregistered
DHT runtime input. Complete DHT coverage plus the bounded reflection/editor set
covers the parser change and generated native dispatch. Workspace symbol searches
and consumer builds cover the additive service API and enum registration changes;
native layouts and existing signatures remain unchanged, so no all build is
required. Python-disabled, other-platform, GPU and application execution were not
repeated; Stage 5 acceptance gates remain open. Lasting API/admission contracts
are in the editor subsystem architecture, Python adapter and script guide.

### Stage 4 simplification acceptance

The follow-up removes FActorEditResult, FComponentEditResult,
FActorScriptEditResult, FActorScriptHistoryResult and both edit status/error enums.
Native Actor/Component creation returns a pointer, Actor duplication returns an
array, and the remaining edits return bool. Rejection returns null/empty/false;
successful no-ops return true. UI consumers provide contextual failure feedback.
No error-string output, replacement result wrapper or legacy facade was added.
The transactor still retains its typed execution/recovery evidence, and recovery
tests inspect that authoritative state directly. Query info snapshots are unchanged.

Validation used baseline `66dfd8af76355bacad8f07cde656458043e83022` plus the final
inputs in `Build/PythonReflectionBridge/simplify-source-manifest.json` (SHA256
`17793ffa443f64025281e4a6f04e021d6a27b76a17a894a95cf6d4a4251fe121`), on the
same Windows preset/compiler/CPython ABI as the original Stage 4 receipt.

| Gate | Result / receipt |
| --- | --- |
| Actor/Component structural edits and recovery | 51 cases passed; `simplify-native.xml` |
| Native World/Actor, editor operations and Python/MCP compatibility | All six targets selected by `@domain=world+python+editor-operation` passed with four scheduling slots; `simplify-regression.xml` |
| Real Python direct returns, rejected targets and unchanged history | All 41 bridge cases passed grouped and with serial isolation, including absence of the retired wrapper types and a no-op dirty/history check; `simplify-regression.xml`, `simplify-isolated.xml` |
| Additional shared-source consumers | ViewportTests: 101 passed; SplineTests: 49 passed; `simplify-viewport.xml`, `simplify-spline.xml` |
| Workspace consumers | RoadEditorIntegrationTests: 8 passed; SandboxGameplayTests: 13 passed; `simplify-road.xml`, `simplify-sandbox.xml` |

Workspace-wide source/test searches found no external-project calls to the changed
Actor service signatures. The selected tests compiled the owning module and all
changed LevelEditor UI consumers. The affected selection also includes unrelated
editor/rendering domains through shared modules; the bounded set covers these
return-value changes without introducing GPU/application execution. DHT inputs
and its unit fixtures are unchanged from the original Stage 4 suite receipt;
production reflection was regenerated and compiled for the new return signatures.
An existing unrelated PythonReflectionObjects.cpp worktree edit was excluded:
validation temporarily used its committed baseline and restored the original
bytes afterward. The source manifest lists only this task's inputs. Later edits
are documentation-only. Stage 5 gates remain open.

## Implementation Stages

### Stage 0: Freeze exposure, lifetime and Actor contracts

Outcome: reviewable API/type matrix with no unresolved admission policy.
Dependencies: baseline reflection, subsystem, property-edit and Python contracts.

- [x] Audit type/property/function metadata, DHT parser/generator, GC roots,
  native module lifetime and the existing job-to-Host execution binding.
- [x] Freeze exposure metadata, naming collisions, construction restrictions,
  type registration/removal policy, proxy identity/equality and error contracts.
- [x] Freeze conversion/output/default semantics and explicit unsupported kinds.
- [x] Specify the native script admission seam without a DurinEd-to-Python
  dependency; include Host gates, active-job checks and property-write policy.
- [x] Freeze reflected Actor signatures/result types and read-only/headless
  context resolution; add complete intended query/spawn/edit examples here.
- [x] Inventory affected symbols across all workspace projects and map concrete
  test targets using the test registry; record the bounded validation selection.

Gate: passed for contract selection. All six decisions are recorded above;
implementation must demonstrate the no-bypass/lifetime gates in later stages.

### Stage 1: Add generic Python types and object lifetime support

Outcome: independent reflected fixture types appear in durin with correct native
inheritance, object identity and invalidation. Depends on Stage 0.

- [x] Add interpreter-owned registry, native heap-type/proxy implementations and
  required symbols to the private pinned CPython ABI table.
- [x] Implement selected exposure metadata and type dependency registration.
- [x] Add runtime-class subsystem lookup using the existing native collection.
- [x] Add FScopedEditorScriptCall, native call-error reporting, Host target-policy
  registration and session option propagation; bind each synchronous entry.
- [x] Integrate active-job/Host/gate checks, wrapper GC edges and purge hooks,
  transient roots, weak Python caches, teardown and the selected module policy.
- [x] Verify same-Host reuse, cross-Host rejection, stale/collected/destroyed
  objects, cached bound methods, cancellation, close, Stop and interpreter restart.

Gate: generic fixtures work without Actor-specific binding logic or Python
dependencies leaking into public native headers/runtime modules.

### Stage 2: Implement reflected values and method dispatch

Outcome: an unrelated fixture class can execute reflected methods through
normal Python syntax. Depends on Stage 1.

- [x] Implement the selected scalar/enum/object/class/struct/container codecs
  using native property operations and owned value lifetimes.
- [x] Add method descriptors, argument binding, reflected defaults,
  ProcessEvent dispatch and return/out conversion.
- [x] Add read-only reflected property access and detached struct field access.
- [x] Verify inheritance/virtual dispatch, signed/unsigned bounds, math values,
  nulls, nontrivial storage cleanup, malformed kwargs, missing/native-only
  defaults, cycles, wrong classes/enums and unsupported signatures.
- [x] Prove invalid inputs never enter the native fixture method and all
  temporary references/storage are released after conversion/dispatch failure.

Gate: a second native reflected type needs only metadata, with no handwritten
Python class or per-method bridge implementation.

### Stage 3: Connect generic editor property writes

Outcome: eligible reflected properties can be edited with native validation,
notifications, dirty state and Undo/Redo. Depends on Stage 2.

Implementation boundary: reuse one native member edit session per synchronous
write, including an unchanged-cancellation path that preserves enclosing history.
Recheck script admission and target policy after native proposal hooks and before
publication; asynchronous proposal validation is unavailable in this entry point.
Top-level fixed-array writes remain unavailable because native sessions snapshot
one fixed-array element at a time, which cannot provide atomic whole-array
replacement. Fixed arrays nested inside a supported struct remain part of its
atomic snapshot; dynamic arrays and maps support whole-member replacement.

- [x] Adapt supported one-shot property edits through DurinEd's existing edit
  pipeline without constructing UI widgets or importing LevelEditor internals.
- [x] Enforce writable exposure, target ownership and authoritative editability.
- [x] Verify normalization/rejection, nested struct replacement, snapshot
  container semantics, notification order, shared scopes and Undo/Redo.
- [x] Cover read-only, foreign, closed and stale targets with no partial writes.

Gate: Python property writes preserve the same editor invariants as native edits.

### Stage 4: Onboard EditorActorSubsystem

Outcome: real scripts query and edit Actors via the subsystem proxy.
Depends on Stages 2-3 and the frozen native admission contract.

Compatibility boundary: automatic type publication defers graphs whose native
module dependencies are not active. Explicit lookup still rejects unavailable
owners; later safe-point publication adds the types after activation. This keeps
lean Toolset/MCP hosts usable without granting access to inactive Actor types.
Preserve external enum default expressions through DHT's hermetic export pass.

- [x] Add reflected query/edit signatures and result types, forwarding all six
  structural operations to existing subsystem implementations.
- [x] Support class and transform arguments without per-Actor Python codecs.
- [x] Verify explicit Level targeting, current editor Level/Host membership,
  read-only/headless policy, unsupported PIE state and hierarchy constraints.
- [x] Exercise query, spawn, destroy, duplicate, rename, attach, detach and
  generic transform property editing in real-CPython CPU fixtures.
- [x] Verify direct success/failure returns, successful no-ops, batch semantics, shared transaction
  Undo/Redo, retained proxies across delete/undo, and native lifecycle callbacks
  that retire objects or close the Host during an operation.

Gate: production Actor calls use the generic bridge, with no Actor Toolset
dependency, second editing implementation or Python-only lifecycle exceptions.

### Stage 5: Complete developer experience and integration acceptance

Outcome: documented, discoverable APIs with final evidence. Depends on Stage 4.

- [x] Provide help/dir/docstrings and deterministic IDE stubs from the same
  exposed registry, including Actor signatures; record output location and
  generation workflow. Generated declarations contain no business implementation.
- [x] Update Python architecture/usage and affected reflection/editor contracts,
  including copy semantics, errors, supported types and explicit limitations.
- [x] Run final bounded coverage for generic reflection, DHT changes, property
  editing, Actor editing and real CPython; run critical lifetime cases alone.
- [x] Verify existing Toolset/Python continuation/transaction and MCP Python-job
  compatibility, Python-disabled startup, and enabled-runtime restoration.
- [x] Compile affected owning/consumer targets across workspace projects; record
  tested revision, configuration, SDK, selections, receipts and remaining limits.
- [x] Validate changed documentation and all plan lifecycles; mark complete only
  after every required gate passes and lasting contracts are in owning docs.

Gate: all above pass on the available supported host. No other-platform execution
or GPU qualification is claimed. Cross-platform SDK/ABI loader coverage must be
recorded explicitly rather than inferred from this host's result.

### Stage 5 Completion Evidence

Validated baseline `ffb392e064fcd3353e48b60eb6092f52e1a4a215` plus the nine
implementation/package/test inputs in
`Build/PythonReflectionBridge/stage5-source-manifest.json` (SHA256
`5a6c047aef326884fcc2a671f38d1d99751c1b4e6b7a308ab4e73f0b354cd5f4`).
Host: Windows x64, `windows-msvc-x64`, `Win64-Debug-DurinEditor`, MSVC 14.44,
packaged CPython 3.13.15 release GIL ABI with the default SDK root. Later edits
are documentation-only and preserve these validated source inputs. Receipts below
are ignored artifacts under `Build/PythonReflectionBridge/`.

| Gate | Result / receipt |
| --- | --- |
| Published API discovery and declarations | `dir`, native docstrings, pydoc rendering, parsed/compiled declaration syntax, default/out packing, Actor direct returns, alias collisions and identical snapshots across sessions passed in the real interpreter |
| Final native/Python/MCP compatibility | All 17 targets in `@domain=reflection+python+property-editor+world+editor-operation+mcp` passed with four scheduling slots; `stage5-restored-regression.xml` |
| Lifetime and independent setup | All 42 reflection cases passed individually with serial isolation after runtime restoration; `stage5-restored-isolated.xml` |
| Production DHT suite | 435 passed, 1 skipped for unavailable Windows directory-symlink privilege; `stage5-dht.xml`, `stage5-dht.log`; a non-failing pytest cache-write warning did not affect fixture execution |
| Python-disabled editor Host | `DURIN_ENABLE_PYTHON=OFF` configured and compiled; the native CPU editor Host started with its Tool subsystem available while interpreter startup returned unavailable; `stage5-disabled.xml` |
| Enabled-runtime restoration | Explicitly configured `DURIN_ENABLE_PYTHON=ON`, rebuilt and passed the final 17-target set and all isolated reflection cases; ON remains the checkout's final configuration |
| Owning/editor consumers | The selected native graph built CoreDObject, Engine, DurinEd, DurinPython, LevelEditor and DurinMCP; explicit MainFrame and DurinLauncher builds passed; `stage5-builds.txt` records build logs and the DurinEditor executable path |
| Cross-project consumers | SandboxGameplayTests: 13 passed; RoadEditorIntegrationTests: 8 passed; `stage5-sandbox.xml`, `stage5-road.xml` |
| Documentation lifecycle | Changed-document validation and all-plan lifecycle validation passed |

The existing broad affected selection includes unrelated Engine domains through
shared fixture/module ownership. The bounded set covers private bridge discovery,
the generic Python facade, native Actor/property behavior and transport consumers;
the disabled-only test adds CPU Host validation without changing production ABI
or foundational headers. No workspace-wide all build is required. This stage has
no GPU/application-hosted execution or timing qualification. Windows dynamic SDK
loading and the interpreter-unavailable path were exercised; macOS/other-platform
SDK and ABI loader behavior were not executed or inferred from these results.

Lasting discovery, declaration generation, copying, error, type and limitation
contracts are owned by [Python architecture](../../../Editor/Architecture/Tools/PythonTools.md)
and the [script guide](../../../Editor/Guides/PythonScripts.md). The generation workflow
writes `Build/PythonStubs/durin/__init__.pyi`; no generated business bindings or
versioned declaration snapshots are introduced.

## Validation Policy

Follow [build selection](../../../Agents/BuildAndRun.md),
[test selection and result reuse](../../../Agents/Testing.md), and
[documentation validation](../../../Agents/Documentation.md). This planning change
requires documentation validation only.

Implementation starts with registry-selected CPU fixtures and the existing real
CPython test infrastructure. Add focused bridge fixtures without making all
tests depend on Python. Extend owning native/DHT tests for changed metadata and
generated bindings. Existing PythonToolIntegrationTests, MCPPythonJobTests and
PythonRuntimeUnavailableTests are compatibility candidates; use the Stage 0
selection above and revalidate it when implementation inputs change. Use existing
Actor/property targets for domain behavior.

A workspace-wide all build is required only if final foundational-header,
generated ABI or build-graph impact prevents reliable bounded consumer coverage;
record that risk before expansion. No all-test, GPU, application-hosted or new
platform gate is introduced by default. Store raw receipts under ignored Build/.

## Required References

- [Native function reflection](../../../Runtime/ObjectSystem/FunctionReflection.md)
- [Property reflection](../../../Runtime/ObjectSystem/PropertyReflection.md)
- [Reflection generation](../../../Development/Tooling/ReflectionGeneration.md)
- [Python runtime and jobs](../../../Editor/Architecture/Tools/PythonTools.md)
- [Python script guide](../../../Editor/Guides/PythonScripts.md)
- [Editor subsystem and Actor contracts](../../../Editor/Architecture/Framework/EditorSubsystems.md)
- [Reflected property editing](../../../Editor/Architecture/PropertyEditing/ReflectedPropertyEditing.md)
- [Editor transactor](../../../Editor/Architecture/Transactions/Transactors.md)
- [UE runtime Python API generation](https://dev.epicgames.com/documentation/en-us/unreal-engine/scripting-the-unreal-editor-using-python#abouttheunrealeditorpythonapi)
- [UE subsystem access](https://dev.epicgames.com/documentation/en-us/unreal-engine/programming-subsystems-in-unreal-engine)

### Pinned UE implementation sources

- [UE 5.3.2 PyGenUtil.cpp](https://github.com/chenyong2github/UnrealEngine/blob/c865e168d0935b8e5f4bd865ddcc1c733c8ce7cf/Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PyGenUtil.cpp)
- [UE 5.3.2 PyWrapperTypeRegistry.cpp](https://github.com/chenyong2github/UnrealEngine/blob/c865e168d0935b8e5f4bd865ddcc1c733c8ce7cf/Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PyWrapperTypeRegistry.cpp)
- [UE 5.3.2 PyWrapperObject.cpp](https://github.com/chenyong2github/UnrealEngine/blob/c865e168d0935b8e5f4bd865ddcc1c733c8ce7cf/Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PyWrapperObject.cpp)
- [UE 5.3.2 PyReferenceCollector.cpp](https://github.com/chenyong2github/UnrealEngine/blob/c865e168d0935b8e5f4bd865ddcc1c733c8ce7cf/Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PyReferenceCollector.cpp)
- [UE 5.3.2 PyUtil.cpp](https://github.com/chenyong2github/UnrealEngine/blob/c865e168d0935b8e5f4bd865ddcc1c733c8ce7cf/Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PyUtil.cpp)
