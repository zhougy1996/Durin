# Documentation Rules

These instructions apply under `Documentation/`.

## Reading Policy

- Read only the files required by the current task.
- Use `README.md` files as compact navigation, not as substitutes for topic documentation.
- Discover files with `rg --files Documentation` and targeted content searches.
- Do not read archived plans unless the user names one, an active document requires its provenance, or historical reasoning is necessary.

## Document Boundaries

- `Setup` contains operational build, test, IDE, dependency, and local workflow instructions.
- `Architecture` describes behavior that is currently implemented and must remain true.
- `Editor` contains user-facing editor workflows.
- `Git` contains repository and content version-control workflows.
- `Plans` contains selected implementation paths, stages, and acceptance gates.
- `Plans/Archive` preserves completed implementation decisions and validation evidence.
- `Issues` contains verified unresolved problems before an implementation path has been selected.

Do not place implementation stages, open decisions, or future roadmaps in Architecture. Move lasting contracts into Architecture after implementation, and keep informal research under the ignored `Documentation/Local/` directory or outside the repository.

## Maintenance

- Prefer links to the authoritative topic document over duplicated guidance.
- Update direct links when a document moves.
- Keep navigation indexes scoped to their active directory; do not create a master file-by-file catalog.
- Add a `Last reviewed` date where required by the nearest authoring rules.
