# Editor Contract Cleanup Plan

Summary: Enforce complete transactor implementations and consolidate editor test bootstrap support.

Last reviewed: 2026-10-19

Status: Archived
Completed: 2026-10-19

## Current Status

Implementation and qualification are complete. All 37 transactor
virtual capabilities are mandatory, with no unsupported/no-op fallback. Listener
infrastructure and convenience forwarding remain. The redundant command header
and cursor-count API are removed. Seven startup-only reflected editor subclasses
are replaced with real editors, and one shared test accessor owns bootstrap and
World/PIE/input probes. The specialized World lifecycle fixture remains.

The baseline is 2acf289c0 plus Stage 1 changes. The affected-test explanation
expands shared fixture headers to all Engine tests; named CPU targets cover the
actual editor/transaction consumers. The final all build checks cross-module
virtual-interface and removed-header integration. DTransactor reflection already
omits its constructor, so C++ abstractness needs no reflection changes.

On Win64-Debug-DurinEditor, all production targets built successfully and 17 named
CPU test targets passed 985 cases. Two ContentBrowserWorkflowTests cases skipped
because Windows file/directory symlink privileges are unavailable. Relevant
editor activation, history, World/PIE, material and Python/MCP behavior passed.
Compile-time assertions require DTransactor to be abstract and DTransBuffer to
remain concrete. The production all receipt is
20261019-131338-065456-22424-cmake.log; it remains applicable after a test-only
MCPPythonJobTests startup correction, which has its own successful final receipt.
Per-target receipts and the manifest are preserved locally under
Build/EditorContractCleanupValidation/receipts.md. Only documentation and receipt
bookkeeping changed after final qualification.

## Selected Decisions

- Keep DTransactor/DTransBuffer and require every virtual transaction capability
  through pure virtual declarations. Retain implemented listener infrastructure
  and convenience forwarding; remove unsupported fallback results.
- Keep ApplyEditorChange's sole declaration in Transactor.h, which needs it for
  friendship, and remove the redundant EditorCommand.h forwarding header.
- Remove GetUndoableCount and use queue length minus undone count in tests.
- Share one FEditorEngineTestAccess definition and explicit headless startup
  helper across native editor consumers. Use real DEditorEngine objects instead
  of reflected subclasses that only expose bootstrap. Keep specialized lifecycle
  probes where they exercise additional behavior.
- Preserve nullable transactor behavior and production lifecycle policy.

## Implementation Stages

### Stage 0: Record constraints and acceptance

- [x] Inspect abstract reflection construction, consumers and test bootstrap.
- [x] Record and validate the selected scope before implementation.

### Stage 1: Enforce interfaces and consolidate test support

- [x] Require concrete transaction implementations and remove fallback stubs.
- [x] Migrate redundant header/count consumers in Engine, Sandbox and RoadWeaver.
- [x] Consolidate test access/bootstrap and retain lifecycle boundary coverage.
- [x] Update contracts and add compile-time abstract/concrete assertions.
- [x] Inspect affected-test selection and pass relevant editor, World, viewport,
  material, Python/MCP and RoadWeaver consumer tests on Win64-Debug-DurinEditor.
- [x] Pass all production targets once: exported virtual interfaces change across
  modules, and bounded tests cannot prove every production consumer still links.
- [x] Validate changed documents/all plans, review the diff and commit.

Follow the [build](../../../Agents/BuildAndRun.md) and [test](../../../Agents/Testing.md)
workflows. Preserve final-code receipts locally under ignored Build/. GPU and
application-hosted behavior is unchanged and requires no execution here.
