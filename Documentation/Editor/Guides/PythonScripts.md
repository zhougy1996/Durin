# Run Python Editor Scripts

Summary: Run trusted GameThread scripts using the selected editor's native Toolsets and transaction scopes.
Modules: DurinPython, MainFrame, DurinEd

Last reviewed: 2026-10-20

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

You can also select a single `.py` file in the Content Browser, right-click it,
and choose **Run Python Script**. This works for ordinary files under Engine or
project content, including physical directories without an asset identity. The
Python Scripts panel opens with the selected path and shows the same job state,
output, errors and Cancel control. A running panel job must finish or be cancelled
before another file can run through this entry. The action is disabled while Play
disallows content mutation; it does not add script authority to edit read-only
documents. Missing files and unavailable Python are reported in the panel.

For a ready-to-run example, open Engine content at
`Scripts/Examples/CreateTestScene.py` and choose **Run Python Script** with an
empty writable Level open. It creates six `Example_*` Actors: an empty origin,
camera, directional light, procedural sky, point light and player start. It also
edits camera position and light properties. Ctrl+Z removes the whole example;
Ctrl+Y restores it. It loads no geometry assets and does not save the Level.
The example identifies its Actors through a persistent `Tags` marker. Running it
again while any marked Actor exists reports an error before editing, even if its
label changed. Undo followed by another run reuses the `Example_*` labels; internal
object names may differ because history retains the original objects.

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

## Query and Edit Actors

Actor edits require a writable current Level document. Native headless hosts must
explicitly grant NonDocumentReadWrite when creating the session.

```python
import durin

actors = durin.get_editor_subsystem(durin.EditorActorSubsystem)
level = actors.get_editor_level()
if level is None:
    raise RuntimeError('No editor Level')

with durin.ScopedEditorTransaction('Create empty Actor and camera'):
    empty = actors.spawn_actor(level, durin.Actor, durin.Transform(),
                               label='PythonEmptyActor')
    camera = actors.spawn_actor(level, durin.CameraActor, durin.Transform(),
                                label='PythonCamera')
    if empty is None or camera is None:
        raise RuntimeError('Actor creation failed')

    root = actors.get_actor_info(level, camera).root_component
    pose = root.get_editor_property('relative_transform')
    position = pose.translation
    position.x += 100.0
    pose.translation = position
    root.set_editor_property('relative_transform', pose)
```

This works in an empty writable Level: the Actor and ActorComponent bases expose
their derived types through inherited `ScriptType`, without an existing instance
or per-subclass annotation. `ScriptNoExport` excludes a class and its descendants.
Other members still require their own exposure. Plain Actor has no default root
component; the camera above supplies one for transform editing. After the scope
ends, Ctrl+Z removes both actors and Ctrl+Y restores them with the edited camera
transform as one history entry.

`actor.get_name()` returns its internal object Name; `actor.get_actor_label()` and
`actors.get_actor_info(level, actor).label` return the display label. Use
`actors.set_actor_label(level, actor, 'Camera')` for ordinary editor renaming.
`rename_actor` is an internal-name operation and leaves the label unchanged.
Labels are not identity keys. For application membership, use a persistent marker
such as `actor.set_editor_property('tags', [durin.Name('MyTool.Scene')])`.

The `name` argument requests an internal name and may receive a numeric suffix.
For an exact internal name, pass
`name_mode=durin.SpawnActorNameMode.REQUIRED_RETURN_NULL`; a conflict returns None
without construction or history. `REQUIRED_ERROR_AND_RETURN_NULL` also logs an
error. `REQUESTED` is the default. These policies also apply to `rename_actor`.

The same service provides duplicate_actors, destroy_actors, set_actor_label, rename_actor,
attach_actors and detach_actors. Supply explicit Level and Actor arguments;
selection does not determine targets. Creation returns Actor or None, duplication
returns an Actor list (empty on failure), and other edits return bool. Successful
no-ops also return true and create no history. Per-operation failure details are
not exposed. Invalid queries raise durin.ScriptAccessError. Type errors and stale
arguments raise binding exceptions before the edit. Empty and duplicate batches
are rejected. Queries inspect the editor Level during PIE; editing requires
stopped play.

## Edit Reflected Properties

When an exposed native method returns an Actor or Component, use editor-property
helpers to read and replace eligible fields:

