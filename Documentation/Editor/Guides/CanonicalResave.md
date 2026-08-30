# Canonical Resave

Summary: Canonicalize reflected identities without reimporting source data.

Last reviewed: 2026-08-27

Use canonical resave when the Asset Compatibility window or a package context
menu says **Resave recommended**. This is maintenance of serialized type names;
it is separate from unsaved authored changes and reimport.

For one asset, open its Content Browser context menu and choose **Resave
Package**. **Save Package** is reserved for loaded assets with ordinary authored
changes. For a multi-selection, choose **Resave Selected Packages**.

For project maintenance, open **Tools > Asset Maintenance > Canonical Resave**,
run the read-only audit, review stored/current identities and blockers, then
apply the recommended set. The apply is a sequence of bounded atomic package
units, so cancellation or failure can leave earlier packages complete; the
terminal report is the authority for the outcome.

DurinDevTool uses the configured game project by default and previews without
writing:

```powershell
.\DevTool.bat asset resave /Game
.\DevTool.bat asset resave /Game/Example --apply
.\DevTool.bat asset resave --all
```

Each positional scope selects an exact package when one exists and every package
below that path. Use `--all` instead of scopes for the complete project, and
`--project <descriptor>` only to override the configured default. Add `--json`
for automation. The lower-level host accepts the corresponding
`DurinAssetTool resave --project=<project.dproject> <scope>...` grammar.

Canonical resave always writes DURF/DAST v7; no format-selection or rollback
option exists. `--apply` is the only option that authorizes writes.
Before apply, check out the reported authored files in source control. After
apply, review the package diffs and rerun the same dry-run; a successful second
scan is empty and a second apply is a no-op.

Blocked packages are never written. Typical blockers are a dirty loaded
package, read-only mount, non-current package format, stale
fingerprint, incompatible or unknown payload, unavailable reflected type, or
corrupt bytes; non-asset entries such as redirectors are skipped. For uncooked asset families, apply also waits for the PostLoad
recovery started by the ordinary loader; missing source/DDC data or a provider
that does not publish family-ready transient state blocks the save rather than
serializing a partially recovered object. Resolve the named condition and
create a fresh plan; do not invoke an authored reimport merely to canonicalize
reflected identities.
