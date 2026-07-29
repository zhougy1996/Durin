# Share DurinDevTool Worktree Link Preparation

## Outcome

Agent and VS Code worktree directories use one parameterized
preserve-and-link implementation while retaining their existing labels, paths,
and safety behavior.

This is a bounded deduplication task, not a worktree lifecycle redesign.
Complete the required changes and validation as one outcome, then delete this
file in the implementation commit.

## Evidence

`worktree/services.py::prepare_agent_link` and `prepare_vscode_link` implement
the same preserve-to-`.pre-link-backup` and link operation with different
labels and relative paths. Maintaining parallel implementations risks drift in
backup conflicts, dry-run output, and idempotency.

## Required Changes

1. Extract one parameterized preserve-and-link helper used by both Agent and
   VS Code preparation.
2. Keep caller-specific labels and relative paths at the small public
   boundaries.
3. Preserve the existing conflict, backup, idempotency, and dry-run branches.

## Protected Invariants

- Preserve non-empty local `.agents` and `.vscode` directories under their
  `.pre-link-backup` names.
- Preserve idempotent preparation, dry-run output, backup-conflict handling,
  and safe junction handling during worktree removal.
- Do not change worktree command syntax or the Windows checkout-lock recovery
  boundary.

## Likely Working Set

- `Tools/DurinDevTool/durin_dev_tool/worktree/services.py`
- `Tools/DurinDevTool/tests/test_bootstrap_worktree.py`

Expand this set only for a direct dependency or a required lasting contract
update.

## Acceptance

- Agent and VS Code preparation call one implementation with their respective
  labels and paths.
- Existing dry-run, backup-conflict, idempotency, preservation, link, and
  removal tests continue to pass.
- Run the complete DurinDevTool Python suite:

  ```powershell
  .\.venv\Scripts\python.exe -m pytest Tools\DurinDevTool\tests
  ```

- Validate changed documentation, inspect the final status and diff, then
  delete this task file in the implementation commit. Do not archive it or add
  Plan provenance.
