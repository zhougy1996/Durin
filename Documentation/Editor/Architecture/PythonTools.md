# Python Toolset Adapter

Summary: DurinPython runs trusted scripts on one interpreter worker and submits editor Toolsets through a session-bound detached endpoint.
Modules: DurinPython, DurinEd, MainFrame, Core

Last reviewed: 2026-10-17

## Ownership and Runtime

DurinPython is an optional editor scripting module. Core and business providers
have no Python dependency. The module exposes owned native session/job APIs in
`PythonRuntime.h`; CPython objects and ABI symbols remain private.

The first qualified runtime is CPython 3.13.15, x64 Windows, release GIL ABI.
The `python` dependency manifest pins the official NuGet SDK archive and its
SHA-256. Prepare it through the repository dependency service before configuration.
Other hosts, or configurations without that SDK, compile an unavailable-runtime
implementation. Missing runtime files return a startup error; Python is not a
loader dependency of MainFrame or DurinPython.
`DURIN_ENABLE_PYTHON=OFF` explicitly selects that unavailable implementation.

Builds package the standard library as `python313.zip` and deploy it with
`python313.dll`, the PSF license and `durin-python.zip` through normal runtime/test
deployment. The `durin` package lives in `DurinPython/Python/durin`; its tool
facade, invocation handles and output adapter are ordinary Python modules.
The private built-in `_durin` module supplies the native request binding and
exception types, re-exported by `durin` with their existing exception names.
Python-only edits rebuild and redeploy the package without requiring a native
module relink.
The private ABI table explicitly resolves the required symbols. The DLL stays
loaded for process lifetime. A process-wide owner permits only one active Durin
interpreter. Python-backed providers and GameThread callbacks into Python are
not supported.

Start/Stop are owner operations serialized by the runtime lifecycle lock against
short public native calls. GetJobResult is named explicitly to avoid the Windows
GetJob macro; all consumers use that exported native API.
The worker initializes and finalizes CPython. Isolated PyConfig disables site,
environment search paths, bytecode writes and signal-handler installation; only
the deployed standard-library and Durin package zips are added to its module
search path. Native
extension packages and user-created background Python work are unsupported.
Trusted Python remains ordinary process code, not a sandbox.

## Sessions and Invocation

Acquire `DEditorToolSubsystem::GetService()` on GameThread from the explicitly
selected editor, then pass its detached endpoint to CreateSession. A session
keeps that endpoint and a host-selected EToolSource; scripts cannot replace the
source, host, invocation identity or cancellation context.

External is the default source. Discovery filters `bExposeExternally`, while
Submit enforces it again. A host may explicitly create a trusted Native session.
Language choice is not an access policy. Python introduces no new source enum.

The worker serializes jobs and gives each fresh globals with `__name__` and
`__file__`. Imports share interpreter state. Each `durin.tools` facade is freshly
bound to its job and rejects reuse in a later job. Invocation handles also carry a
job identity; retaining one through shared imports does not permit later jobs to
use it or redirect it to another host.

`durin.tools.list()` returns toolset/tool names, descriptions and contract
versions. `describe(toolset, tool)` also returns input/output schemas. Schema
`type` is the native EToolValueType integer; bounds/defaults/enums and UTF-8 byte
size limits retain native semantics. Toolset and tool names stay separate;
toolsets permit dot-separated segments and local tool names cannot contain dots.
The MCP adapter joins them for discovery; Python passes the original two names.

Submit pins the discovered contract version unless the caller explicitly supplies
a version. Arguments become owned FToolValue data before entering the existing
[editor tool service](EditorTools.md). Defaults and authoritative schema/output
validation run there. Python performs checked schema-directed numeric conversion,
preserves signed/unsigned 64-bit values, rejects non-finite numbers, cycles and
unsupported objects, and bounds traversal/copying with Core's value budgets.

No worker accesses DObjects or invokes handlers. Existing TickTools dispatches
on GameThread at the existing host/transaction safe point. There is no synchronous
GameThread script execution or wait API. `call` uses Submit/GetInvocation, releases
the GIL while polling, then reacquires it to return final data or raise an error.
Queued/running admission is never reported as successful business data.

