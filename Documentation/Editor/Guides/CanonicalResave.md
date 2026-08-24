# Canonical Resave

Summary: Canonicalize reflected identities or explicitly migrate package format without reimporting source data.

Last reviewed: 2026-08-24

Use canonical resave when the Asset Compatibility window or a package context
menu says **Resave recommended**. This is maintenance of serialized type names;
it is separate from unsaved authored changes and reimport. The same bounded
plan/apply machinery also accepts an explicit package-format target.

For one asset, open its Content Browser context menu and choose **Resave
Package**. **Save Package** is reserved for loaded assets with ordinary authored
changes. For a multi-selection, choose **Resave Selected Packages**. A managed
import output also offers **Resave Import Record**, which rewrites only its
companion record and does not read source files or rebuild outputs.

For project maintenance, open **Tools > Asset Maintenance > Canonical Resave**,
run the read-only audit, review stored/current identities and blockers, then
apply the recommended set. The apply is a sequence of bounded atomic package
units, so cancellation or failure can leave earlier packages complete; the
terminal report is the authority for the outcome.

The command-line host is dry-run by default:

```text
DurinAssetTool --project=<project.dproject> --operation=canonical-resave --mount=/Game --format=human
DurinAssetTool --project=<project.dproject> --operation=canonical-resave --package=/Game/Example --apply
DurinAssetTool --project=<project.dproject> --operation=canonical-resave --project-scope --target=v5
DurinAssetTool --project=<project.dproject> --operation=canonical-resave --project-scope --ci
```

Selection must name packages, folders, mounts, or the explicit project scope.
The default `--target=v4` is canonical rollback; `--target=v5` is the explicit
trailer migration route. Redirectors remain at their existing supported format.
Before apply, check out the reported authored files in source control. After
apply, review the package diffs and rerun the same dry-run; a successful second
scan is empty and a second apply is a no-op.

Blocked packages are never written. Typical blockers are a Dirty loaded
package, read-only mount, redirector, non-current package format, stale
fingerprint, incompatible or unknown payload, unavailable reflected type, or
corrupt bytes. Resolve the named condition and create a fresh plan; do not use
reimport merely to canonicalize reflected identities.
