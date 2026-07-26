# AGENTS.md

Repository entrypoint for Codex-style agents. Read only task-relevant docs.

## Task Routing

- Do not pre-read `Documentation/` or expand every link in an index. Start from the task and the affected code.
- For repository-owned C++ changes, read `Documentation/Development/Standards/CodingStandards.md`.
- When a task needs a repository-specific contract, workflow, plan, or investigation and the owning document is not already named, use `Documentation/README.md` as the routing table. Open only the matching topic and follow further links only when required.
- Authoring and lifecycle rules come from the nearest `AGENTS.md`; do not sample unrelated documents to infer their format.
- Machine-local build overrides belong in optional `.agents/build-config.json`; create it with `Setup.bat` when needed.

## Plan Stage Continuation

- Treat completed-stage handoffs and their baseline commits as established context. Start with the current stage, prior handoff, recorded working set, and relevant diff; do not rediscover completed-stage architecture.
- Validate recorded symbols with targeted searches and initially inspect no more than five relevant code files. Expand only when code conflicts with the handoff, a required direct dependency is missing, or validation points outside the working set; state the gap and added scope first.
- End each stage with a compact handoff recording the baseline commit, working set, key symbols and decisions, open questions, and validation outcome.

## Repository Rules

- Each checkout has one source/build writer; use separate worktrees for concurrency.
- Before any configure, build, test, or runtime launch, read `Documentation/Development/Build/BuildAndRun.md` and use the repository BuildTool entrypoint described there.

## Agent Handoff

- After completing and validating a functional change, or generating/updating documentation, directly issue one command request that stages only the task files and creates one commit. Use the command execution approval as the user's authorization; do not ask separately in conversation and do not commit if approval is denied. Inspection-only, advice-only, and unchanged tasks need no request.
- Use one subject: `<type>(<scope>): <imperative summary>`, with a short lowercase scope, no trailing period, and preferably `feat`, `fix`, `refactor`, `perf`, `build`, `test`, `docs`, or `chore`. Describe the outcome, not file edits. Example: `refactor(rhi): centralize swapchain lifetime management`.
- A commit body is optional. Add one only when it preserves context that is not obvious from the subject and diff, such as design motivation, non-obvious constraints, or important tradeoffs. Keep it brief and do not turn it into a file-by-file change list.
- Do not record routine validation in the commit body. Include validation only when it was incomplete, used a non-standard procedure, exposed a known limitation, or produced a result worth preserving in history.
- When the task implements or updates an active implementation plan, end the commit body with explicit plan provenance using `Plan: Documentation/Plans/<Plan>.md` and `Stage: Stage <N>: <stage title>`. Use the exact plan path and stage heading, list multiple stages when the commit spans them, and update the plan's status/checklists in the same commit when required. If no task-relevant plan exists, do not invent a Plan or Stage reference.
- For a user-visible editor change, complete a successful full `all` build before handoff and link the verified editor executable from the same Agent Build Profile in the final response. For other changes, the final response may link an executable after a successful full build; partial builds need no link.
