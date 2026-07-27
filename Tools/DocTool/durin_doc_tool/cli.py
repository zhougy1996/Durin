"""Command-line and interactive shell interfaces for DocTool."""

from __future__ import annotations

import argparse
import os
import shlex
import sys
from pathlib import Path
from typing import Sequence

from .archive import ArchiveError, apply_archive, preview_archive
from .plans import filter_plans, load_catalog, render_listing


SCOPES = ("active", "completed", "archive", "all")


class DocToolError(RuntimeError):
    pass


def _plans_directory() -> Path:
    return Path(__file__).resolve().parents[3] / "Documentation" / "Plans"


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="DocTool",
        description="List, validate, and archive Durin implementation plans.",
    )
    commands = parser.add_subparsers(dest="command")

    listing = commands.add_parser("list", help="list implementation plans")
    listing.add_argument("--scope", choices=SCOPES, default="active")
    listing.add_argument("--query", help="filter by title or filename")
    listing.add_argument(
        "--all-results",
        action="store_true",
        help="allow an unfiltered archive or all-scope listing",
    )
    listing.add_argument(
        "--format",
        choices=("markdown", "terminal"),
        default=None,
        dest="output_format",
    )
    listing.add_argument(
        "--color",
        choices=("auto", "always", "never"),
        default="auto",
    )

    validate = commands.add_parser("validate", help="validate plan metadata and layout")
    validate.add_argument("--scope", choices=SCOPES, default="all")

    archive = commands.add_parser("archive", help="archive one completion month")
    archive.add_argument("month", help="completion month in YYYY-MM form")
    archive.add_argument(
        "--apply",
        action="store_true",
        help="apply the transaction; the default is a dry-run",
    )

    commands.add_parser("shell", help="open the interactive DocTool shell")
    return parser


def _print_errors(errors: Sequence[str]) -> None:
    for error in errors:
        print(f"error: {error}", file=sys.stderr)


def _run_list(args: argparse.Namespace, *, interactive: bool) -> int:
    if args.scope in {"archive", "all"} and not args.query and not args.all_results:
        raise DocToolError(
            "archive listings require --query <title-or-filename>; "
            "use --all-results only for an explicitly requested full listing"
        )
    catalog = load_catalog(_plans_directory())
    errors = catalog.errors_for(args.scope)
    if errors:
        _print_errors(errors)
        return 1
    plans = filter_plans(catalog.select(args.scope), args.query)
    if not plans:
        if args.query:
            raise DocToolError(
                f"no {args.scope} plans match query {args.query!r}"
            )
        if args.scope == "completed":
            print("No completed implementation plans are awaiting archival.")
            return 0
        raise DocToolError(f"no {args.scope} implementation plans found")
    output_format = args.output_format or ("terminal" if interactive else "markdown")
    print(
        render_listing(
            plans,
            catalog.plans_directory,
            scope=args.scope,
            output_format=output_format,
            color=args.color,
        )
    )
    return 0


def _run_validate(args: argparse.Namespace) -> int:
    catalog = load_catalog(_plans_directory())
    errors = catalog.errors_for(args.scope)
    if errors:
        _print_errors(errors)
        return 1
    plans = catalog.select(args.scope)
    if args.scope == "all":
        print(
            f"Validated {len(catalog.active)} active, "
            f"{len(catalog.completed)} completed, and "
            f"{len(catalog.archived)} archived implementation plans."
        )
    else:
        print(f"Validated {len(plans)} {args.scope} implementation plans.")
    return 0


def _print_archive(preview, *, applied: bool) -> None:
    repository = _plans_directory().parent.parent
    if not preview.moves:
        print(f"No completed plans are awaiting archival for {preview.month}.")
        return
    action = "Archived" if applied else "Would archive"
    for move in preview.moves:
        print(
            f"{action}: {move.source.relative_to(repository).as_posix()} -> "
            f"{move.destination.relative_to(repository).as_posix()}"
        )
    verb = "Updated" if applied else "Would update"
    print(f"{verb} {len(preview.reference_files)} referencing Markdown file(s).")
    for path in preview.reference_files:
        print(f"  {path.relative_to(repository).as_posix()}")
    if applied:
        print("Archive applied and all plans validated.")
    else:
        print("Dry-run only; add --apply to perform the archive.")


def _run_archive(args: argparse.Namespace) -> int:
    preview = (
        apply_archive(_plans_directory(), args.month)
        if args.apply
        else preview_archive(_plans_directory(), args.month)
    )
    _print_archive(preview, applied=args.apply)
    return 0


def _execute(args: argparse.Namespace, *, interactive: bool) -> int:
    if args.command == "list":
        return _run_list(args, interactive=interactive)
    if args.command == "validate":
        return _run_validate(args)
    if args.command == "archive":
        return _run_archive(args)
    if args.command == "shell":
        return run_shell()
    raise DocToolError("a command is required")


def _split_shell_command(line: str) -> list[str]:
    parts = shlex.split(line, posix=os.name != "nt")
    if os.name == "nt":
        return [
            part[1:-1]
            if len(part) >= 2 and part[0] == part[-1] and part[0] in {"'", '"'}
            else part
            for part in parts
        ]
    return parts


def _shell_help() -> None:
    print(
        "Commands:\n"
        "  list [--scope active|completed|archive|all] [--query <text>]\n"
        "       [--all-results] [--format terminal|markdown]\n"
        "  validate [--scope active|completed|archive|all]\n"
        "  archive <YYYY-MM> [--apply]\n"
        "  help\n"
        "  exit\n\n"
        "List output defaults to the human-oriented terminal format in this shell.\n"
        "Archive is always a dry-run unless --apply is supplied."
    )


def run_shell() -> int:
    parser = _parser()
    print("Durin DocTool shell")
    print("Type help for available commands.")
    while True:
        try:
            line = input("DocTool> ").strip()
        except EOFError:
            print()
            return 0
        except KeyboardInterrupt:
            print("\nUse exit to leave the shell.")
            continue
        if not line:
            continue
        try:
            parts = _split_shell_command(line)
            command = parts[0].lower()
            if command in {"exit", "quit"}:
                return 0
            if command in {"help", "?"}:
                _shell_help()
                continue
            args = parser.parse_args(parts)
            if args.command == "shell":
                raise DocToolError("the shell command cannot open a nested shell")
            _execute(args, interactive=True)
        except SystemExit:
            continue
        except (ArchiveError, DocToolError, OSError, ValueError) as error:
            print(f"error: {error}", file=sys.stderr)


def main(argv: Sequence[str] | None = None) -> int:
    parser = _parser()
    values = list(sys.argv[1:] if argv is None else argv)
    if not values:
        return run_shell()
    try:
        args = parser.parse_args(values)
        return _execute(args, interactive=False)
    except (ArchiveError, DocToolError, OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
