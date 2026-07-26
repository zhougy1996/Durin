# Documentation Rules

These instructions apply under `Documentation/`.

## Reading Policy

- Read only the files required by the current task.
- Use `README.md` files as compact navigation, not as substitutes for topic documentation.
- Discover files with `rg --files Documentation` and targeted content searches.
- Do not read archived plans unless the user names one, an active document requires its provenance, or historical reasoning is necessary.

## Document Boundaries

- `Development` contains build, test, tooling, standards, dependency, and
  version-control workflows. Build-system contracts belong beside the
  operational guidance they govern.
- `Runtime` contains currently implemented engine-runtime contracts and
  long-lived invariants, organized by subsystem.
- `Editor/Architecture` contains currently implemented editor contracts and
  long-lived invariants.
- `Editor/Design` contains editor visual language, design tokens, layout,
  interaction, and theme conventions.
- `Editor/Guides` contains user-facing editor workflows.
- `Workspace` contains workspace, project, module, and profile ownership
  boundaries that span development and runtime concerns.
- `Plans` contains selected implementation paths, stages, and acceptance gates.
- `Plans/Archive` preserves completed implementation decisions and validation evidence.
- `Investigations` contains verified unresolved problems before an
  implementation path has been selected.

Do not place implementation stages, open decisions, or future roadmaps in
`Runtime`, `Editor/Architecture`, `Editor/Design`, or `Workspace`. After
implementation, move lasting contracts into the owning domain. Keep informal
research under the ignored `Documentation/Local/` directory or outside the
repository.

## Maintenance

- Prefer links to the authoritative topic document over duplicated guidance.
- Update direct links when a document moves.
- Keep navigation indexes scoped to their active directory; do not create a master file-by-file catalog.
- Add a `Last reviewed` date where required by the nearest authoring rules.
