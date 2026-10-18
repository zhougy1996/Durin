# Python Reflection Bridge Plan

Summary: Add a generic runtime Python binding over native reflection, using EditorActorSubsystem as the first production consumer.

Last reviewed: 2026-10-18

Status: Active
Completed:

## Current Status

Stages 0-1 completed on 2026-10-18. Stage 1 implementation and validation use
baseline `a7a7d0548` plus the source manifest recorded below. Generic native
classes/structs/enums are published as interpreter-owned heap types, and exposed
editor subsystem objects support identity, inheritance and guarded bootstrap
introspection. Stage 1 has no Actor-specific binding code. Stages 2-5 remain
unimplemented; reflected function dispatch and value/property conversion are next.

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
`Script`, ScriptCallable and explicit ScriptName values below. Existing native
methods/signatures remain unchanged. The new members take DLevel*/AActor* and
const vector references instead of non-reflectable references/spans/callbacks;
forward to the existing service. Query methods are const. All six edit methods
return FActorScriptEditResult. Transform is required on spawn because its native
construction expression is not a reflected default; name uses std::string = "".
Attach/detach rules get reflected DENUM registration and enum defaults.

| Python method | Reflected native parameters / return |
| --- | --- |
| `get_editor_level()` | `() -> DLevel*`; None only when no editor Level exists |
| `get_all_level_actors(level)` | `(DLevel*) -> vector<TObjectPtr<AActor>>`; native Level order, live published actors only |
| `get_actor_info(level, actor)` | `(DLevel*, AActor*) -> FActorScriptInfo` |
| `spawn_actor(level, actor_class, transform, name="")` | `(DLevel*, DClass*, const FTransform&, const string&) -> FActorScriptEditResult` |
| `destroy_actors(level, actors)` | `(DLevel*, const vector<TObjectPtr<AActor>>&) -> FActorScriptEditResult` |
| `duplicate_actors(level, actors)` | Same parameter/result types as destroy |
| `rename_actor(level, actor, name)` | `(DLevel*, AActor*, const string&) -> FActorScriptEditResult` |
| `attach_actors(level, actors, parent, rule=AttachmentTransformRule.KEEP_WORLD)` | `(DLevel*, const vector<TObjectPtr<AActor>>&, AActor*, EAttachmentTransformRule) -> FActorScriptEditResult` |
| `detach_actors(level, actors, rule=DetachmentTransformRule.KEEP_WORLD)` | `(DLevel*, const vector<TObjectPtr<AActor>>&, EDetachmentTransformRule) -> FActorScriptEditResult` |

No mutation infers its Level or participants from selection/focus. Match existing
native naming/conflict, batching, no-op and result order rules; do not add bridge
batch loops or promises of rollback beyond native guarantees. Spawn leaves its
native Configure callback empty. Asset-specific placement, component structural
methods, UI selection/focus and specialized Transform setters remain deferred.

Promote EActorEditStatus/EActorEditError to reflected enums without changing native
values; alias them as ActorEditStatus/ActorEditError. Add ScriptType result/info
structs with ScriptReadOnly fields. Do not expose std::optional or raw native
history objects. FActorScriptEditResult fields are:

- `status: ActorEditStatus`, `error: ActorEditError`, `message: str`;
- `actors: list[Actor]`, preserving the native result, including Failed results;
- `history: ActorScriptHistoryResult`, containing `available: bool`,
  `code: str` (native transactor result code name), and `message: str` (formatted
  native history diagnostic). Absent history has false/empty/empty fields.

FActorScriptInfo contains `actor: Actor`, `name: str`, `actor_class: type[Actor]`,
`parent: Actor | None`, `children: list[Actor]`, `root_component: SceneComponent |
None` and `world_transform: Transform`. Capture direct child Actors in Level order;
use existing native transform behavior for rootless Actors. These snapshots do
not grant structural property writes. RelativeTransform on the root Component is
already Edit and becomes the initial generic transform-edit target.

