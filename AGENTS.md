# AGENTS.md

Repository entrypoint for Codex-style agents. Read only task-relevant docs.

## Task Routing

- Start from the task and affected code. When an unnamed repository contract,
  workflow, plan, or investigation is needed, route through
  `Documentation/README.md`; open only the matching topic and required links.
- When the affected module is unknown, read
  `Documentation/Workspace/CodeModules.md`, select the smallest plausible module
  set, then search only those roots. Use `Engine/Engine.dproject` for the
  authoritative module-to-directory mapping and open only the selected
  `.dmodule` files when dependency direction matters.
- When asked to select or continue a bounded repository task, run
  `.\DevTool.bat doc task list`, then open only the selected task. Task files
  are live work items: complete their acceptance criteria, then delete the file
  in the implementation commit instead of archiving it.
- Read `Documentation/Development/Standards/CodingStandards.md` only when the
  user explicitly requests refactoring repository-owned C++.
- Follow the nearest `AGENTS.md` for authoring and lifecycle rules; do not infer
  formats from unrelated documents.
- Machine-local build overrides belong in optional `.agents/DevTool.user.json`;
  create it with `.\DevTool.bat setup` when needed.

## Plan Stage Continuation

- Treat completed-stage handoffs and their baseline commits as established context. Start with the current stage, prior handoff, recorded working set, and relevant diff; do not rediscover completed-stage architecture.
- Validate recorded symbols with targeted searches and initially inspect no more than five relevant code files. Expand only when code conflicts with the handoff, a required direct dependency is missing, or validation points outside the working set; state the gap and added scope first.
- End each stage with a compact handoff recording the baseline commit, working set, key symbols and decisions, open questions, and validation outcome.

## Repository Rules

- Each checkout has one source/build writer; use separate worktrees for concurrency.
- Use DurinDevTool for routine configure, build, test, and runtime validation;
  the execution rules below are sufficient for ordinary task validation. Read
  `Documentation/Development/Build/BuildAndRun.md` only when changing those
  workflows, selecting non-routine validation, or diagnosing a build, test, or
  runtime-operation problem.
- For routine changes, validate with the smallest affected native test target
  or targets: `.\DevTool.bat test --target <target>`. Do not run
  `.\DevTool.bat test --target all` by default. Full native-test validation is
  reserved for an explicit user or plan gate, changes that cross test targets
  or alter shared runtime/test infrastructure, or concrete evidence that the
  affected scope cannot be covered reliably by targeted validation; state the
  reason before starting it.
- When full native-test validation is required, use the default target
  granularity: `.\DevTool.bat test --target all`. Do not run the unfiltered
  combination `--target all --granularity case`; diagnose aggregate failures with
  `--target <failed-target> --filter <suite.case>`. Case granularity is only for
  a narrow case-name `--ctest-regex` or an explicit isolation-qualification
  gate. After diagnosis, return to default target granularity for the final
  full-suite validation unless the task specifically changes case isolation.
- Treat configure, build, rebuild, and test operations as long-running tasks,
  including DurinDevTool actions, scripts, wrappers, and other commands that may
  invoke them transitively. Set the execution tool's timeout explicitly to at
  least 10 minutes (`timeout_ms: 600000`), and use at least one hour
  (`timeout_ms: 3600000`) for a full `all` build or rebuild. Do not rely on the
  tool's default timeout or assume that an approved command prefix supplies one.
- Do not run the `Win64-Release-DurinEditor-Tests` or
  `Win64-Shipping-DurinGame-Tests` presets as routine Agent validation. They are
  opt-in configuration-parity entrypoints for an explicit user or plan gate, or
  for a change that directly modifies cross-configuration assertion, build, or
  conditional-compilation behavior. When one is required, use a single selected
  checkout; do not duplicate its build and install trees across worktrees.
- If the execution tool explicitly yields a running process or cell ID, wait on
  that same invocation in intervals no longer than 60 seconds. Once it returns a
  final result, stop waiting and act on that result. DurinDevTool prints a
  30-second heartbeat while a child command is alive; a yield, quiet output, or
  elapsed UI window alone is not an interruption and must not trigger a second
  build, rebuild, or recovery-state inspection.
- Do not start another build while an earlier CMake, Ninja, compiler, or linker
  process tree may still be running. Run the recovery command reported by
  `DevTool status` only after an operation was cancelled, externally
  terminated, or lost its controlling DurinDevTool process, the old process tree
  has exited, and `DevTool status` reports a recovery state other than
  `clean`. A `recover required` state resumes the interrupted target
  incrementally; `rebuild required` falls back to `rebuild --target all` when
  the prior state cannot be recovered safely.
  Ordinary compiler, linker,
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
