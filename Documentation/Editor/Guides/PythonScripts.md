# Run Python Editor Scripts

Summary: Run trusted GameThread scripts using the selected editor's native Toolsets and transaction scopes.
Modules: DurinPython, MainFrame, DurinEd

Last reviewed: 2026-10-18

## Prepare and Run

The first supported host is Windows x64 with packaged CPython 3.13.15. Prepare
the SDK before configuring/building the editor:

```powershell
.\DevTool.bat dependency prepare python
```

For macOS native-host verification, an explicit same-version GIL SDK may be
selected with `./DevTool configure -DDURIN_PYTHON_SDK_ROOT=/absolute/sdk`.
Its layout and code-signing requirements are in the [runtime contract](../Architecture/PythonTools.md#ownership-and-runtime).
This does not install or search the user's system Python.

Then follow [build/run guidance](../../Agents/BuildAndRun.md). Python is optional;
ordinary editing remains available when the runtime is absent. SDK preparation
after configuration requires reconfiguration to enable the binding.
To explicitly disable embedding, configure with
`--define DURIN_ENABLE_PYTHON=OFF`; use `--define DURIN_ENABLE_PYTHON=ON` to
restore it after preparing the SDK. The disabled build retains the panel and
reports that Python is unavailable when a script is requested.

Open Window > Python Scripts, enter an absolute or project-relative path to a
UTF-8 `.py` file, and select Run. The panel displays job state, captured output
and exception details.
Cancel requests cooperative cancellation. Scripts run in the editor process with
normal process privileges; use trusted scripts. No packages are installed from
the user's Python environment.

For an explicit startup script, set `DURIN_PYTHON_SCRIPT` to its absolute path in
the launching process before starting the editor. Unset it after the intended
launch. The editor log records the final status, output and traceback.

## Discover and Call Tools

```python
import durin

for tool in durin.tools.list():
    print(tool['toolset'], tool['tool'], tool['version'])

print(durin.tools.describe('editor.pie', 'pause'))
print(durin.tools.call('editor.pie', 'status', {}))
```

These are the native two-part names, not MCP's escaped advertised names.
The default external session sees only externally exposed tools. Availability
is checked when the tool executes, not inferred from discovery.

To pause an already playing session:

```python
import durin

try:
    result = durin.tools.call('editor.pie', 'pause', {'paused': True})
    print(result['state'])
except durin.ToolError as error:
    print(str(error))
    print(getattr(error, 'diagnostics', []))
```

`call` executes immediately on GameThread and returns final business data,
including None for a native null result. Scripts block editor UI interaction
until a step returns. Call rejects tools declared as potentially pending before
their provider executes.
Explicit `version=...` pins a known contract; mismatches fail without retry.

## Invocation Handles And Later Steps

```python
import durin

invocation = durin.tools.submit('editor.pie', 'status', {})
print(invocation.state())
print(invocation.result())  # Ready handlers still complete immediately.
```

A pending handle's result() raises `durin.NotReady`; it never blocks. Native
failures raise ToolError. Handles and tool facades belong to their creating job.
Use a generator to wait cooperatively:

```python
import durin

def prepare_and_apply():
    # Replace these illustrative names with registered native tools.
    prepared = yield durin.tools.submit('your.toolset', 'prepare', {})
    with durin.ScopedEditorTransaction('Apply prepared edit'):
        durin.tools.call('your.toolset', 'apply', prepared)
    yield  # Resume any remaining work on a later tick.

durin.defer(prepare_and_apply())
```

`durin.defer` also accepts one-shot callbacks and callbacks returning generators.
Registrations run in order within the job; dynamic registrations append. A generator
finishes before later continuations in that job. Other ready jobs interleave between
steps. A callback returning a generator takes one step to create it and starts its
body on a later tick. Each individual step runs synchronously, so split long work
explicitly. There is no automatic asyncio or background Python execution.

A job remains live until its continuations and owned native calls settle. Dropping
a pending handle does not create fire-and-forget work. Handle cancel() requests
cooperative cancellation of that invocation; job Cancel stops normal resumes,
signals owned work and closes generators so finally blocks run on GameThread.
Cleanup finishes before terminal status. Neither cancellation restores applied edits.
The per-call/result timeout parameter has been removed.

## Group Native Edits

```python
import durin

with durin.ScopedEditorTransaction('Edit scene'):
    result = durin.tools.call('your.toolset', 'read', {})
    durin.tools.call('your.toolset', 'edit', {'value': result['value'] + 1})
```

Replace the example tool names with registered native tools. Participating edits
use the same native history as C++. Nested scopes join one outer recording.
Normal exit and exception exit both End; exceptions propagate. Calling the scope's
`cancel()` discards the entire recording, including from an inner scope. Applied
edits remain after exception or cancellation. Neither operation promises rollback.
A job closes unfinished scopes before it publishes completion.

A scope spanning yield keeps that editor's global recording open. UI/native edits
and other jobs on the same host can join it, and Undo/Redo stays unavailable during
recording. Other hosts keep separate histories. Prefer waiting outside a transaction,
then revalidate the target and apply within a short synchronous scope.

## Supported Scope

Scripts have fresh globals but share imported module state. Source is limited to
1 MiB and output/error text to 64 KiB each. Native hosts can configure one optional
total-elapsed JobDeadline, including queueing and suspension; the default has no
fixed deadline. It cannot preempt native code. Tool values follow Core limits; each
job retains at most 256 submissions and 256 live continuations.
Use builtin/pure-Python standard-library facilities. Native extension packages,
background Python threads, cancellation suppression, and direct engine-object
bindings are outside the supported first release.

Use ScopedEditorTransaction when multiple native edits should share one history
entry. A job alone does not create a transaction. Completed edits remain if a
later operation fails or the job is cancelled.

See the [adapter contract](../Architecture/PythonTools.md) for ownership,
conversion rules, resource limits and shutdown behavior.
For agent submission, polling and cancellation over authenticated local MCP, see
the [MCP script workflow](LocalMCP.md#submit-inspect-and-cancel-python-scripts).
MCP and the panel share one runtime with independent job/session ownership.