Native wrappers convert results once; the generic Python bridge sees ordinary
reflected fields, with no Actor-specific codecs or implicit bool conversion.
Pre-dispatch invalid/stale object arguments raise binding exceptions; live but
invalid domain targets return domain failure for edits and a native script-call
error for queries. Query rejection never masquerades as an empty list.

### Intended script acceptance examples

These scripts run after Stage 4 in the existing real-CPython CPU editor fixture.
The fixture owns a current Level with at least one constructible rooted Actor,
and explicitly selects NonDocumentReadWrite. The same calls in an interactive
editor require a live writable Level document. They are not runnable today.

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
    created = actors.spawn_actor(
        level, seed.actor_class, seed.world_transform, name="PreviewActor"
    )
    if created.status != durin.ActorEditStatus.CHANGED:
        raise RuntimeError(created.message)
    preview = created.actors[0]
    renamed = actors.rename_actor(level, preview, "PreviewActorRenamed")
    if renamed.status == durin.ActorEditStatus.FAILED:
        raise RuntimeError(renamed.message)

    root = actors.get_actor_info(level, preview).root_component
    relative = root.get_editor_property("relative_transform")
    translation = relative.translation
    translation.x += 100.0
    relative.translation = translation
    root.set_editor_property("relative_transform", relative)
```

A second fixture script uses the same typed results for the remaining operations:

```python
with durin.ScopedEditorTransaction("Duplicate and attach preview"):
    copied = actors.duplicate_actors(level, [preview])
    if copied.status != durin.ActorEditStatus.CHANGED:
        raise RuntimeError(copied.message)
    duplicate = copied.actors[0]
    def require_success(result):
        if result.status == durin.ActorEditStatus.FAILED:
            raise RuntimeError(result.message)

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
| Native consumers, Stage 5 | Owning CoreDObject/Engine/DurinEd/DurinPython plus LevelEditor/MainFrame/DurinMCP and DurinEditor; compile SandboxGameplayTests and RoadEditorIntegrationTests for cross-project shared-header consumption |

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

- [ ] Implement the selected scalar/enum/object/class/struct/container codecs
  using native property operations and owned value lifetimes.
- [ ] Add method descriptors, argument binding, reflected defaults,
  ProcessEvent dispatch and return/out conversion.
- [ ] Add read-only reflected property access and detached struct field access.
- [ ] Verify inheritance/virtual dispatch, signed/unsigned bounds, math values,
  nulls, nontrivial storage cleanup, malformed kwargs, missing/native-only
  defaults, cycles, wrong classes/enums and unsupported signatures.
- [ ] Prove invalid inputs never enter the native fixture method and all
  temporary references/storage are released after conversion/dispatch failure.

Gate: a second native reflected type needs only metadata, with no handwritten
Python class or per-method bridge implementation.

### Stage 3: Connect generic editor property writes

Outcome: eligible reflected properties can be edited with native validation,
notifications, dirty state and Undo/Redo. Depends on Stage 2.

- [ ] Adapt supported one-shot property edits through DurinEd's existing edit
  pipeline without constructing UI widgets or importing LevelEditor internals.
- [ ] Enforce writable exposure, target ownership and authoritative editability.
- [ ] Verify normalization/rejection, nested struct replacement, snapshot
  container semantics, notification order, shared scopes and Undo/Redo.
- [ ] Cover read-only, foreign, closed and stale targets with no partial writes.

Gate: Python property writes preserve the same editor invariants as native edits.

### Stage 4: Onboard EditorActorSubsystem

Outcome: real scripts query and edit Actors via the subsystem proxy.
Depends on Stages 2-3 and the frozen native admission contract.

- [ ] Add reflected query/edit signatures and result types, forwarding all six
  structural operations to existing subsystem implementations.
- [ ] Support class and transform arguments without per-Actor Python codecs.
- [ ] Verify explicit Level targeting, current editor Level/Host membership,
  read-only/headless policy, unsupported PIE state and hierarchy constraints.
- [ ] Exercise query, spawn, destroy, duplicate, rename, attach, detach and
  generic transform property editing in real-CPython CPU fixtures.
