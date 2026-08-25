# Canonical Resave

Summary: Canonicalize reflected identities without reimporting source data.

Last reviewed: 2026-08-25

Use canonical resave when the Asset Compatibility window or a package context
menu says **Resave recommended**. This is maintenance of serialized type names;
it is separate from unsaved authored changes and reimport.

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
DurinAssetTool --project=<project.dproject> --operation=canonical-resave --project-scope --target=v6
DurinAssetTool --project=<project.dproject> --operation=canonical-resave --project-scope --ci
```

Selection must name packages, folders, mounts, or the explicit project scope.
Canonical resave always writes DURF/DAST v6. `--target=v6` is accepted as an explicit
spelling of the only supported target; no rollback or legacy-format target exists.
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
