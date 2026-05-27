# Git Workflow (main / dev / feature)

## 1. Branch Structure

```text
main      Stable / releasable branch
dev       Long-term development branch
feature/* Temporary feature branches
```

---

# 2. Branch Responsibilities

## main

The stable branch.

Use it for:

* stable builds
* releases
* production-ready code

Rules:

* avoid direct development on `main`
* receive changes through Pull Requests

---

## dev

The long-term development branch.

Use it for:

* daily development
* integrating new systems/features
* testing unfinished work

Rules:

* keep it alive permanently
* continue development after each PR

---

## feature/*

Temporary branches for large features.

Use them for:

* renderer refactors
* AI systems
* editor overhauls
* experimental systems

Rules:

* create from `dev`
* merge back into `dev`
* delete after completion

---

# 3. Initial Setup

Starting from a repository with only `main`:

```bash
git checkout main
git pull

git checkout -b dev
git push -u origin dev
```

---

# 4. Small Features / Small Fixes

For small changes:

* bug fixes
* editor tweaks
* utility tools

Work directly on `dev`:

```bash
git checkout dev
```

Commit normally:

```bash
git add .
git commit -m "message"
git push
```

---

# 5. Large Feature Development

## Create Feature Branch

Always create from `dev`:

```bash
git checkout dev
git pull

git checkout -b feature/render-graph
```

Why:

`dev` contains the latest development state.

---

## Development

Commit normally:

```bash
git add .
git commit -m "message"
```

---

## Push Feature Branch (Recommended)

For long-running features:

```bash
git push -u origin feature/render-graph
```

Benefits:

* backup
* multi-device work
* Draft PR support
* safer experimentation

---

# 6. Merge Feature into dev

Create a PR:

```text
feature/render-graph -> dev
```

Or merge locally.

After merging:

```bash
git branch -d feature/render-graph
git push origin --delete feature/render-graph
```

---

# 7. Release dev into main

When `dev` becomes stable:

```text
dev -> main
```

Create a Pull Request on GitHub.

---

# 8. IMPORTANT: After PR Merge

After merging `dev` into `main`:

DO NOT do this anymore:

```bash
git checkout main
git rebase dev
```

Why:

* PR already merged `dev` into `main`
* rebasing afterward may duplicate commits
* commit hashes change
* Git history becomes messy

---

# 9. Correct Synchronization Workflow

After PR merge:

```bash
git checkout main
git pull

git checkout dev
git merge main
git push
```

Meaning:

```text
main -> merge -> dev
```

NOT:

```text
main <- rebase <- dev
```

---

# 10. Why Use merge Instead of rebase

Because after a PR:

```text
main already contains dev history
```

Using rebase afterward may cause:

* duplicated commits
* rewritten commit hashes
* protected branch warnings
* confusing history

Using `merge main into dev` is safer for long-term branches.

---

# 11. Recommended Long-Term Workflow

## Small Changes

```text
Work directly on dev
```

---

## Large Features

```text
feature/* -> dev
```

---

## Releases

```text
dev -> main
```

---

## Synchronization

```text
main -> merge -> dev
```

---

# 12. Core Rules

## DO

* keep `main` stable
* keep `dev` long-term
* create feature branches from `dev`
* merge `main` back into `dev`
* use PRs for releases

---

## DON'T

* develop directly on `main`
* rebase `main` onto `dev`
* keep feature branches forever
* rebase long-lived branches after PR merges

---

# 13. Final Workflow Overview

```text
main (stable)
  ↑
  PR
dev (daily development)
  ↑
  merge
feature/*
```
