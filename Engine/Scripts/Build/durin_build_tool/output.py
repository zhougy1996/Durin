from __future__ import annotations

import os
import sys
from contextlib import contextmanager
from time import perf_counter
from typing import Iterator, Mapping, TextIO

from rich.console import Console
from rich.panel import Panel
from rich.table import Table
from rich.text import Text

from .config import (
    Action,
    BuildContext,
    BuildToolError,
    CommandRequest,
    OutputMode,
    preset_build_directory,
)


class BuildOutput:
    def __init__(
        self,
        *,
        plain: bool = False,
        output_mode: OutputMode = OutputMode.AUTO,
        stdout: TextIO | None = None,
        stderr: TextIO | None = None,
        force_terminal: bool | None = None,
    ):
        stdout = stdout or sys.stdout
        stderr = stderr or sys.stderr
        no_color = plain or "NO_COLOR" in os.environ
        stdout_is_terminal = bool(getattr(stdout, "isatty", lambda: False)())
        output_is_terminal = stdout_is_terminal if force_terminal is None else force_terminal
        if force_terminal is None:
            force_terminal = stdout_is_terminal and not no_color
        self.plain = no_color or not force_terminal
        self.output_mode = output_mode
        self.compact = (
            not output_is_terminal
            if output_mode is OutputMode.AUTO
            else output_mode is OutputMode.COMPACT
        )
        self.console = Console(
            file=stdout,
            force_terminal=False if self.plain else force_terminal,
            no_color=self.plain,
            highlight=False,
            soft_wrap=True,
        )
        self.error_console = Console(
            file=stderr,
            force_terminal=False if self.plain else force_terminal,
            no_color=self.plain,
            highlight=False,
            soft_wrap=True,
        )

    def flush(self) -> None:
        self.console.file.flush()
        self.error_console.file.flush()

    def info(self, message: str) -> None:
        self.console.print(message)
        self.flush()

    def warning(self, message: str) -> None:
        self.console.print(f"[yellow]warning[/yellow]  {message}")
        self.flush()

    def success(self, message: str) -> None:
        self.console.print(f"[green]success[/green]  {message}")
        self.flush()

    def cancelled(self, message: str) -> None:
        self.console.print(f"[yellow]cancelled[/yellow]  {message}")
        self.flush()

    def command(self, command: str) -> None:
        self.console.print(f"[dim]$ {command}[/dim]")
        self.flush()

    def child_output(self, text: str) -> None:
        self.console.file.write(text)
        self.console.file.flush()

    def context(self, context: BuildContext) -> None:
        rows: Mapping[str, object] = {
            "Action": context.request.action.value,
            "Profile": context.profile.name,
            "Preset": context.preset.name,
            "Target": context.target or "—",
            "Build directory": preset_build_directory(context.preset),
            "CMake": context.cmake or "not required",
            "Parallel jobs": context.jobs or "not required",
            "Child output": (
                f"auto ({'compact' if self.compact else 'full'})"
                if self.output_mode is OutputMode.AUTO
                else self.output_mode.value
            ),
        }
        if self.plain:
            self.console.print("Durin BuildTool")
            for label, value in rows.items():
                self.console.print(f"  {label}: {value}")
            self.flush()
            return
        table = Table.grid(padding=(0, 1))
        table.add_column(style="bold cyan")
        table.add_column()
        for label, value in rows.items():
            table.add_row(label, str(value))
        self.console.print(Panel(table, title="Durin BuildTool", border_style="cyan"))
        self.flush()

    @contextmanager
    def stage(self, title: str) -> Iterator[None]:
        started = perf_counter()
        if self.plain:
            self.console.print(f"== {title} ==")
        else:
            self.console.rule(f"[bold cyan]{title}[/bold cyan]")
        self.flush()
        try:
            yield
        except BaseException:
            self.console.print(f"[red]{title} failed after {perf_counter() - started:.2f}s[/red]")
            self.flush()
            raise
        else:
            self.console.print(f"[dim]{title} completed in {perf_counter() - started:.2f}s[/dim]")
            self.flush()

    def failure(
        self,
        error: BuildToolError,
        context: BuildContext | None,
        elapsed: float,
        *,
        request: CommandRequest | None = None,
        preset: str = "",
    ) -> None:
        details = []
        active_request = context.request if context is not None else request
        if context is not None:
            details.extend(
                [
                    ("Action", context.request.action.value),
                    ("Preset", context.preset.name),
                    ("Target", context.target or "—"),
                ]
            )
        elif active_request is not None and active_request.action is not Action.SHELL:
            details.extend(
                [
                    ("Action", active_request.action.value),
                    ("Preset", preset or active_request.preset or "—"),
                    ("Target", active_request.target or "—"),
                ]
            )
        if error.command:
            details.append(("Command", " ".join(error.command)))
        if error.exit_code is not None:
            details.append(("Exit code", str(error.exit_code)))
        if error.log_path is not None:
            details.append(("Full output", str(error.log_path)))
        details.append(("Elapsed", f"{elapsed:.2f}s"))
        title = (
            f"{active_request.action.value.capitalize()} failed"
            if active_request is not None and active_request.action is not Action.SHELL
            else "Command failed"
        )

        if self.plain:
            if error.output_excerpt:
                self.error_console.print("---- Child output excerpt ----")
                self.error_console.print(error.output_excerpt, markup=False)
            self.error_console.print(f"ERROR: {title}: {error}")
            for label, value in details:
                self.error_console.print(f"  {label}: {value}")
            if error.recovery:
                self.error_console.print(f"  Recovery: {error.recovery}")
            self.flush()
            return

        if error.output_excerpt:
            self.error_console.print(
                Panel(
                    Text(error.output_excerpt),
                    title="Child output excerpt",
                    border_style="red",
                )
            )
        body = Text()
        body.append(str(error), style="bold red")
        for label, value in details:
            body.append(f"\n{label}: ", style="bold")
            body.append(value)
        if error.recovery:
            body.append("\nRecovery: ", style="bold yellow")
            body.append(error.recovery)
        self.error_console.print(Panel(body, title=title, border_style="red"))
        self.flush()
