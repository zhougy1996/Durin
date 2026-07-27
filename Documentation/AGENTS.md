# Documentation Rules

These instructions apply under `Documentation/`.

## Reading Policy

- Read only task-required files. Use `README.md` for navigation, then read the
  authoritative topic; discover with `rg --files Documentation` and targeted
  searches.
- Do not read archived plans unless the user names one, an active document requires its provenance, or historical reasoning is necessary.
- Do not run untargeted content searches under `Plans/Archive/`. Exclude that
  directory from general documentation searches; when archive lookup is
  justified, query plan metadata by title or filename first and open a body
  only after selecting a specific result.

## Document Boundaries

- `Development`: build, test, tooling, standards, dependency, and
  version-control workflows; keep build contracts beside their operational
  guidance.
- `Runtime`: implemented engine-runtime contracts and long-lived invariants.
- `Editor/Architecture`: implemented editor contracts and invariants.
- `Editor/Design`: visual language, tokens, layout, interaction, and themes.
- `Editor/Guides`: user-facing editor workflows.
- `Workspace`: cross-cutting workspace, project, module, and profile ownership.
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
- Add a `Last reviewed` date where required by the nearest authoring rules.
