from __future__ import annotations

from .specification import CommandSpec, argument


HANDLER = "durin_dev_tool.documentation.handler:run"
PLAN_SCOPES = ("active", "completed", "archive", "all")
DOCUMENT_KINDS = (
    "router", "contract", "guide", "task", "plan", "roadmap",
    "investigation", "policy", "generic",
)
CREATABLE_DOCUMENT_KINDS = ("router", "contract", "guide", "generic")

PLAN_LIST_ARGUMENTS = (
    argument("--scope", choices=PLAN_SCOPES, default="active"),
    argument("--query", help="filter by title or filename"),
    argument("--all-results", action="store_true", help="allow an unfiltered archive or all-scope listing"),
    argument("--format", choices=("markdown", "terminal"), default=None, dest="output_format"),
    argument("--color", choices=("auto", "always", "never"), default="auto"),
)
PLAN_VALIDATE_ARGUMENTS = (argument("--scope", choices=PLAN_SCOPES, default="all"),)
PLAN_ARCHIVE_ARGUMENTS = (
    argument("month", help="completion month in YYYY-MM form"),
    argument("--dry-run", action="store_true", help="preview without applying the archive"),
    argument("--apply", action="store_true", help="compatibility flag; archives apply by default"),
)
PLAN_CREATE_ARGUMENTS = (
    argument("plan_path", metavar="PATH"),
    argument("--title", required=True, help="plan title without the 'Plan' suffix"),
    argument("--summary", required=True, help="one-line primary scope"),
    argument("--dry-run", action="store_true", help="preview without creating the plan"),
    argument("--format", choices=("markdown", "terminal", "json"), default=None, dest="output_format"),
)


def lifecycle_command(name: str, summary: str, default_prefix: str) -> CommandSpec:
    create_commands = (
        (
            CommandSpec(
                "create",
                "create an implementation plan, or preview with --dry-run",
                HANDLER,
                arguments=PLAN_CREATE_ARGUMENTS,
                defaults=((f"{default_prefix}_action", "create"),),
            ),
        )
        if name == "plan"
        else ()
    )
    return CommandSpec(
        name,
        summary,
        subcommands=(
            *create_commands,
            CommandSpec(
                "list",
                f"list {'implementation plans' if name == 'plan' else 'engineering roadmaps'}",
                HANDLER,
                arguments=PLAN_LIST_ARGUMENTS,
                defaults=((f"{default_prefix}_action", "list"),),
            ),
            *(
                (
                    CommandSpec(
                        "context",
                        "show compact implementation context for one plan",
                        HANDLER,
                        arguments=(
                            argument("plan_query", metavar="QUERY"),
                            argument("--scope", choices=PLAN_SCOPES, default="active"),
                            argument("--format", choices=("markdown", "json"), default="markdown", dest="output_format"),
                        ),
                        defaults=((f"{default_prefix}_action", "context"),),
                    ),
                )
                if name == "plan"
                else ()
            ),
            CommandSpec(
                "validate",
                f"validate {name} metadata and layout",
                HANDLER,
                arguments=PLAN_VALIDATE_ARGUMENTS,
                defaults=((f"{default_prefix}_action", "validate"),),
            ),
            CommandSpec(
                "archive",
                f"archive one {'completion' if name == 'plan' else 'roadmap completion'} month",
                HANDLER,
                arguments=PLAN_ARCHIVE_ARGUMENTS,
                defaults=((f"{default_prefix}_action", "archive"),),
            ),
        ),
    )


DOCUMENT_OUTPUT = argument("--format", choices=("markdown", "terminal", "json"), default=None, dest="output_format")
DOCUMENT_ARCHIVE = argument("--include-archive", action="store_true", help="include archived plans and roadmaps")
DOCUMENT_FILTERS = (
    argument("--under", help="repository-relative Documentation path"),
    argument("--kind", dest="kinds", choices=DOCUMENT_KINDS, action="append", default=None),
    DOCUMENT_ARCHIVE,
    DOCUMENT_OUTPUT,
)

COMMAND_SPEC = CommandSpec(
    "doc",
    "discover, validate, and safely change documentation",
    subcommands=(
        CommandSpec("list", "list repository documentation", HANDLER, arguments=DOCUMENT_FILTERS, defaults=(("document_action", "list"),)),
        CommandSpec(
            "find", "find documentation by natural-language terms", HANDLER,
            arguments=(
                argument("document_query", metavar="QUERY"),
                argument("--limit", dest="document_limit", type=int, choices=range(1, 101), default=10, metavar="1..100", help="maximum ranked results (default: 10)"),
                *DOCUMENT_FILTERS,
            ),
            defaults=(("document_action", "find"),),
        ),
        CommandSpec(
            "refs", "show inbound and outbound document references", HANDLER,
            arguments=(argument("document_path", metavar="PATH"), DOCUMENT_ARCHIVE, DOCUMENT_OUTPUT),
            defaults=(("document_action", "refs"),),
        ),
        CommandSpec(
            "validate", "validate repository documentation", HANDLER,
            arguments=(argument("--scope", choices=("all", "changed"), default="all"), DOCUMENT_ARCHIVE, DOCUMENT_OUTPUT),
            defaults=(("document_action", "validate"),),
        ),
        CommandSpec(
            "create", "create a documentation file, or preview with --dry-run", HANDLER,
            arguments=(
                argument("document_kind", choices=CREATABLE_DOCUMENT_KINDS, metavar="KIND"),
                argument("document_path", metavar="PATH"), argument("--title", required=True),
                argument("--summary", default=""),
                argument("--dry-run", action="store_true"),
                argument("--apply", action="store_true", help="compatibility flag; changes apply by default"),
                DOCUMENT_OUTPUT,
            ),
            defaults=(("document_action", "create"),),
        ),
        CommandSpec(
            "move", "move a document and repair references, or preview with --dry-run", HANDLER,
            arguments=(
                argument("source_path", metavar="SOURCE"),
                argument("destination_path", metavar="DESTINATION"),
                argument("--dry-run", action="store_true"),
                argument("--apply", action="store_true", help="compatibility flag; changes apply by default"),
                DOCUMENT_OUTPUT,
            ),
            defaults=(("document_action", "move"),),
        ),
        CommandSpec(
            "task", "discover, validate, and remove open engineering tasks",
            subcommands=(
                CommandSpec("list", "list open engineering tasks", HANDLER, arguments=(argument("--query", dest="task_query", help="filter by title, outcome, or filename"), DOCUMENT_OUTPUT), defaults=(("task_action", "list"),)),
                CommandSpec("validate", "validate open engineering tasks", HANDLER, arguments=(DOCUMENT_OUTPUT,), defaults=(("task_action", "validate"),)),
                CommandSpec(
                    "remove",
                    "remove an open engineering task, or preview with --dry-run",
                    HANDLER,
                    arguments=(
                        argument("task_path", metavar="PATH"),
                        argument("--dry-run", action="store_true"),
                        argument("--apply", action="store_true", help="compatibility flag; changes apply by default"),
                        DOCUMENT_OUTPUT,
                    ),
                    defaults=(("task_action", "remove"),),
                ),
            ),
            default_subcommand="list",
        ),
        lifecycle_command("plan", "manage implementation-plan lifecycle", "plan"),
        lifecycle_command("roadmap", "manage engineering-roadmap lifecycle", "roadmap"),
    ),
)