## Results, Limits and Closure

Successful native Data becomes ordinary Python None/bool/int/float/str/list/dict.
Failures raise ToolError with a diagnostics list containing numeric code, RFC 6901
path and message. Exceptions retain script filenames and bounded tracebacks.
No provider failure triggers automatic retry. stdout/stderr share a bounded job
output buffer; overflow fails the job even if the script catches the write error.

| Resource | Limit |
| --- | --- |
| Editor sessions | 8 |
| Outstanding jobs | 16 |
| Retained jobs | 64, oldest terminal jobs evicted first |
| Script source | 1 MiB; no embedded NUL; UTF-8 |
| Filename | 4096 bytes; no embedded NUL |
| Captured output / exception text | 64 KiB each |
| Owned invocations per job | 256 |
| Job deadline | 30 seconds, checked cooperatively |
| Tool wait | 0-30 seconds, default 30, capped by job deadline |
| Receipt polling interval | 5 ms |

RunFile accepts absolute paths or paths relative to FPaths::ProjectDir, reads
bounded source before queue admission and does not change process cwd or sys.path.
Its resolved absolute path becomes __file__ and the traceback filename. Later
edits do not alter queued source; imports are still resolved when execution occurs.
Jobs expose Queued, Running, Succeeded, Failed and
Cancelled snapshots. Tool handles preserve the native receipt state vocabulary.
Unknown/evicted IDs fail explicitly. Completed jobs own no editor endpoint.

Cancellation stops later binding calls, removes queued jobs, and requests native
invocation cancellation. Python tracing checks both line and opcode events.
Every job cancels unfinished owned invocations on exit, including handles never
awaited. Closing a session cancels its jobs and prevents new submissions. Stop
closes sessions and cancels waits before joining the worker, so supported scripts
cannot wait for another editor tick during shutdown. Finalization occurs only
after script work and job-local Python references have returned.

Cancellation does not preempt C++ handlers or roll back effects. Python scripts
which suppress cancellation and indefinitely blocking native code are unsupported;
module shutdown must wait for actual return and has no hard deadline for those
cases. Do not terminate a thread or finalize an interpreter still in use.

Each domain tool owns its transaction. There is no whole-script Undo/rollback or
transaction kept open across queued calls; that would conflict with idle-host
dispatch. Compound atomic operations require their own native business contract.

## Editor Entry and Verification

MainFrame creates a presenter for its selected editor. Opening Window > Python
Scripts and running an absolute script file lazily starts the runtime/session.
The panel shows status, output, diagnostics and cooperative cancellation. The
optional `DURIN_PYTHON_SCRIPT` absolute path submits one trusted startup script;
normal startup runs no script. Terminal results are also emitted to the editor log.
Host destruction stops Python before releasing editor UI/host state; module
shutdown repeats the idempotent stop.
EnsureRuntime registers `CreatePythonToolset(Runtime)` as `editor.python`. Its
submit/status/cancel handlers work through native, console and MCP tool dispatch.
The definition and registration belong to DurinPython and are removed before
runtime shutdown. PythonRuntime remains independently callable from C++ and UI.
The Toolset scopes opaque job handles by editor endpoint, source and client ID.
FToolClientContext supplies a detached client cancellation token; runtime sessions,
jobs and script-originated tool requests preserve it without retaining transport
objects. Closing an MCP client cancels its work even without another editor tick.
When MCP is enabled, EnsureRuntime shares this interpreter before UI session
creation. Stopping MCP does not stop the interpreter or UI
jobs. Native shared ownership and lifecycle locking make late queries return an
unavailable result after owner shutdown rather than accessing freed state. See
the [MCP job contract](MCPTools.md#python-script-jobs) for external operations.

`PythonToolIntegrationTests` uses the deployed interpreter with CPU editor hosts
for values, PIE calls, errors, cancellation, retention, host isolation and teardown.
Follow [script usage](../Guides/PythonScripts.md), [testing](../../Agents/Testing.md)
and [build guidance](../../Agents/BuildAndRun.md) for supported workflows.
