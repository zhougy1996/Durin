# Git Workflow

## Branch responsibilities

| Branch | Purpose | Integration |
| --- | --- | --- |
| `main` | Stable, releasable code; no direct development | Receive release PRs from `dev` |
| `dev` | Long-lived daily development and integration | Receive small changes and completed features |
| `feature/*` | Temporary branches for large or experimental work | Start from `dev`, merge back, then delete |

Each checkout has one source/build writer. Use the
[worktree workflow](../Build/BuildAndRun.md#windows-workflow) for concurrent work.
Run Git with the checkout's absolute path in command-local `safe.directory`, as
required by the [repository instructions](../../../AGENTS.md).

## Development and integration

Small fixes may be committed directly on `dev`. For a large feature, update
`dev`, create a feature branch from it, and push the branch with an upstream
for backup and draft PR review. Stage only the intended files and follow the
repository commit-message convention.

Merge the feature into `dev` through a PR or a local merge. Once integrated,
delete its local and remote branches. Keep `dev` for subsequent development.
For a new repository that has only `main`, create `dev` from the updated
`main` and publish it with an upstream before starting development.

## Release and synchronization

When `dev` is stable, open a PR from `dev` to `main`. After that PR merges:

1. Update local `main` from its remote.
2. Switch to `dev` and merge `main` into it.
3. Push the synchronized `dev`.

Do not rebase the long-lived branches after release merges or rebase `main`
onto `dev`: rewriting their shared history changes commit identities and
complicates subsequent integration.
