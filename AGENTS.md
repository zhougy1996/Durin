# AGENTS.md

Repository entrypoint for agents. Read task-relevant guidance once; reread only
when it changes or needed context is unavailable.

## Task Routing

- Start from the task and affected code; use `Documentation/README.md` only to
  route to the matching repository guidance.
- If module ownership is unclear, read `Documentation/Workspace/CodeModules.md`
  and search only the smallest plausible module set.

## Repository Rules

- Use `.\DevTool.bat` on Windows or `./DevTool` on macOS/Linux; translate
  documented commands to the host launcher.
- Each checkout has one source/build writer; use separate worktrees for concurrency.
- Run Git operations with the checkout's absolute path supplied command-locally,
  for example `git -c safe.directory=<absolute-checkout-path> status`; do not
  modify the user's global `safe.directory` configuration.
- Configure, build, run, or recovery: read `Documentation/Agents/BuildAndRun.md` first.
- Native-test selection or execution: read `Documentation/Agents/Testing.md` first.
- Documentation maintenance: read `Documentation/Agents/Documentation.md` first.
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
  add exact `Plan` and `Stage` provenance using `git commit --trailer`; use
  `Documentation/Plans/<Plan>.md` and `Stage <N>: <stage title>` as their values
  and do not invent provenance.