```python
def move_component(component):
    pose = component.get_editor_property('relative_transform')
    translation = pose.translation
    translation.x += 10
    pose.translation = translation
    with durin.ScopedEditorTransaction('Move component'):
        component.set_editor_property('relative_transform', pose)
```

Values are copies, so assign modified nested values back before replacing the
native property. Direct assignment is also available for `ScriptReadWrite`
fields. Helpers require editable reflected fields; native read-only,
nontransactional and excluded fields reject writes. The target must belong to
the current editor Level and the native Host must grant write access. Assets and
subsystem fields cannot be edited this way. Root fixed arrays cannot be replaced;
supported structs, dynamic arrays and maps can.

Native validation, notifications, dirty state and Undo/Redo follow ordinary editor
editing. A rejected value raises `ValueError`; denied editability raises
`durin.ScriptAccessError`. Earlier successful edits and their undo history remain
when a later value is rejected. Deferred validation is unavailable in this
synchronous helper.

## Discover Native APIs and Generate IDE Hints

Use `dir(durin)` to find the native types published by loaded modules and
`dir(durin.EditorActorSubsystem)` to find its methods. Inspect a method's
`__doc__` or call `help(durin.EditorActorSubsystem.spawn_actor)` for its Python
signature. Type docstrings retain the native qualified name for `get_type`.

Run this in an editor Python job to write completion declarations, using your
checkout's absolute path:

```python
from pathlib import Path
import durin

output = Path(r'D:\Studyspace\Durin-architect\Build\PythonStubs\durin\__init__.pyi')
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(durin.generate_stubs(), encoding='utf-8')
```

Add the checkout's `Build/PythonStubs` directory to your IDE's Python analysis
paths. Regenerate after loading modules or changing reflected APIs. The snapshot
contains declarations for published native APIs, including Actor editing;
generation itself performs no scene edits. Defaults displayed as `...` can be
omitted. Native-only defaults still require an explicit value. Hints cannot prove
object lifetime, editor permissions or numeric/container limits; those checks
still occur when the script runs. Dynamic Toolset calls retain their runtime
discovery workflow above.

## Supported Scope

Scripts have fresh globals but share imported module state. Source is limited to
1 MiB and output/error text to 64 KiB each. Native hosts can configure one optional
total-elapsed JobDeadline, including queueing and suspension; the default has no
fixed deadline. It cannot preempt native code. Tool values follow Core limits; each
job retains at most 256 submissions and 256 live continuations.
Use builtin/pure-Python standard-library facilities. Native extension packages,
background Python threads and cancellation suppression remain unsupported.
Exposed native types are available through durin.get_type(qualified_name), and
get_editor_subsystem(type) returns an existing exposed service on the selected
Host. These object wrappers support identity, inheritance, get_name/get_class
introspection and direct calls to methods explicitly marked ScriptCallable.
Calls accept positional/keyword arguments and reflected defaults. Native-only
defaults require explicit arguments; unsupported signatures are not published.
Services must opt their methods into script exposure. The Actor subsystem above
uses ordinary reflected native methods and result types.

Script-exposed attributes can be read. Mutable ScriptType structs and intrinsic
math values support default/named-field construction and detached writable
fields. Struct and container reads return copies: write modified nested values
back to their containing struct or object with the property helpers above.
Name and Guid accept validated strings; enums require the exact reflected enum
type, and object parameters require compatible live proxies (or None).

Methods return None, a single output, or a return-first tuple. A bool return with
out parameters uses None for failure and packs only the out values on success.
Input/output reference parameters still need inputs. Invalid arguments and
conversion budgets reject calls before native effects; failure while converting
outputs preserves a native operation that already completed. See the adapter
contract for exact supported types, budgets and lifetime rules.

Use ScopedEditorTransaction when multiple native edits should share one history
entry. A job alone does not create a transaction. Completed edits remain if a
later operation fails or the job is cancelled.

See the [adapter contract](../Architecture/PythonTools.md) for ownership,
conversion rules, resource limits and shutdown behavior.
For agent submission, polling and cancellation over authenticated local MCP, see
the [MCP script workflow](LocalMCP.md#submit-inspect-and-cancel-python-scripts).
MCP and the panel share one runtime with independent job/session ownership.
