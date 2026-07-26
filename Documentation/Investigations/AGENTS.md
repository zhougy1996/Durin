# AGENTS.md

These instructions apply to `Documentation/Investigations/`.

## Purpose

- Preserve verified unresolved engineering problems across tasks. Describe
  current behavior, impact, evidence, and candidate direction—not an
  implementation plan or adopted architecture.
- Once scope, decisions, stages, and gates are selected, use
  `Documentation/Plans/`. After implementation stabilizes, move lasting
  constraints to the owning documentation domain.

## Required Content

- Give every issue a status and `Last reviewed` date; separate verified
  findings from risks, assumptions, and recommendations.
- Link to the relevant architecture documentation and source files.
- Rank independent findings by priority when an issue contains more than one problem.
- Describe observable impact and supporting evidence; avoid vague requests to
  "improve" or "optimize."
- Include validation gaps or reproduction guidance when the behavior is not already covered by tests.

## Maintenance

- Update `README.md` when an issue is added, renamed, resolved, or removed.
- When work begins, link the issue to its implementation plan or change. Do not duplicate the plan's stage checklist here.
- Resolve only after the change and validation land: record the resolving commit
  or replacement document, transfer lasting rules, then remove the issue and
  open-index entry. Git and archived plans preserve history; create no
  Investigations archive or resolved index.
- Keep filenames descriptive and topic-based; do not encode transient priority or issue numbers in filenames.
