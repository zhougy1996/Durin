# Active Implementation Plans

Active plans live as individual Markdown files in this directory. Generate the
current index on demand instead of maintaining a shared table:

```powershell
python Documentation/Plans/list_active_plans.py
```

The command prints a deterministic Markdown table containing each plan's title,
relative link, and `Summary:` metadata. To perform the same metadata and
structure checks without printing the table, run:

```powershell
python Documentation/Plans/list_active_plans.py --validate
```

Plan authoring, validation, and archival rules are in `AGENTS.md`. Completed
history is indexed separately in [Archive](Archive/README.md).
