# AGENTS.md

Repository entrypoint for agents. Read task-relevant guidance once; reread only
when it changes or needed context is unavailable.

## Task Routing

- Start from the task and affected code. If guidance is unknown, search matching
  rows in `Documentation/README.md` or run `doc find "<task terms>" --limit 5`
  through the host DevTool launcher; do not print the full routing table.
- If module ownership is unclear, search matching rows in
  `Documentation/Workspace/CodeModules.md` and search only those source roots.
- For reference documents, locate headings or symbols before reading the needed
  sections. Keep reads bounded; do not concatenate long documents. If output is
  truncated, narrow the query or range instead of repeating the full read.
  Read applicable `AGENTS.md` files and required agent workflows in full; follow
  contract dependencies when the selected section does not resolve the task.
- When changing a shared API, search its symbols across the source and test
  roots of every project declared in `Durin.dworkspace`; migrate all consumers.
  Validate affected project targets as well as the owning module. For shared
  Engine API migrations, complete an `all` build before handoff.

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
