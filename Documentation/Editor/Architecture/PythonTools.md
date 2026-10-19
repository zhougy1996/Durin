# Python Toolset Adapter

Summary: Embed trusted editor scripts with native Toolsets, reflected type/object wrappers and transaction scopes.
Modules: DurinPython, DurinEd, MainFrame, Core

Last reviewed: 2026-10-19

## Ownership and Runtime

DurinPython is optional. Core and business providers have no Python dependency;
CPython objects and ABI symbols remain private. `PythonRuntime.h` exposes owned
session/job data. One process-wide owner initializes, executes and finalizes
CPython on GameThread. Start, Stop and destruction are owner operations; native
submission, query and cancellation remain detached and thread-safe.

The default SDK is CPython 3.13.15, Windows x64, release GIL ABI, pinned by the
`python` dependency manifest. `DURIN_PYTHON_SDK_ROOT` explicitly selects the same
version/ABI for native-host verification on Windows or macOS. Its layout is
`include/`, `Lib/`, `LICENSE.txt`, and `python313.dll` or
`libpython3.13.dylib`. On macOS an extracted framework binary needs a valid local
code signature when used outside its framework. The SDK override does not
prepare or install a system interpreter. Without an SDK, or with
`DURIN_ENABLE_PYTHON=OFF`, the module returns runtime-unavailable errors.

Builds deploy the library, license, `python313.zip` and `durin-python.zip` through
the normal runtime closure. Zips contain Python sources, stored without compression
so startup needs no native zlib extension. Python-only edits redeploy the package.
The private `_durin` built-in supplies native bindings and exceptions; the ordinary
`DurinPython/Python/durin` package supplies the public facade. The ABI table loads
symbols explicitly and keeps the library mapped for process lifetime.

Isolated PyConfig disables site, environment paths, bytecode writes and signal
handlers. Only the two deployed zips enter sys.path. Native extension packages,
Python-backed providers, background Python threads, and native callbacks into
Python are unsupported. Trusted Python is ordinary process code, not a sandbox.

## Reflection Implementation Boundaries

The native reflection bridge lives in DurinPython's private implementation:

| Source | Responsibility |
| --- | --- |
| `PythonReflection.cpp` | Interpreter registry, exposure/naming rules, type publication and lifecycle entry points |
| `PythonReflectionObjects.cpp` | Native object identity, factory observations, admission and object-type slots |
| `PythonReflectionValues.cpp` | Owned native values, reference roots, retirement and value construction/type slots |
| `PythonReflectionConversion.cpp` | Supported schemas, conversion budgets and Python/native codecs |
| `PythonReflectionMembers.cpp` | Member descriptors, argument binding, native dispatch and reflected field access |
| `PythonReflectionProperties.cpp` | Editor-property lookup, write admission and native edit-session composition |
| `PythonReflectionDocumentation.cpp` | Shared signature/type formatting, member documentation and deterministic IDE declarations |

`PythonReflectionInternal.h` declares only the state and operations shared by
these translation units. The registry owns Python type references; its reverse
index borrows those identities for Python-to-native type lookup. Descriptor
callbacks and other file-local helpers remain private to their source file.
The opaque runtime-facing `PythonReflection.h` contains no CPython definitions.
Schema qualification, runtime value accounting and native root collection retain
their distinct responsibilities; member dispatch delegates conversion and value
ownership to the corresponding implementation instead of duplicating them.

## Reflected Types and Object Wrappers

DurinPython also owns an interpreter-local native type registry. ScriptType
classes/structs/enums and classes with script-exposed members are published as
CPython heap types. Ancestors and exposed signature dependencies receive wrappers
without granting unmarked methods or native object constructors. Types retain
native inheritance; get_type(qualified_name) resolves canonical identities.
For classes, `ScriptType` is effective through the native superclass chain;
derived classes need not repeat it. `ScriptNoExport` on a class or any ancestor
excludes the entire branch, even when a descendant declares `ScriptType` again.
Automatic publication, qualified lookup, signature filtering and dynamic object
wrapping honor that exclusion. This is interpreter publication policy; generated
metadata continues to record only each declaration's own flags. Structs/enums
retain explicit type exposure. Class visibility does not expose unmarked members
or grant native construction/edit authority.

