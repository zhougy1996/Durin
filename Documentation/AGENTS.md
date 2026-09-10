# Documentation Rules

These instructions apply under `Documentation/`.

## Authoring Language

Write new or revised prose in English; preserve literal identifiers, commands,
paths, and quoted source text.

## Reading Policy

- Read only task-required files. When the owning document is unknown, follow
  the discovery and targeted-search policy in `README.md`.
- Exclude `Plans/Archive/` from general searches. Read archived plans only for
  named requests, required provenance, or historical reasoning; select by title
  or filename metadata before opening a body.

## Document Boundaries

- `Agents`: minimal operational decisions with links to detailed contracts and
  exceptional workflows.
- `Development`: build, test, tooling, standards, dependency, and version-control
  workflows and contracts.
- `Runtime`: implemented engine-runtime contracts and long-lived invariants.
- `Editor/Architecture`: implemented editor contracts and invariants.
- `Editor/Design`: visual language, tokens, layout, interaction, and themes.
- `Editor/Guides`: user-facing editor workflows.
- `Workspace`: cross-cutting workspace, project, module, and profile ownership.
- `Tasks`: bounded implementation work; their local rules own completion and
  deletion.
- `Roadmaps`: cross-plan outcomes, milestones, dependencies, and plan boundaries;
  child plans own implementation checklists, handoffs, and acceptance gates.
- `Plans`: selected paths, stages, and acceptance gates; `Plans/Archive`
  preserves completed decisions and evidence.
- `Investigations`: verified unresolved problems without a selected path.

Keep stages, open decisions, and roadmaps out of contract domains. After
implementation, move lasting contracts to their owning domain. Keep informal
research in ignored `Documentation/Local/` or outside the repository.

## Maintenance

- Prefer links to the authoritative topic document over duplicated guidance.
- Update direct links when documents move. Keep indexes local; do not create a
  master file catalog.
- Active and completed documents require valid local links. Archive audits
  treat targets removed by later evolution as warnings; archive operations must
  introduce no diagnostics or leave errors. Use code-formatted paths for
  historical source locations that should not track the current tree.
- Start implemented Runtime and Editor contract documents with a concise
  `Summary:` line. Add a comma-separated `Modules:` line when source ownership
  is bounded, using registered module names; omit it for code-agnostic or
  cross-repository guidance.
- Add a `Last reviewed` date where required by the nearest authoring rules.

### Growth Reviews

Apply these checks to long-lived `Development`, `Runtime`, `Editor`, and
`Workspace` documents. Plans, roadmaps, tasks, investigations, and archives
follow their own lifecycle rules.

- Before expanding a document, check existing domain authority and link rather
  than duplicate it; retain only necessary local context.
- At 500 lines, review ownership before substantial expansion. Split independently
  maintainable concerns, but keep cohesive contracts intact regardless of size.
- After splitting, link the owning documents and repair references. Add a
  `Documentation/README.md` entry only for a distinct task trigger.
