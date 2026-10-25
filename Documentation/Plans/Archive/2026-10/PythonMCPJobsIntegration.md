# Python MCP Jobs Integration Plan

Summary: Expose session-owned Python script submission, result polling and cooperative cancellation through DurinMCP using the shared editor Python runtime.

Last reviewed: 2026-10-16

Status: Archived
Completed: 2026-10-16

## Current Status

All stages are complete on the qualified Windows x64 Python runtime. MCP exposes
submit/status/cancel with session ownership and shares the editor interpreter.
Twelve new real-HTTP cases pass together and in isolation; all ten selected
regression targets, workspace all builds and real-editor official SDK acceptance
pass. No required gate remains. This explicitly extends the earlier Python
Toolset plan's deferred MCP execution scope.

### Acceptance evidence

- Tested baseline: `b789de342` plus this implementation, windows-msvc-x64 profile,
  Win64-Debug-DurinEditor, packaged CPython 3.13.15 and official Python MCP SDK
  2.3.0 in legacy handshake mode against the existing 2025-06-18 server.
- Shared ownership/lifecycle locking passed the existing 20 Python cases and new
  concurrent owner-stop/HTTP query and UI/MCP coexistence cases. GetJob was renamed
  to GetJobResult because Windows' spooler header rewrites GetJob to GetJobW;
  every consumer across Engine, Sandbox and RoadWeaver was searched/migrated.
- MCPPythonJobTests passed all 12 cases in one process and with four isolated
  slots, including the exact 64 KiB accepted source boundary. Coverage includes
  admission without editor tick, actual Toolset execution, traceback filenames,
  queued/running cancellation, foreign IDs, input/queue/retention limits, delete,
  stop, connection lifetime, Unicode budgets, runtime absence and name collision.
  Receipt: `Build/NativeTestResults/Win64-Debug-DurinEditor/MCPPythonJobTests.xml`;
  raw isolation log: `Build/.agent-state/logs/20261016-171033-163180-36784-ctest.log`.
- Long-script tests exposed an existing parser defect: the container entry budget
  also charged string byte length through Num(). Container checks now apply only
  to objects/arrays; dedicated string budgets remain enforced and codec tests
  cover both accepted long strings and rejected oversized strings.
- Final `@domain=python+tools+editor-shell+mcp` selection passed all ten targets.
  Raw log: `Build/.agent-state/logs/20261016-171258-123746-33164-ctest.log`;
  receipt: `Build/NativeTestResults/Win64-Debug-DurinEditor/domain-python-tools-editor-shell-mcp.xml`.
  The legacy opt-in SDK-host case remains skipped in ordinary runs as designed;
  the new application SDK acceptance was run independently.
- Final all build passed across workspace projects after the shared API migration:
  `Build/.agent-state/logs/20261016-171242-180776-40360-cmake.log`.
- MCPPythonClientSmoke.py passed against the real Sandbox editor: discovered seven
  tools, verified a shared UI interpreter, submitted PIE status/play/pause/stop,
  polled output, observed a traceback, cancelled a loop, rejected a foreign job
  query and exited normally. Results: `Build/MCP/python-sdk-results.json`;
  launcher log: `Build/MCP/python-sdk-editor.log`; application log:
  `Build/.agent-state/logs/20261016-170714-314225-21472-DurinEditor.log`.
  Temporary credentials are removed; no token is committed or printed.
- Lasting contracts and client workflow are in the MCP/Python architecture and
  guides. Changed-document, all-plan and diff validation pass. Python remains
  trusted process code, with shared import state and cooperative cancellation;
  hard termination, sandboxing and other native Python platforms remain deferred.

## Goal and Selected Design

Add `editor.python__submit`, `editor.python__status` and `editor.python__cancel`
as adapter-owned MCP tools when the host supplies a running Python runtime.
Submit accepts UTF-8 code (up to 64 KiB) and an optional diagnostic filename,
returns an opaque job ID immediately, and does not claim script completion.
Status returns actual state/output/error. Cancel requests cooperative cancellation
and does not imply rollback or completed termination. No remote file-path runner
or synchronous script wait is added.