The Actor and ActorComponent bases declare `ScriptType`, so active providers'
derived types, including CameraActor and CameraComponent, are discoverable before
any instances are queried or created. Scripts can pass `durin.Actor` or
`durin.CameraActor` directly to `spawn_actor` in an empty writable Level. A plain
Actor has no default root component; CameraActor supplies its camera root.
Top-level aliases omit the native type prefix. Colliding aliases are omitted with
a diagnostic, leaving qualified lookup available. Generated Python declarations
are not executable binding implementations.

`durin.get_editor_subsystem(type)` returns the selected Host's existing service,
wrapped with native identity and a non-owning Python factory observation. Object
wrappers expose get_name/get_class as bootstrap introspection helpers. Classes
are immutable and native object classes cannot be instantiated or subclassed in
Python. `ScriptCallable` methods use ordinary bound or unbound Python calls,
including positional/keyword arguments and reflected defaults. Dispatch uses
`FFunctionParameterBuffer` and `DObject::ProcessEvent`; an unmarked/excluded
effective override blocks inherited descriptors and previously bound methods.
Unsupported signatures are omitted with a qualified member/parameter diagnostic.

The generic bridge converts checked bool/integer/finite-float values, strings,
Name/Guid values, exact reflected enums and structs, compatible object proxies,
native classes, list/tuple arrays and dict maps. `durin.Name(str)` and
`durin.Guid(str)` provide immutable, hashable values; names preserve native
case-insensitive identity. Intrinsic math structs expose their reflected fields
and native default/named-field construction. Mutable ScriptType structs likewise
allow writable named fields; read-only result structs and signature-only
dependencies cannot be constructed. Reflected defaults apply only to omitted
arguments; native-only defaults still require an explicit argument.

Direct reflected attributes are readable only when script-exposed. Detached
struct writable fields can be assigned. Structs and containers are copies, including nested
field reads: modify a nested value and explicitly assign it back. Object elements
retain proxy identity and native GC edges; owned struct storage also roots its
references and is purged on target/Host retirement. Detached field edits validate
a complete draft before publishing it.

Return packing is None for no output, the value for one output, or a tuple in
return-first/out-parameter order. With a bool return plus out parameters, false
returns None and true packs only the out values; a lone bool remains bool.
Mutable-reference inputs are required and their updated values appear in the
outputs. Conversion is bounded to four container levels, 65,536 value nodes,
16 MiB of converted payload and 64 struct/value levels per call. Cycles, numeric
overflow, invalid values, wrong types and converted map-key collisions fail
before native effects. Output conversion failure does not roll back a completed
native call. Native exceptions become RuntimeError after parameter cleanup;
native script admission errors become ScriptAccessError.

A live wrapper holds a collector-visible native strong reference. Host/target
object keys and endpoint gate identity qualify factory identity. Imported state
may retain a wrapper across jobs or sessions on the same live Host; each native
access rechecks active-job admission, cancellation, module and service lifetime.
Cross-Host access fails even through a cached bound method. Hash/equality/repr do
not dereference native memory. Native destruction, subsystem retirement and Host
closure purge wrapper roots before physical collection; old wrappers cannot
revive when a replacement service has the same native class. Stop purges roots
and discards all interpreter-owned type/factory state before CPython finalization.
Native types come from non-unloadable active modules or the process-lifetime
CoreDObject foundation/bootstrap. Runtime native type unloading is unsupported.
Automatic publication defers an entire type dependency graph when a provider is
inactive, unresolved or unloadable, with a diagnostic once per deferred type.
Explicit lookup still raises ScriptAccessError. Later script-pump safe points
publish newly available graphs after module activation; lean native Toolset/MCP
hosts can therefore start Python without activating Actor providers.

DurinEd's FScopedEditorScriptCall supplies synchronous Host/endpoint, source,
cancellation and edit-policy context without a Python dependency. Nested scopes
restore their predecessor and keep separate errors. A Host target-policy token
reports live document editability. LevelEditor registers one alongside its Level
context; read-only/unavailable or conflicting providers reject writes. Native
FPythonSessionOptions defaults to DocumentManaged; headless callers must explicitly
select NonDocumentReadOnly or NonDocumentReadWrite. Python/remote arguments cannot
change this policy.

