# Unify DurinDevTool Document Transactions

## Outcome

Plan archival and ordinary Markdown changes use one fingerprint-checked
transaction model with identical precondition, rollback, and validation
semantics.

This is a bounded transaction-consolidation task, not a documentation workflow
redesign. Complete the required changes and validation as one outcome, then
delete this file in the implementation commit.

## Evidence

`documentation/archive.py::apply_archive` owns a separate snapshot, write,
delete, validation, and rollback transaction even though
`documentation/changes.py` already owns fingerprint-checked
`DocumentChangeSet` application for Markdown moves and rewrites. The two paths
have diverged precondition and rollback semantics.

## Required Changes

1. Express plan archival through the existing document change transaction, or
   extract one shared transaction primitive used by both archive and ordinary
   document changes.
2. Preserve archive preview data and post-apply catalog validation.
3. Use the shared fingerprint, write, delete, and rollback behavior rather than
   retaining an archive-specific fallback transaction.
4. Extend focused tests for write failure, stale preconditions, and
   post-validation rollback where needed.

## Protected Invariants

- Preserve atomic Markdown updates, reference repair, and rollback after
  validation failure.
- Preserve archive previews and the rule that archival changes completed plan
  status from completed to archived.
- Preserve existing CLI syntax and dry-run behavior.

## Likely Working Set

- `Tools/DurinDevTool/durin_dev_tool/documentation/archive.py`
- `Tools/DurinDevTool/durin_dev_tool/documentation/changes.py`
- `Tools/DurinDevTool/tests/test_documentation_domain.py`

Expand this set only for a direct dependency or a required lasting contract
update.

## Acceptance

- Plan archival and ordinary document changes share transaction semantics.
- Existing write-failure and post-validation rollback tests continue to pass,
  with stale inputs rejected before partial application.
- Run the complete DurinDevTool Python suite:

  ```powershell
  .\.venv\Scripts\python.exe -m pytest Tools\DurinDevTool\tests
  ```

- Validate changed documentation, inspect the final status and diff, then
  delete this task file in the implementation commit. Do not archive it or add
  Plan provenance.