MainFrame's Python module owns one shared interpreter for both UI and MCP. MCP
sessions lazily create separate External Python sessions against the server's
explicit editor endpoint. Each MCP session owns its job-ID map; foreign, expired
and unknown IDs fail without disclosing another session's jobs. Session deletion,
idle expiry and server stop close owned Python sessions and cancel their jobs.
HTTP disconnect alone does not cancel. The completed submit RPC's ID is distinct
from its script job ID; use the cancel tool for an admitted job.

Serialize runtime Start/Stop against short native API calls so HTTP workers can
query or cancel safely during owner shutdown. MCP holds a shared runtime handle,
does not initialize CPython on HTTP workers, and stops before the Python owner
during editor host destruction. Native script Toolset calls still pass through
EditorToolService and retain External source/exposure checks. Python itself is
trusted process code with normal process privileges, not a sandbox; the configured
MCP bearer token therefore grants script execution when these tools are enabled.

Reuse the [MCP adapter](../../../Editor/Architecture/Tools/MCPTools.md),
[Python adapter](../../../Editor/Architecture/Tools/PythonTools.md) and
[editor tool service](../../../Editor/Architecture/Tools/EditorTools.md). Adapter tools use
normal schema descriptions/results, explicit route kinds and collision rejection;
they do not register global native Toolsets or call Python from GameThread tool
handlers. Python absence omits these tools while ordinary MCP remains available.

## Implementation Stages

### Stage 0: Share runtime ownership and synchronize lifecycle

- [x] Add owner-side EnsureRuntime with shared native ownership; reuse it from UI
  and MCP without creating a second interpreter or elevating script source.
- [x] Synchronize public runtime access against Start/Stop; preserve worker-only
  Python execution and no GameThread wait on a future editor tick.
- [x] Migrate all workspace consumers and order MCP closure before Python closure.
- [x] Verify existing Python tests, runtime/UI coexistence and stopped-runtime
  access; complete all build coverage for the shared Engine API change.

Completion: both adapters can share the same runtime with explicit, safe teardown.

### Stage 1: Expose session-owned MCP script jobs

Dependencies: Stage 0.

- [x] Add complete schemas/annotations and explicit routes for submit/status/cancel;
  merge sorted discovery and reject collisions with native advertised tools.
- [x] Validate bounded code, filenames and opaque job IDs; preserve valid UTF-8
  when Python output/error reaches its native byte budget.
- [x] Implement immediate admission, status snapshots and cooperative cancel with
  MCP-session ownership, bounded job maps and unchanged native invocation routing.
- [x] Close owned Python sessions on delete/expiry/stop; keep disconnected jobs
  queryable and preserve actual completion versus cancellation outcomes.
- [x] Add real HTTP/editor tests for discovery, Toolset execution, failure output,
  cancellation, ownership, invalid inputs, limits, closure and runtime absence.

Completion: an authenticated client can submit, inspect and cancel only its jobs.

### Stage 2: Qualify and publish the external contract

Dependencies: Stages 0-1.

- [x] Follow [testing](../../../Agents/Testing.md) and [build/run](../../../Agents/BuildAndRun.md);
  run new integration cases together and in isolation, relevant Python/Toolset/
  Console/MCP regressions, and the required workspace all build.
- [x] Exercise a real SDK client against an application editor: discover, submit
  script using durin.tools, poll final output, observe a traceback and cancel work.
- [x] Document schemas, job states, session ownership, unavailable-runtime behavior,
  limits, trust boundary and client examples in the owning architecture/guides.
- [x] Record exact baseline, selections and concise outcomes here; retain raw logs
  under ignored Build, validate documents/plans and commit completed work with
  exact plan/stage provenance.

Completion: required checks pass, durable contracts are published, and no required
gate remains. Other Python platforms, hard cancellation and sandboxing remain
outside this extension.