Object wrappers expose `get_editor_property(name)` for supported `Edit` fields
unless `ScriptNoExport` excludes them. Names use the normal reflected snake-case
mapping. `set_editor_property(name, value)` requires `Edit`, rejects `ReadOnly`,
`NonTransactional` and `ScriptNoExport`, and accepts only live Actors or owned
Components in the selected Host's current editor Level. Direct attribute writes
additionally require `ScriptReadWrite`; both entry points share the same writer.
Read access does not imply write admission. Assets and subsystem fields cannot
be written through this path.

Each write converts a complete detached candidate before opening one native
`FPropertyEditSession`. Native proposal hooks normalize or reject it; script
admission and authoritative editability are checked again before publication.
The session emits native notifications, updates dirty state and commits through
the Host transactor. `ScopedEditorTransaction` groups multiple writes. Rejection
of an unchanged candidate ends only its own scope, preserving earlier edits and
their enclosing history. Conversion failures never publish a partial value.
Deferred validation is rejected before starting work because this API completes
synchronously. Root fixed-array replacement is unavailable; fixed arrays inside
a supported struct, dynamic arrays and maps are replaced as complete snapshots.
Native validation failures raise `ValueError`, admission failures raise
`ScriptAccessError`, and retired targets raise `ReferenceError`.

The native editing contract belongs to
[reflected property editing](ReflectedPropertyEditing.md).

