# Editor Architecture Documentation Rules

These instructions apply under `Documentation/Editor/Architecture/` in addition
to the repository documentation rules.

## Domain Ownership

Organize implemented editor contracts by architectural responsibility, using
`Modules:` to describe source ownership. Place topic documents in these domains
rather than at the Architecture root:

- `Framework`: editor engine activation, shared subsystem/service lifetime,
  workspaces, documents, panels, commands, and host integration.
- `Assets`: shared import, asset operations and compensation, thumbnails,
  and Content Browser presentation and mutation orchestration.
- `PropertyEditing`: shared reflected edit targets, validation, notifications,
  edit sessions, Details views/customizations, and reflected function actions.
  Concrete Level Editor inspection policy belongs in `LevelEditing`.
- `Transactions`: transaction records, collector ownership, recording, history,
  Undo/Redo, replay failure, and package checkpoint contracts.
- `Tools`: editor Toolset discovery/invocation and Console, MCP, and Python
  adapters, including their execution, cancellation, and session lifetimes.
- `LevelEditing`: scene viewport interaction, selection, gizmos, Actor/Component
  Details, level mutation commands, PIE sessions, rendering diagnostics, and
  integrated cloud-authoring presentation and viewport policy.
- `Materials`: Material Editor graph operations, canvas behavior, previews,
  working-copy lifetime, compilation controls, Apply/Save, and instance editing.

Keep cross-module feature contracts together under their primary responsibility
and link to shared mechanisms rather than duplicating them. Runtime authority
and rendering algorithms belong under `Runtime`; visual language and interaction
design belong under `Editor/Design`; user walkthroughs belong under
`Editor/Guides`. Technical presentation and lifetime contracts remain here.
