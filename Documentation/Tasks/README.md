# Open Engineering Tasks

- [Harden and simplify DurinDevTool code quality](DurinDevToolCodeQuality.md) —
  make child-output failures fail promptly, unify direct and interactive command
  execution, and remove duplicated fallback paths without weakening recovery or
  worktree safety.
- [Simplify DurinHeaderTool code quality](DHTCodeQuality.md) — remove unsafe
  parser fallbacks and unused abstraction without weakening cache recovery,
  generated-output publication, or partial-translation-unit support.

This index lists open, bounded implementation tasks only. A task disappears
from this directory and index in the commit that completes its acceptance
criteria. Authoring and lifecycle rules are in `AGENTS.md`.