`durin.get_editor_subsystem(durin.EditorActorSubsystem)` exposes reflected
queries and six structural Actor edits through the same generic descriptors,
class/transform codecs and native ProcessEvent dispatch. DurinEd owns the
language-neutral adapters; DurinPython contains no Actor-specific codec or edit
implementation. Explicit Levels and participants replace UI selection as inputs.
Queries return typed detached snapshots in Level order. Creation returns Actor or
None, duplication returns an Actor list, and other edits return bool. Rejected
domain edits use None/empty/false; successful no-ops return true without adding
history. Transaction recovery diagnostics stay native, and binding/type/lifetime
errors continue to raise exceptions.
The full API and admission rules belong to
[editor subsystems](EditorSubsystems.md#script-actor-adapter).

## Discovery and IDE Declarations

Standard `dir(durin)`, `dir(type)` and `dir(instance)` expose published aliases
and inherited members. Native type docstrings identify the canonical qualified
name; method/property descriptor docstrings describe Python signatures and
read/write status. Method documentation includes native tooltips when present.
`help()` consumes these same docstrings. Defaults shown as `...` denote available
reflected defaults, rather than their C++ expression; native-only defaults remain
required arguments. An unexposed effective override still rejects an inherited
descriptor at invocation, even if ordinary Python inheritance lists its name.

`durin.generate_stubs()` returns declaration text for the current interpreter's
published native registry. It reads the actual type dictionaries, including enum
constants, rather than maintaining another exposure list. Types and members are
sorted by canonical names and script names, so repeated generation with the same
published schema produces identical bytes across jobs/sessions. Loading more
modules can change the snapshot. Generation does not invoke native business
methods or load additional providers.

Save this text to `Build/PythonStubs/durin/__init__.pyi`; add `Build/PythonStubs`
to the IDE's Python analysis paths. Regenerate after changing reflection metadata
or loading native modules. Generated files stay under ignored `Build/` and contain
declarations only. Actor methods use the ordinary native descriptors and retain
their pointer/list/bool returns. Read-only fields use getter declarations;
writable fields include setters. Container inputs permit the accepted list/tuple
shapes while outputs describe detached lists/dicts. Object annotations permit
None; liveness, numeric ranges, exact container/enum identity, fixed-array lengths,
admission and conversion budgets remain runtime checks.

Ambiguous short aliases have private deterministic declaration names and typed
`get_type` overloads for canonical qualified lookup. These private names are
annotation identities, not extra runtime exports. Stubs describe published
reflection and a small generic facade; dynamic Toolset schemas remain `Any`.
They do not grant constructors, native subclassing, hidden members or edit
authority. They are completion/type hints rather than a replacement runtime.

## Sessions and Invocation

Acquire `DEditorToolSubsystem::GetService()` on GameThread from the selected
editor. CreateSession retains that detached endpoint, EToolSource and client
identity/cancellation token. Scripts cannot replace them. External sessions see
only externally exposed tools; trusted native hosts may select Native.

RunString/RunFile admit owned source into a bounded queue. File paths resolve
against FPaths::ProjectDir, and contents are read before admission. Each job gets
fresh globals, `__name__` and `__file__`; imports share interpreter state. Tool
facades and invocation handles reject use by another job or host.

Editor Tick drains tools, then calls `TickScripts`. CPU/native hosts use the same
explicit boundary. `IEditorScriptPump` version 2 executes outside provider dispatch.
Plain script bodies run synchronously. `durin.defer(callback_or_generator)` appends
ordered continuations to that same job; a callback runs once, and a generator
holds its position until completion. A callback returning a generator uses one
step to create it, then starts it on a later tick. Dynamic registrations append
without recursive entry. Bare yield resumes on a later pump boundary; yielding a
job-owned invocation waits for validated native completion and sends converted Data
or throws ToolError into the generator. Other yielded values/foreign handles fail.

Each ready job runs at most one step per pump round. Ready jobs rotate through the
existing queue, with at most 16 steps and a soft 2 ms budget checked between steps.
A single callback/native handler still runs until return and can block the UI.
The interpreter remains on GameThread. There is no worker interpreter, asyncio,
automatic bytecode slicing or nested editor tick. Native completion never reenters
Python inline. Every entry rebinds job tools/globals, host, stdout/stderr, tracing
and cancellation; suspended references remain owned until owner-thread cleanup.

`durin.tools.list/describe` expose native names, versions and schemas. Toolset and
local names remain separate. Schema `type` is the EToolValueType integer.
`tools.call` uses CallNow and rejects may-pend descriptors before provider callbacks.
`tools.submit` uses SubmitNow to admit and dispatch exactly one invocation without
pumping a queue or tick. Ready handlers still return completed handles immediately;
pending handlers leave the handle running. Both paths share source, version,
availability, schema, value and result-budget validation. No provider is retried.

Handles retain the native invocation identity and its authoritative validated
Future; Python keeps no FToolResult cache. `result()` returns converted data/raises
ToolError when ready, otherwise raises `durin.NotReady` without waiting. `cancel()`
signals that invocation only. Dropping observation is separate from cancellation.
Native retention accounting survives receipt TTL eviction. A job remains Running
until its continuations and owned invocations settle or cancellation safely detaches
them; submission without reading a handle is not implicit fire-and-forget.
The old per-call/result timeout argument is removed. Python conversion preserves
signed/unsigned 64-bit values and rejects cycles, non-finite and unsupported values.

## Transactions

`durin.ScopedEditorTransaction(desc)` follows UE 5.3.2's
[Python scope implementation](https://github.com/chenyong2github/UnrealEngine/blob/c865e168d0935b8e5f4bd865ddcc1c733c8ce7cf/Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PyEditor.cpp).
Construction stores the description; enter begins native recording and returns
self. Exit ends recording and returns None, including when an exception leaves
the block. Exceptions propagate. Cancel discards the entire active recording,
including when called on an inner scope, and returns None. Applied edits remain.

The scope uses the admitting editor's transactor, without GEditor lookup or a
second history stack. Ordinary native domain scopes join it. A script can read a
result, make subsequent edits and group them into one Undo/Redo entry. Each
participant still calls Modify or records its native command change. Exit after
cancel is harmless; a completed scope can be entered again. Unfinished job-owned
scopes close in reverse entry order before the terminal snapshot is published;
retaining a Python scope in imported state cannot keep recording across jobs.
A scope may span yield and keeps the editor's global recording open: UI/native
edits and other jobs on that host join it. Undo/Redo follows the native recording
state. Sessions share the active editor's history; suspension
releases interpreter execution, not the recording. Scope validity uses the native transaction
ID and GC-visible transactor ownership so stale helpers cannot alter a replacement.
Prefer waiting/preparation outside recording, then revalidate and apply in a short
synchronous scope. No isolation, automatic splitting or rollback is supplied.
See [native recording](Transactors.md#transactions-and-scoped-recording).

## Results, Limits and Closure

Successful data becomes None/bool/int/float/str/list/dict. ToolError includes
numeric diagnostics codes, RFC 6901 paths and messages. Script exceptions retain
filenames and bounded tracebacks. stdout/stderr share a bounded buffer; overflow
fails the job even when caught by Python.

| Resource | Limit |
| --- | --- |
| Editor sessions | 8 |
| Outstanding jobs | 16 |
| Retained jobs | 64; oldest terminal jobs evicted first |
| Source / filename | 1 MiB / 4096 bytes, no NUL; UTF-8 source |
| Output / exception text | 64 KiB each |
| Owned native submissions per job | 256 |
| Live continuations per job | 256 |
| Steps per pump / soft between-step budget | 16 / 2 ms |
| Job deadline | Optional positive FPythonRuntimeOptions::JobDeadline; default absent |

Snapshots are Queued, Running, Succeeded, Failed or Cancelled. Unknown/evicted IDs
fail explicitly. Terminal publication follows native return, Python reference and
scope cleanup. Completed jobs release their endpoint. Closing a session signals
jobs; queued jobs have no interpreter references and can cancel immediately.
Running/suspended jobs stop normal resumes and signal owned native work, then
close generators/finally and End still-valid scopes on GameThread before terminal
publication. Cleanup permits output and scope closure, rejects new work, and reports
generator/transaction cleanup failures. Job-owned cyclic references are collected
under this cleanup context, so finalizers cannot submit new editor work. Cancellation and explicit transaction Cancel
remain different: neither promises rollback of applied edits.

The optional deadline measures total elapsed time from job admission, including
queueing and suspension. The pump and Python line/opcode tracing check that same
deadline; native code cannot be preempted. There is no mandatory runtime deadline
or inert per-call timeout. Existing transport deadlines remain adapter policy.
Host deinitialization calls the existing pump's CloseEndpoint boundary to close
suspended references/scopes before releasing editor state. Late native completion
cannot revive a cancelled job or authorize DObject access through a closed host.

Stop cancels admission/work on GameThread. During a native callback it defers
finalization until the active step returns. Suspended generators and job references
are cleaned before Py_FinalizeEx. Finalization/cleanup failures are reported; no
restored effects or safe producer unload are inferred. An
outer safe-point Stop retires its registration; repeated Stop is harmless. The
process interpreter reservation is released when finalization actually completes,
so another native runtime can start after return. Retiring the old registration
cannot release the new runtime's ownership.
There is no worker join. Scripts that suppress cancellation or block forever in
native code must actually return before teardown can complete.

## Editor Entry and Verification

Window > Python Scripts lazily starts the runtime/session for the selected
MainFrame editor. `DURIN_PYTHON_SCRIPT` optionally submits a trusted startup file.
MainFrame's Python presenter owns a scoped Content Browser context action for
one ordinary `.py` file (case-insensitive extension). It declares content mutation
so browser Play policy gates both presentation and dispatch. Invocation uses the
owned physical path, opens the same presenter and submits through its existing
session/RunFile path. It never executes inline in the menu callback. Busy checks
include newly queued jobs before the first snapshot, preventing replacement of
the displayed/cancellable job. Scoped unregistration rejects retained action
identities after presenter retirement. Host startup explicitly submits its
environment-selected script through this same presenter entrypoint.
The panel/log show terminal output and errors. Long individual steps block panel interaction;
external detached cancellation remains available. Host shutdown stops Python
before releasing editor state; module shutdown repeats the idempotent Stop.

EnsureRuntime registers the ordinary `editor.python` submit/status/cancel
Toolset. Submission remains an ordinary GameThread provider operation. During the
script pump, only status/cancel bypass Tick through an installed detached job
control. The endpoint validates source, client, schema, contract and the exact
catalog registration identity; the pump refreshes the catalog and pins feature
publication/unload. Its callback holds only owned runtime/client/job state, never
an editor pointer or provider callback context. Outside that boundary the tools
use normal dispatch. Closed endpoints and ambiguous/retired routes reject access.
This narrow path preserves existing [MCP job controls](MCPTools.md#python-script-jobs)
without adding a general asynchronous provider scheduler or changing transport.

`PythonToolIntegrationTests` exercises deployed CPython, ready/pending completion,
ordered/fair continuations, real return-to-tick boundaries, global recording,
result-dependent short apply, cancellation/finally, multiple client sessions and sequential editor teardown.
`MCPPythonJobTests` verifies existing transport consumers with the explicit script
pump. Follow [usage](../Guides/PythonScripts.md), [testing](../../Agents/Testing.md)
and [build guidance](../../Agents/BuildAndRun.md).
