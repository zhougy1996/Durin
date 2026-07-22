# AGENTS.md

These instructions apply to `Documentation/Issues/`.

## Purpose

- Record verified, unresolved engineering problems that are important enough to preserve across tasks.
- Keep issue documents diagnostic and evidence-based. They describe the current behavior, impact, and candidate direction; they are not implementation plans or adopted architecture.
- Use `Documentation/Plans/` after scope, decisions, stages, and acceptance gates have been selected. Move lasting constraints to `Documentation/Architecture/` after implementation stabilizes.

## Required Content

- Give every issue a clear status and a `Last reviewed` date.
- Separate verified findings from risks, assumptions, and recommendations.
- Link to the relevant architecture documentation and source files.
- Rank independent findings by priority when an issue contains more than one problem.
- Describe observable impact and the evidence that supports each finding. Avoid vague requests to "improve" or "optimize."
- Include validation gaps or reproduction guidance when the behavior is not already covered by tests.

## Maintenance

- Update `README.md` whenever an issue document is added, renamed, resolved, or removed.
- When work begins, link the issue to its implementation plan or change. Do not duplicate the plan's stage checklist here.
- Mark an issue resolved only after the change and its validation have landed. Record the resolving commit or replacement document before removing it from the open index.
- Keep filenames descriptive and topic-based; do not encode transient priority or issue numbers in filenames.