- [ ] Verify Changed/NoOp/Failed results, batch semantics, shared transaction
  Undo/Redo, retained proxies across delete/undo, and native lifecycle callbacks
  that retire objects or close the Host during an operation.

Gate: production Actor calls use the generic bridge, with no Actor Toolset
dependency, second editing implementation or Python-only lifecycle exceptions.

### Stage 5: Complete developer experience and integration acceptance

Outcome: documented, discoverable APIs with final evidence. Depends on Stage 4.

- [ ] Provide help/dir/docstrings and deterministic IDE stubs from the same
  exposed registry, including Actor signatures; record output location and
  generation workflow. Generated declarations contain no business implementation.
- [ ] Update Python architecture/usage and affected reflection/editor contracts,
  including copy semantics, errors, supported types and explicit limitations.
- [ ] Run final bounded coverage for generic reflection, DHT changes, property
  editing, Actor editing and real CPython; run critical lifetime cases alone.
- [ ] Verify existing Toolset/Python continuation/transaction and MCP Python-job
  compatibility, Python-disabled startup, and enabled-runtime restoration.
- [ ] Compile affected owning/consumer targets across workspace projects; record
  tested revision, configuration, SDK, selections, receipts and remaining limits.
- [ ] Validate changed documentation and all plan lifecycles; mark complete only
  after every required gate passes and lasting contracts are in owning docs.

Gate: all above pass on the available supported host. No other-platform execution
or GPU qualification is claimed. Cross-platform SDK/ABI loader coverage must be
recorded explicitly rather than inferred from this host's result.

## Validation Policy

Follow [build selection](../Agents/BuildAndRun.md),
[test selection and result reuse](../Agents/Testing.md), and
[documentation validation](../Agents/Documentation.md). This planning change
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

- [Native function reflection](../Runtime/Core/FunctionReflection.md)
- [Property reflection](../Runtime/Core/PropertyReflection.md)
- [Reflection generation](../Development/Tooling/ReflectionGeneration.md)
- [Python runtime and jobs](../Editor/Architecture/PythonTools.md)
- [Python script guide](../Editor/Guides/PythonScripts.md)
- [Editor subsystem and Actor contracts](../Editor/Architecture/EditorSubsystems.md)
- [Reflected property editing](../Editor/Architecture/ReflectedPropertyEditing.md)
- [Editor transactor](../Editor/Architecture/Transactors.md)
- [UE runtime Python API generation](https://dev.epicgames.com/documentation/en-us/unreal-engine/scripting-the-unreal-editor-using-python#abouttheunrealeditorpythonapi)
- [UE subsystem access](https://dev.epicgames.com/documentation/en-us/unreal-engine/programming-subsystems-in-unreal-engine)

### Pinned UE implementation sources

- [UE 5.3.2 PyGenUtil.cpp](https://github.com/chenyong2github/UnrealEngine/blob/c865e168d0935b8e5f4bd865ddcc1c733c8ce7cf/Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PyGenUtil.cpp)
- [UE 5.3.2 PyWrapperTypeRegistry.cpp](https://github.com/chenyong2github/UnrealEngine/blob/c865e168d0935b8e5f4bd865ddcc1c733c8ce7cf/Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PyWrapperTypeRegistry.cpp)
- [UE 5.3.2 PyWrapperObject.cpp](https://github.com/chenyong2github/UnrealEngine/blob/c865e168d0935b8e5f4bd865ddcc1c733c8ce7cf/Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PyWrapperObject.cpp)
- [UE 5.3.2 PyReferenceCollector.cpp](https://github.com/chenyong2github/UnrealEngine/blob/c865e168d0935b8e5f4bd865ddcc1c733c8ce7cf/Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PyReferenceCollector.cpp)
- [UE 5.3.2 PyUtil.cpp](https://github.com/chenyong2github/UnrealEngine/blob/c865e168d0935b8e5f4bd865ddcc1c733c8ce7cf/Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PyUtil.cpp)
