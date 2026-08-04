from __future__ import annotations

import os
import re
import sys
import threading
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

ANSI_ESCAPE_PATTERN = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
NINJA_PROGRESS_PATTERN = re.compile(r"^\[\d+/\d+(?:\s+[^\]]+)?\](?:\s|$)")
RUNTIME_LOG_LEVEL_PATTERN = re.compile(
    r"^\[\d{2}:\d{2}:\d{2}\]\[(trace|debug|info|warning|error|critical)\]",
    re.IGNORECASE,
)
RUNTIME_LOG_LEVEL_STYLES = {
    "trace": "white",
    "debug": "cyan",
    "info": "green",
    "warning": "bold yellow",
    "error": "bold red",
    "critical": "bold white on red",
}
TEST_STATUS_PATTERNS = (
    (re.compile(r"^\[\s*(?:PASSED|OK)\s*\]", re.IGNORECASE), "bold green"),
    (re.compile(r"^\[\s*(?:FAILED|ERROR)\s*\]", re.IGNORECASE), "bold red"),
    (re.compile(r"^\[\s*SKIPPED\s*\]", re.IGNORECASE), "bold yellow"),
    (re.compile(r"^\[\s*RUN\s*\]", re.IGNORECASE), "bold cyan"),
    (re.compile(r"^\s*Start\s+\d+:\s+", re.IGNORECASE), "bold cyan"),
    (
        re.compile(r"^\s*\d+/\d+\s+Test\s+#\d+:.*?\bPassed\b", re.IGNORECASE),
        "bold green",
    ),
    (
        re.compile(
            r"^\s*\d+/\d+\s+Test\s+#\d+:.*?(?:\bFailed\b|\bTimeout\b|\bNot Run\b)",
            re.IGNORECASE,
        ),
        "bold red",
    ),
    (
        re.compile(r"^\s*\d+/\d+\s+Test\s+#\d+:.*?\bSkipped\b", re.IGNORECASE),
        "bold yellow",
    ),
    (re.compile(r"^\s*\d+%\s+tests passed", re.IGNORECASE), None),
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
        if output_mode is OutputMode.AUTO:
            self.resolved_output_mode = (
                OutputMode.PROGRESS if output_is_terminal else OutputMode.COMPACT
            )
        elif output_mode is OutputMode.PROGRESS and not output_is_terminal:
            self.resolved_output_mode = OutputMode.COMPACT
        else:
            self.resolved_output_mode = output_mode
        self.compact = self.resolved_output_mode is OutputMode.COMPACT
        self.progress = self.resolved_output_mode is OutputMode.PROGRESS
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
        self._output_lock = threading.RLock()
        self._progress_width = 0

    def flush(self) -> None:
        with self._output_lock:
            self.console.file.flush()
            self.error_console.file.flush()

    def _finish_progress(self) -> None:
        if not self._progress_width:
            return
        self.console.file.write("\n")
        self.console.file.flush()
        self._progress_width = 0

    def finish_child_output(self) -> None:
        with self._output_lock:
            self._finish_progress()

    def info(self, message: str) -> None:
        with self._output_lock:
            self._finish_progress()
            self.console.print(message)
            self.flush()

    def raw_line(self, message: str) -> None:
        with self._output_lock:
            self._finish_progress()
            self.console.file.write(f"{message}\n")
            self.flush()

    def warning(self, message: str) -> None:
        with self._output_lock:
            self._finish_progress()
            self.console.print(f"[yellow]warning[/yellow]  {message}")
            self.flush()

    def success(self, message: str) -> None:
        with self._output_lock:
            self._finish_progress()
            self.console.print(f"[green]success[/green]  {message}")
            self.flush()

    def cancelled(self, message: str) -> None:
        with self._output_lock:
            self._finish_progress()
            self.console.print(f"[yellow]cancelled[/yellow]  {message}")
            self.flush()

    def command(self, command: str) -> None:
        with self._output_lock:
            self._finish_progress()
            self.console.print(f"[dim]$ {command}[/dim]")
            self.flush()

    def child_output(
        self,
        text: str,
        *,
        colorize_log_levels: bool = False,
        colorize_test_output: bool = False,
    ) -> None:
        with self._output_lock:
            clean = ANSI_ESCAPE_PATTERN.sub("", text.rstrip("\r\n"))
            if self.progress and NINJA_PROGRESS_PATTERN.match(clean):
                maximum_width = max(1, self.console.width - 1)
                visible = clean
                if len(visible) > maximum_width:
                    visible = visible[: max(0, maximum_width - 1)] + "…"
                if self.plain:
                    erase = "\r" + (" " * self._progress_width) + "\r"
                else:
                    erase = "\r\x1b[2K"
                self.console.file.write(erase + visible)
                self.console.file.flush()
                self._progress_width = len(visible)
                return
            self._finish_progress()
            if colorize_log_levels and not self.plain and "\x1b[" not in text:
                match = RUNTIME_LOG_LEVEL_PATTERN.match(text)
                if match is not None:
                    rendered = Text(text)
                    rendered.stylize(
                        RUNTIME_LOG_LEVEL_STYLES[match.group(1).lower()],
                        match.start(1),
                        match.end(1),
                    )
                    self.console.print(rendered, end="")
                    self.console.file.flush()
                    return
            if colorize_test_output and not self.plain and "\x1b[" not in text:
                for pattern, fixed_style in TEST_STATUS_PATTERNS:
                    match = pattern.match(text)
                    if match is None:
                        continue
                    lowered = match.group(0).lower()
                    style = fixed_style
                    if style is None:
                        if re.match(r"^\s*\d+%\s+tests passed", lowered):
                            style = "bold green" if lowered.lstrip().startswith("100%") else "bold red"
                        else:
                            style = "bold red" if "failed" in lowered else "bold green"
                    rendered = Text(text)
                    rendered.stylize(style, match.start(), match.end())
                    self.console.print(rendered, end="")
                    self.console.file.flush()
                    return
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
                f"auto ({self.resolved_output_mode.value})"
                if self.output_mode is OutputMode.AUTO
                else (
                    "progress (compact fallback)"
                    if self.output_mode is OutputMode.PROGRESS and self.compact
                    else self.output_mode.value
                )
            ),
        }
        if self.plain:
            self.console.print("DurinDevTool")
            for label, value in rows.items():
                self.console.print(f"  {label}: {value}")
            self.flush()
            return
        table = Table.grid(padding=(0, 1))
        table.add_column(style="bold cyan")
        table.add_column()
        for label, value in rows.items():
            table.add_row(label, str(value))
        self.console.print(Panel(table, title="DurinDevTool", border_style="cyan"))
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
