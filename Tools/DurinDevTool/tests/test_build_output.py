from __future__ import annotations
import pytest
import argparse
import io
import json
import os
import shutil
import subprocess
import zipfile
from dataclasses import replace
from pathlib import Path
from unittest import mock
REPO_ROOT = Path(__file__).resolve().parents[3]
DEV_TOOL_DIR = REPO_ROOT / 'Tools' / 'DurinDevTool'
if str(DEV_TOOL_DIR) not in os.sys.path:
    os.sys.path.insert(0, str(DEV_TOOL_DIR))
from durin_dev_tool.build import operations as build_cli
from durin_dev_tool.build import config as build_config
from durin_dev_tool.build import core as build_core
from durin_dev_tool.build import descriptors as build_descriptors
from durin_dev_tool.build import scaffolding as build_scaffolding
from durin_dev_tool.build.handler import request_from_namespace
from durin_dev_tool.build.output import BuildOutput
from durin_dev_tool.registry import CommandRegistry

def parse_build_request(arguments: list[str]) -> build_config.CommandRequest:
    _spec, namespace = CommandRegistry().parse(arguments)
    if getattr(namespace, 'selected_preset', ''):
        namespace.preset = namespace.selected_preset
    return request_from_namespace(namespace)

class TestOutput:

    def test_plain_output_has_no_ansi_sequences(self) -> None:
        stdout = io.StringIO()
        stderr = io.StringIO()
        output = BuildOutput(plain=True, stdout=stdout, stderr=stderr, force_terminal=True)
        output.success('done')
        output.failure(build_config.BuildToolError('failed'), None, 1.0)
        assert '\x1b[' not in stdout.getvalue() + stderr.getvalue()

    def test_non_tty_output_automatically_uses_plain_mode(self) -> None:
        output = BuildOutput(stdout=io.StringIO(), stderr=io.StringIO())
        assert output.plain
        assert output.compact

    def test_rich_tty_output_contains_ansi_and_semantic_status(self) -> None:
        stdout = io.StringIO()
        with mock.patch.dict(os.environ, {}, clear=True):
            output = BuildOutput(stdout=stdout, stderr=io.StringIO(), force_terminal=True)
            output.success('done')
        assert '\x1b[' in stdout.getvalue()
        assert 'success' in stdout.getvalue()
        assert not output.compact
        assert output.progress

    def test_explicit_output_mode_overrides_terminal_detection(self) -> None:
        compact = BuildOutput(output_mode=build_config.OutputMode.COMPACT, stdout=io.StringIO(), stderr=io.StringIO(), force_terminal=True)
        full = BuildOutput(output_mode=build_config.OutputMode.FULL, stdout=io.StringIO(), stderr=io.StringIO())
        assert compact.compact
        assert not full.compact
        assert not full.progress

    def test_progress_mode_falls_back_to_compact_without_terminal(self) -> None:
        output = BuildOutput(output_mode=build_config.OutputMode.PROGRESS, stdout=io.StringIO(), stderr=io.StringIO())
        assert output.compact
        assert not output.progress

    def test_progress_mode_replaces_ninja_status_and_streams_other_output(self) -> None:
        stdout = io.StringIO()
        output = BuildOutput(plain=True, output_mode=build_config.OutputMode.PROGRESS, stdout=stdout, stderr=io.StringIO(), force_terminal=True)
        output.child_output('[1/2] Building first.cpp\n')
        output.child_output('[2/2] Linking result.dll\n')
        output.child_output('compiler diagnostic\n')
        text = stdout.getvalue()
        assert '\r[1/2] Building first.cpp' in text
        assert '\r[2/2] Linking result.dll' in text
        assert '[1/2] Building first.cpp\n' not in text
        assert '[2/2] Linking result.dll\ncompiler diagnostic\n' in text

    def test_runtime_log_levels_are_colored_for_terminal_output(self) -> None:
        stdout = io.StringIO()
        with mock.patch.dict(os.environ, {}, clear=True):
            output = BuildOutput(stdout=stdout, stderr=io.StringIO(), force_terminal=True, output_mode=build_config.OutputMode.FULL)
            output.child_output('[12:34:56][warning]Runtime warning\n', colorize_log_levels=True)
        text = stdout.getvalue()
        assert '\x1b[' in text
        assert 'warning' in text
        assert 'Runtime warning' in text

    def test_runtime_log_level_coloring_respects_plain_output(self) -> None:
        stdout = io.StringIO()
        output = BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO(), force_terminal=True, output_mode=build_config.OutputMode.FULL)
        output.child_output('[12:34:56][error]Runtime error\n', colorize_log_levels=True)
        assert '\x1b[' not in stdout.getvalue()

    @pytest.mark.parametrize(
        ('line', 'expected_ansi'),
        [
            ('[ RUN      ] Core.Loads\n', True),
            ('[       OK ] Core.Loads (1 ms)\n', True),
            ('[  FAILED  ] Core.Loads (1 ms)\n', True),
            ('[  PASSED  ] 12 tests.\n', True),
            ('1/2 Test #1: Core.Loads ..........   Passed  0.01 sec\n', True),
            ('50% tests passed, 1 tests failed out of 2\n', True),
            ('ordinary test detail\n', False),
        ],
    )
    def test_native_test_statuses_are_colored_for_terminal_output(
        self, line: str, expected_ansi: bool
    ) -> None:
        stdout = io.StringIO()
        with mock.patch.dict(os.environ, {}, clear=True):
            output = BuildOutput(
                stdout=stdout,
                stderr=io.StringIO(),
                force_terminal=True,
                output_mode=build_config.OutputMode.FULL,
            )
            output.child_output(line, colorize_test_output=True)
        assert ('\x1b[' in stdout.getvalue()) is expected_ansi

    def test_native_test_coloring_respects_plain_output(self) -> None:
        stdout = io.StringIO()
        output = BuildOutput(
            plain=True,
            stdout=stdout,
            stderr=io.StringIO(),
            force_terminal=True,
            output_mode=build_config.OutputMode.FULL,
        )
        output.child_output('[  FAILED  ] Core.Loads\n', colorize_test_output=True)
        assert '\x1b[' not in stdout.getvalue()

    def test_failure_summary_contains_command_exit_code_and_recovery(self) -> None:
        stderr = io.StringIO()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=stderr)
        error = build_config.BuildToolError('compile failed', command=['cmake', '--build', 'Build'], exit_code=1, recovery='fix the compiler error')
        output.failure(error, None, 2.5)
        text = stderr.getvalue()
        assert 'cmake --build Build' in text
        assert 'Exit code: 1' in text
        assert 'fix the compiler error' in text

    def test_failure_without_derived_context_uses_available_request_details(self) -> None:
        stderr = io.StringIO()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=stderr)
        request = build_config.CommandRequest(build_config.Action.TEST, target='CoreTests', preset='debug')
        output.failure(build_config.BuildToolError('validation failed'), None, 0.5, request=request)
        text = stderr.getvalue()
        assert 'ERROR: Test failed: validation failed' in text
        assert 'Action: test' in text
        assert 'Preset: debug' in text
        assert 'Target: CoreTests' in text

    def test_no_color_environment_forces_plain_output(self) -> None:
        with mock.patch.dict(os.environ, {'NO_COLOR': '1'}):
            output = BuildOutput(stdout=io.StringIO(), stderr=io.StringIO(), force_terminal=True)
        assert output.plain

    def test_plain_stage_uses_ascii_boundary(self) -> None:
        stdout = io.StringIO()
        output = BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO())
        with output.stage('Build'):
            pass
        assert '== Build ==' in stdout.getvalue()
