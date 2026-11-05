# Console Toolset Integration

Summary: Define Console discovery, typed Toolset invocation, explicit PIE controls, and host-owned result presentation.

Modules: MainFrame, LevelEditor, DurinEd, Core

Last reviewed: 2026-11-03

## Ownership and Routing

MainFrame's private `FConsoleToolAdapter` merges the existing
`FConsoleCommandRegistry` metadata with the owning editor's
[tool catalog](EditorTools.md). MainFrame receives that editor explicitly through
`IMainFrameModule::CreateDefaultMainFrame`; the adapter holds a weak object handle and a
detached service endpoint, never a process-global editor lookup or cached provider.
ConsolePanel uses the adapter for execution, help, completion, and receipt polling.
Ordinary legacy commands retain their original argument parser and callbacks.

Canonical addresses are `<toolset>/<tool>`, such as `editor.pie/status`. Toolset
and local names remain separate and case-sensitive. Legacy names retain case-insensitive lookup. Discovery compares names case-insensitively to
detect overlapping legacy/tool entries. Help marks collisions and dispatch
refuses them; neither implementation executes. Completion returns one sorted
name per merged entry. `help` lists both systems, while `help <name>` includes
tool descriptions, scalar options/default markers, and the argument-file form.

## Structured Arguments

Use named scalar fields:

```text
editor.pie/pause --paused true
```

Each option appears at most once. Booleans are exactly `true` or `false`, integer
tokens must fit their signed/unsigned schema and be consumed completely, numbers
must be finite, and enum strings match exactly. No implicit string/numeric/bool
coercion is performed. The tool contract supplies defaults and required fields.
Single/double quotes group tokens. Inside quotes, a backslash escapes only the
quote delimiter or another backslash; otherwise it is preserved. Unquoted
backslashes are preserved, including Windows paths. The legacy tokenizer does
not process structured argument text.

Objects and arrays require one JSON argument file:

```text
example.tools/configure --args-file "C:\Project Data\arguments.json"
```

`--args-file` cannot be mixed with scalar options and is reserved as a Console
option. A schema may use an `args-file` business field only through the JSON file.
Files contain a UTF-8 JSON object of business arguments; an optional UTF-8 BOM is
accepted. Invalid JSON/UTF-8, duplicate or unknown fields, incompatible types,
fractional integer inputs, and integer overflow are rejected. Nonnegative JSON
integers are converted to the declared signed/unsigned type only after range
checking, preserving 64-bit values. `FJsonDocument` remains private to this
adapter; requests contain deep-owned [Core tool values](../../../Runtime/Core/Tools.md).

Command text and opened file bytes each have a 1 MiB limit. File size is checked
before allocation through the bounded Core file reader. Conversion also enforces
Core depth, node, container, and aggregate string limits before publishing the
request. No inline JSON is routed through the legacy parser.

## Execution and Presentation

The adapter runs on GameThread. At a ready service safe point it calls `CallNow`
once with Console source; otherwise it submits once to the existing bounded queue.
It never automatically retries a provider's Busy/failure result, which may follow
domain side effects. `CanCallNow` is advisory scheduling information, not permission
to skip service validation or host admission.

Immediate successful tool calls render JSON data; failures render diagnostic
codes, paths, and messages. Result display truncates at 64 KiB. Queued calls display a session/sequence receipt
immediately. Console Tick polls up to 256 retained receipt identities and appends
their completion, cancellation, or expiration without blocking GameThread.
MainFrame invokes this polling from `TickMainFrame` after script and world
updates, independently of rendering and Console visibility.
Closing the panel cancels its queued submissions; running cancellation remains
cooperative. Host shutdown closes admission and settles pending work through the
tool service. A retained adapter remains safe after host GC and cannot retarget a
replacement editor.

## Name Pool Diagnostics

MainFrame registers the module-owned `core.namepool` Toolset at module startup,
independently of host-window creation, and unregisters it before destroying its
native bindings. DurinEd retains only the shared Toolset infrastructure.
Run `core.namepool/stats` without
arguments for an on-demand process-wide snapshot from Core's `GetNamePoolStats()`.
The read-only, idempotent tool is available to Console and native callers; it does
not declare external exposure. Console help and completion use the shared catalog.

Results contain created-entry counts, entry/block/hash-slot byte counts, active
and allocated block counts, block size and limit, and `comparison`/`display`
summaries with shard count, created entries, used slots, and capacity. Byte fields
use bytes; entry storage includes headers and alignment, allocated blocks include
spare capacity, and hash slots are separate. Each shard is sampled independently,
so concurrent inserts do not produce a globally atomic snapshot. No sampling
history, growth rate, per-shard listing, or capacity mutation is retained.

## Tracy Tools

MainFrame registers the module-owned `editor.tracy` Toolset independently of
host-window creation and unregisters it before releasing its native bindings.
The existing profiling service owns manifest resolution and platform operations;
no profiler process is owned by editor shutdown. All tools are available to
Console and native callers without declaring external exposure.

| Tool | Behavior |
| --- | --- |
| `editor.tracy/status` | Read-only, idempotent installation and connection-readiness report; unavailable tools remain successful diagnostic data |
| `editor.tracy/launch` | Launches the managed profiler for this process's actual localhost port; does not claim handshake success |
| `editor.tracy/open_capture --path "capture.tracy"` | Opens an explicit existing capture in the managed profiler |

Actions declare external effects and unsafe retries. The shared argument schema
rejects missing/empty capture paths. Domain failures preserve service diagnostics. See the
[profiling workflow](../../../Development/Build/Profiling.md#editor-tool-workflow)
for configuration sources and connection constraints.

## PIE Tools

LevelEditor registers the module-owned `editor.pie` Toolset at module startup and
unregisters it before destroying its native bindings. Each handler resolves its
admitting host through `GetToolEditor(Context)` and calls its
[`DLevelEditorSubsystem`](../Framework/EditorSubsystems.md#level-editor-play-controls).
The subsystem owns domain checks and delegates PIE lifetime to `DEditorEngine`;
handlers retain tool metadata and result conversion. The dispatcher adds no implicit Undo transaction.
All successful results contain `message` and a `state` field of `stopped`,
`playing`, or `paused`.

| Tool | Behavior |
| --- | --- |
| `editor.pie/status` | Reports state; read-only and idempotent |
| `editor.pie/play` | Starts from the owning editor's current level |
| `editor.pie/stop` | Stops an active session; stopped state is a failure |
| `editor.pie/pause --paused true\|false` | Sets explicit pause state; idempotent while playing |

These four tools declare external exposure for future adapters; that flag is not
an authorization grant. Pause always requires an explicit boolean, including for
queued calls. Console uses the same structured result format for every tool.

The four migrated callbacks are removed from EditorEngine's legacy registry.
`pie.step`, `pie.physics`, `pie.apply`, `clear`, `asset.fixup_redirectors`, viewport,
and renderer commands retain their existing legacy paths. The same PIE Toolset
is exposed through [MCP](MCPTools.md) and [Python](PythonTools.md).
