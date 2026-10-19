# Editor Interface Cleanup Plan

Summary: Internalize recording hooks, make editor activation explicit, and remove the tool result compatibility wrapper.

Last reviewed: 2026-10-19

Status: Archived
Completed: 2026-10-19

## Current Status

Implementation and qualification are complete. Recording identity is
private to scopes; object capture is a private buffer helper behind ITransaction.
Init/subsystem startup now require explicit activation, and repeated activation
is rejected. Headless fixtures claim the editor explicitly. Tool snapshots and
MCP polling retain the native read-only shared result allocation directly.
EditorOperationTests passed 134 cases, including explicit activation preconditions,
sequential lifetimes, stale scopes, shared allocation and expired-result checks.
The affected-test explanation expands a shared native fixture header to all
Engine tests; named CPU suites cover the changed boundaries and consumers.
The final all build passed and covers the exported header/layout and Launch
integration. Seventeen named test targets passed 912 cases with no skips,
covering editor, World/structural, viewport, Python/MCP, material and project
consumers. Validation used 28bd1588e plus the final Stage 1 source and fixtures;
only documentation and receipt bookkeeping changed afterward. Receipts and the
per-target manifest are preserved locally in
Build/EditorInterfaceCleanupValidation/receipts.md; the production build receipt
is 20261019-125638-837753-28888-cmake.log in that directory.

## Selected Decisions

- Keep ITransaction::SaveObject public as the runtime capture contract. Remove
  DTransactor's public capture hook; buffer capture and recording identity are
  private to the transaction/scope collaborators. Tests observe supported
  outcomes rather than exposing recording internals again.
- ActivateEditor is the sole publication boundary, called explicitly by Launch
  and headless fixture setup. Init and subsystem initialization require the
  already-active editor and never activate it. Reject repeated activation;
  retirement still releases the editor only after callbacks/World work unwind.
- Invocation snapshots expose shared_ptr<const FToolResult> directly. Remove
  optional emulation and implicit deep copies. MCP holds the shared result until
  bounded protocol serialization; synthetic adapter errors own their storage.

## Implementation Stages

### Stage 0: Record interface and lifetime decisions

- [x] Record migration scope, lifecycle invariants and acceptance gates.
- [x] Validate and commit the plan before implementation.

### Stage 1: Remove redundant interfaces and qualify consumers

Depends on Stage 0.

- [x] Restrict recording hooks and migrate their test observers across all
  Engine, Sandbox and RoadWeaver source/test roots.
- [x] Remove implicit/repeated activation and migrate headless initialization;
  cover unactivated initialization, exclusive ownership and sequential lifetimes.
- [x] Replace tool result compatibility views and migrate MCP/console/tests;
  verify shared allocation, expiry/accounting and closed-endpoint behavior.
- [x] Update transaction, editor and tool contracts and inspect affected tests.
- [x] Pass relevant editor, structural/World, viewport, Python/MCP and project
  consumer suites under Win64-Debug-DurinEditor.
- [x] Build all production targets once at final acceptance because exported
  editor/recording interfaces, receipt layout and Launch startup cross module boundaries.
- [x] Validate changed documents/all plans, review final diff and removed-symbol
  searches, preserve receipts and commit the completed plan.

Follow [build](../../../Agents/BuildAndRun.md) and [test](../../../Agents/Testing.md)
workflows. GPU/application-hosted qualification and full UE serialization are
outside this interface/lifetime cleanup. Preserve passing results when relevant
inputs are unchanged; record any unavailable coverage explicitly.
