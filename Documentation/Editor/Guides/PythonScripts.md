# Run Python Editor Scripts

Summary: Prepare the optional Windows Python runtime and run trusted scripts against the selected editor's Toolsets.
Modules: DurinPython, MainFrame, DurinEd

Last reviewed: 2026-10-16

## Prepare and Run

The first supported host is Windows x64 with packaged CPython 3.13.15. Prepare
the SDK before configuring/building the editor:

```powershell
.\DevTool.bat dependency prepare --libs python
```

Then follow [build/run guidance](../../Agents/BuildAndRun.md). Python is optional;
ordinary editing remains available when the runtime is absent. SDK preparation
after configuration requires reconfiguration to enable the binding.
To explicitly disable embedding, configure with
`--define DURIN_ENABLE_PYTHON=OFF`; use `--define DURIN_ENABLE_PYTHON=ON` to
restore it after preparing the SDK. The disabled build retains the panel and
reports that Python is unavailable when a script is requested.

Open Window > Python Scripts, enter an absolute path to a UTF-8 `.py` file, and
select Run. The panel displays job state, captured output and exception details.
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
    result = durin.tools.call('editor.pie', 'pause', {'paused': True}, timeout=5.0)
    print(result['state'])
except durin.ToolError as error:
    print(str(error))
    print(getattr(error, 'diagnostics', []))
```

`call` waits on the script worker while the editor continues ticking. It returns
final business data, including None when the declared native result is null.
Explicit `version=...` pins a known contract; mismatches fail without retry.

## Invocation Handles

```python
import durin

invocation = durin.tools.submit('editor.pie', 'status', {})
print(invocation.state())
print(invocation.result(timeout=5.0))
# invocation.cancel() requests cancellation when needed.
```

Tool facades and handles are usable only in the job which created them. Jobs cancel unfinished
handles on exit. A timeout requests cancellation and raises TimeoutError; it does
not establish that running work stopped or edits were undone. Expired receipts
raise ToolError. Cancellation may raise `durin.CancelledError`, a BaseException
subclass; scripts must let it propagate.

## Supported Scope

Scripts have fresh globals but share imported module state. Source is limited to
1 MiB, output and error text to 64 KiB each, and job execution to a cooperative
30-second deadline. Tool values follow existing Core schema and size limits.
Use builtin/pure-Python standard-library facilities. Native extension packages,
background Python threads, cancellation suppression, and direct engine-object
bindings are outside the supported first release.

Script execution provides no whole-script transaction. Completed tools may have
effects even if a later operation fails or the job is cancelled. Use native
domain tools with appropriate transaction semantics for compound edits.

See the [adapter contract](../Architecture/PythonTools.md) for ownership,
conversion rules, resource limits and shutdown behavior.
