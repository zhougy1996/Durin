from . import build_request_fixtures as request_fixtures
import pytest
import io
import os
from pathlib import Path
from unittest import mock
from durin_dev_tool.build import build_context, errors, models
from durin_dev_tool.build import process as build_process
from durin_dev_tool.build import runtime as build_runtime
from durin_dev_tool import runtime_program
from durin_dev_tool.build.output import BuildOutput
from durin_dev_tool.errors import DevToolError


class TestCore:
    make_profile = staticmethod(request_fixtures.make_profile)
    make_preset = staticmethod(request_fixtures.make_preset)

    def test_runtime_path_uses_runtime_variant_and_preset_role(self) -> None:
        preset = self.make_preset()
        values = dict(preset.values)
        cache = dict(values['cacheVariables'])
        cache['CMAKE_BUILD_TYPE'] = 'Release'
        cache['DURIN_PRESET_ROLE'] = 'Profiling'
        preset = models.ConfigurePreset('profiling', {**values, 'cacheVariables': cache})
        path = build_runtime.runtime_executable_path(self.make_profile(), preset, root=Path('repo'))
        assert path == Path('repo/Engine/Binaries/Win64/Release-Profiling/Runtime/DurinEditor/DurinEditor.exe')
    def test_run_application_reports_how_to_build_missing_runtime(self) -> None:
        preset = self.make_preset()
        request = request_fixtures.command_request(models.Action.RUN, options=request_fixtures.RunActionOptions())
        context = build_context.BuildContext(request, models.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows')
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with mock.patch.object(runtime_program, 'runtime_executable_path', return_value=Path('missing/DurinEditor.exe')), pytest.raises(DevToolError, match='was not found'):
            build_runtime.run_application(context, output)
    def test_run_application_waits_for_relaunched_descendants(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = self.make_preset()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        project = root / '示例 Project' / 'Example.dproject'
        project.parent.mkdir()
        project.touch()
        context = build_context.BuildContext(request_fixtures.command_request(models.Action.RUN, options=request_fixtures.RunActionOptions(project_path=project.resolve(), arguments=('--hidden-window', 'argument with spaces'))), models.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows')
        executable = root / 'DurinEditor.exe'
        executable.touch()
        with mock.patch.object(runtime_program, 'runtime_executable_path', return_value=executable), mock.patch.object(runtime_program, 'run_command') as run:
            build_runtime.run_application(context, output)
        assert run.call_args.args[0] == [str(executable), f'--project={project.resolve()}', '--hidden-window', 'argument with spaces']
        assert run.call_args.kwargs['wait_for_descendants']
        assert not run.call_args.kwargs['show_heartbeat']
        assert run.call_args.kwargs['colorize_log_levels']
    def test_run_command_waits_for_windows_process_job(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        process = mock.Mock(pid=42, returncode=0)
        process.stdout = io.StringIO()
        process.wait.return_value = 0
        process_job = mock.Mock()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        with mock.patch.object(build_process, 'command_log_path', return_value=directory / 'command.log'), mock.patch.object(build_process.subprocess, 'Popen', return_value=process), mock.patch.object(build_process.subprocess, 'CREATE_NEW_PROCESS_GROUP', 512, create=True), mock.patch.object(build_process, 'WindowsProcessJob', return_value=process_job), mock.patch.object(build_process.os, 'name', 'nt'):
            build_process.run_command(['DurinEditor.exe'], environment={}, output=output, wait_for_descendants=True, cwd=directory, state_directory=directory)
        process_job.assign.assert_called_once_with(process)
        process_job.wait.assert_called_once_with()
        process_job.close.assert_called_once_with()
    def test_interrupt_terminates_relaunched_windows_process_job(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        process = mock.Mock(pid=42, returncode=0)
        process.stdout = io.StringIO()
        process.wait.return_value = 0
        process.poll.return_value = 0
        process_job = mock.Mock()
        process_job.wait.side_effect = KeyboardInterrupt
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        with mock.patch.object(build_process, 'command_log_path', return_value=directory / 'command.log'), mock.patch.object(build_process.subprocess, 'Popen', return_value=process), mock.patch.object(build_process.subprocess, 'CREATE_NEW_PROCESS_GROUP', 512, create=True), mock.patch.object(build_process, 'WindowsProcessJob', return_value=process_job), mock.patch.object(build_process.os, 'name', 'nt'), pytest.raises(errors.BuildToolError, match='Application run was interrupted'):
            build_process.run_command(['DurinEditor.exe'], environment={}, output=output, recovery_required_on_interrupt=False, wait_for_descendants=True, cwd=directory, state_directory=directory)
        process_job.terminate.assert_called_once_with()
        process_job.close.assert_called_once_with()
