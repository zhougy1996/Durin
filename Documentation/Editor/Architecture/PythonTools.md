# Python Toolset Adapter

Summary: Embed trusted editor scripts with native Toolsets, reflected type/object wrappers and transaction scopes.
Modules: DurinPython, DurinEd, MainFrame, Core

Last reviewed: 2026-10-18

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

## Reflected Types and Object Wrappers

DurinPython also owns an interpreter-local native type registry. ScriptType
classes/structs/enums and classes with script-exposed members are published as
CPython heap types. Ancestors and exposed signature dependencies receive wrappers
without granting unmarked methods or native object constructors. Types retain
native inheritance; get_type(qualified_name) resolves canonical identities.
Top-level aliases omit the native type prefix. Colliding aliases are omitted with
a diagnostic, leaving qualified lookup available. Generated Python declarations
are not executable binding implementations.

`durin.get_editor_subsystem(type)` returns the selected Host's existing service,
wrapped with native identity and a non-owning Python factory observation. Object
wrappers expose get_name/get_class as bootstrap introspection helpers. Classes
are immutable and cannot be instantiated or subclassed in Python. Reflected
method dispatch, native value construction/conversion and reflected property
reads/writes are not available yet; they are separate implementation stages.

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

DurinEd's FScopedEditorScriptCall supplies synchronous Host/endpoint, source,
cancellation and edit-policy context without a Python dependency. Nested scopes
restore their predecessor and keep separate errors. A Host target-policy token
reports live document editability. LevelEditor registers one alongside its Level
context; read-only/unavailable or conflicting providers reject writes. Native
FPythonSessionOptions defaults to DocumentManaged; headless callers must explicitly
select NonDocumentReadOnly or NonDocumentReadWrite. Python/remote arguments cannot
change this policy. These policies establish admission for future editor writes;
Stage 1 does not yet expose Actor mutations or property setters.

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
state. Different hosts have separate histories and execution bindings; suspension
releases the binding, not the recording. Scope validity uses the native transaction
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
result-dependent short apply, cancellation/finally, two hosts and teardown.
`MCPPythonJobTests` verifies existing transport consumers with the explicit script
pump. Follow [usage](../Guides/PythonScripts.md), [testing](../../Agents/Testing.md)
and [build guidance](../../Agents/BuildAndRun.md).
