# AGENTS.md

Repository entrypoint for Codex-style agents. Read only task-relevant docs.

## Task Routing

- Start from the task and affected code. When an unnamed repository contract,
  workflow, plan, or investigation is needed, route through
  `Documentation/README.md`; open only the matching topic and required links.
- Read `Documentation/Development/Standards/CodingStandards.md` only when the
  user explicitly requests refactoring repository-owned C++.
- Follow the nearest `AGENTS.md` for authoring and lifecycle rules; do not infer
  formats from unrelated documents.
- Machine-local build overrides belong in optional `.agents/build-config.json`; create it with `Setup.bat` when needed.

## Plan Stage Continuation

- Treat completed-stage handoffs and their baseline commits as established context. Start with the current stage, prior handoff, recorded working set, and relevant diff; do not rediscover completed-stage architecture.
- Validate recorded symbols with targeted searches and initially inspect no more than five relevant code files. Expand only when code conflicts with the handoff, a required direct dependency is missing, or validation points outside the working set; state the gap and added scope first.
- End each stage with a compact handoff recording the baseline commit, working set, key symbols and decisions, open questions, and validation outcome.

## Repository Rules

- Each checkout has one source/build writer; use separate worktrees for concurrency.
- Before any configure, build, test, or runtime launch, read `Documentation/Development/Build/BuildAndRun.md` and use the repository BuildTool entrypoint described there.
- Treat configure, build, rebuild, and test operations as long-running tasks,
  including BuildTool actions, scripts, wrappers, and other commands that may
  invoke them transitively. Set the execution tool's timeout explicitly to at
  least 10 minutes (`timeout_ms: 600000`), and use at least one hour
  (`timeout_ms: 3600000`) for a full `all` build or rebuild. Do not rely on the
  tool's default timeout or assume that an approved command prefix supplies one.
- If the execution tool explicitly yields a running process or cell ID, wait on
  that same invocation in intervals no longer than 60 seconds. Once it returns a
  final result, stop waiting and act on that result. BuildTool prints a
  30-second heartbeat while a child command is alive; a yield, quiet output, or
  elapsed UI window alone is not an interruption and must not trigger a second
  build, rebuild, or recovery-state inspection.
- Do not start another build while an earlier CMake, Ninja, compiler, or linker
  process tree may still be running. Run the recovery command reported by
  `BuildTool status` only after an operation was cancelled, externally
  terminated, or lost its controlling BuildTool process, the old process tree
  has exited, and `BuildTool status` reports
  `Recovery state: rebuild required`. The command normally rebuilds the
  interrupted target and falls back to `all` when the prior target cannot be
  recovered safely. Ordinary compiler, linker,
  configuration, clean, assertion, test-timeout, test-process, and runtime
  failures do not require rebuild; fix the cause and rerun the same command.

## Agent Handoff

- After validating a functional change or generating/updating documentation,
  stage only the task files and create one local commit. An explicit user request
  to implement or update repository files authorizes this bounded staging and
  commit operation; do not request separate approval. Before committing, inspect
  the status and diff, preserve unrelated changes, and stop for clarification if
  task ownership is ambiguous or a task file contains overlapping user edits.
  Do not commit when the user declines, asks to leave changes uncommitted, or the
  task is inspection-only, advice-only, or unchanged.
- Subject: `<type>(<scope>): <imperative summary>`. Use a short lowercase scope,
  no trailing period, and preferably `feat`, `fix`, `refactor`, `perf`, `build`,
  `test`, `docs`, or `chore`; describe the outcome, not file edits.
- Add a brief body only for non-obvious motivation, constraints, or tradeoffs.
  Mention validation only when incomplete, non-standard, limited, or historically
  noteworthy.
- For active-plan work, update required status/checklists in the same commit and
  end the body with the exact `Plan: Documentation/Plans/<Plan>.md` and
  `Stage: Stage <N>: <stage title>` provenance (one Stage line per stage). Do not
  invent provenance when no relevant plan exists.
- For a user-visible editor change, complete a successful full `all` build before handoff and link the verified editor executable from the same Agent Build Profile in the final response. For other changes, the final response may link an executable after a successful full build; partial builds need no link.
