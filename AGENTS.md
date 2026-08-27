# AGENTS.md

Repository entrypoint for Codex-style agents. Read only task-relevant docs.

## Task Routing

- Start from the task and affected code; use `Documentation/README.md` only to
  route to the matching repository guidance.
- If module ownership is unclear, read `Documentation/Workspace/CodeModules.md`
  and search only the smallest plausible module set.

## Repository Rules

- Each checkout has one source/build writer; use separate worktrees for concurrency.
- Run Git operations with the checkout's absolute path supplied command-locally,
  for example `git -c safe.directory=<absolute-checkout-path> status`; do not
  modify the user's global `safe.directory` configuration.
- Before configuring, building, rebuilding, running, or recovering repository
  targets, read `Documentation/Agents/BuildAndRun.md`.
- Before selecting or running native tests, read
  `Documentation/Agents/Testing.md`.
- Before creating, moving, removing, validating, completing, or archiving
  repository documentation, read `Documentation/Agents/Documentation.md`.
- Do not start another build while an earlier CMake, Ninja, compiler, or linker
  process tree may still be running.

## Agent Handoff

- After successful validation, stage and commit the task's changes unless the
  user explicitly requests an uncommitted handoff or the commit cannot be
  isolated from existing changes; request elevated permission if Git metadata
  writes are blocked.
- Use `<type>(<scope>): <imperative summary>` with a short lowercase scope and
  no trailing period; describe the outcome, and add a body only for non-obvious
  motivation, tradeoffs, or incomplete/non-standard validation.
- For active-plan work, update required status/checklists in the same commit and
  end the body with exact `Plan: Documentation/Plans/<Plan>.md` and `Stage:
  Stage <N>: <stage title>` provenance; do not invent provenance.
