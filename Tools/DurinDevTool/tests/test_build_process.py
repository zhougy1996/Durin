import pytest
import io
import os
import threading
from pathlib import Path
from unittest import mock
from . import build_request_fixtures as request_fixtures
from durin_dev_tool.build import errors, models, settings
from durin_dev_tool.build import process as build_process
from durin_dev_tool.build.output import BuildOutput


class TestCore:
    make_profile = staticmethod(request_fixtures.make_profile)
    make_preset = staticmethod(request_fixtures.make_preset)

    @pytest.mark.parametrize('mode, terminal, agent, heartbeat', [
        (models.OutputMode.AUTO, False, False, True),
        (models.OutputMode.COMPACT, True, False, True),
        (models.OutputMode.PROGRESS, False, False, True),
        (models.OutputMode.PROGRESS, True, False, False),
        (models.OutputMode.FULL, False, False, False),
        (models.OutputMode.FULL, False, True, True),
    ])
    def test_heartbeat_and_log_visibility_during_command(
        self, tmp_path, mode, terminal, agent, heartbeat
    ) -> None:
        stdout = io.StringIO()
        output = BuildOutput(plain=True, output_mode=mode, force_terminal=terminal,
                             stdout=stdout, stderr=io.StringIO())
        log_path = tmp_path / 'command.log'
        drained = threading.Event()
        release_reader = threading.Event()

        class ChildOutput(io.StringIO):
            def __iter__(self):
                yield '\x1b[32m[12/80] Building CXX object sample.cpp\x1b[0m\n'
                yield 'routine diagnostic\n'
                drained.set()
                assert release_reader.wait(5)

        process = mock.Mock(stdout=ChildOutput())
        waits = 0

        def launch(*args, **kwargs):
            assert (str(log_path) in stdout.getvalue()) == output.compact
            return process

        def wait(timeout):
            nonlocal waits
            assert timeout == 30.0
            try:
                assert drained.wait(5)
                # The reader still has the log open: it must already be readable.
                assert 'routine diagnostic' in log_path.read_text(encoding='utf-8')
                waits += 1
                if waits == 1:
                    raise build_process.subprocess.TimeoutExpired('cmake', timeout)
                return 0
            finally:
                release_reader.set()

        process.wait.side_effect = wait
        with mock.patch.object(build_process, 'command_log_path', return_value=log_path), \
             mock.patch.object(build_process.subprocess, 'Popen', side_effect=launch):
            build_process.run_command(['cmake', '--build', '.'], environment={}, output=output,
                                      show_heartbeat=agent, cwd=tmp_path, state_directory=tmp_path)
        rendered = stdout.getvalue()
        assert ('still running' in rendered) == heartbeat
        if heartbeat:
            assert 's elapsed). Latest progress: [12/80] Building CXX object sample.cpp' in rendered
        if output.compact:
            assert 'routine diagnostic' not in rendered
            assert rendered.count(str(log_path)) == 1

    def test_silent_compact_command_still_emits_heartbeat(self, tmp_path) -> None:
        stdout = io.StringIO()
        output = BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO())
        process = mock.Mock(stdout=io.StringIO())
        process.wait.side_effect = [build_process.subprocess.TimeoutExpired('cmake', 30), 0]
        with mock.patch.object(build_process.subprocess, 'Popen', return_value=process):
            build_process.run_command(['cmake'], environment={}, output=output,
                                      cwd=tmp_path, state_directory=tmp_path)
        assert 'still running' in stdout.getvalue()
        assert 'Latest progress' not in stdout.getvalue()

    def test_transcript_retains_latest_configure_status(self) -> None:
        transcript = build_process.CommandTranscript()
        transcript.add('-- Configuring done (1.0s)\n')
        transcript.add('routine diagnostic\n')
        assert transcript.latest_progress == '-- Configuring done (1.0s)'
        transcript.add('[3/4] Linking CXX executable app\n')
        assert transcript.latest_progress == '[3/4] Linking CXX executable app'

    def test_keyboard_interrupt_terminates_child_process_tree(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        process = mock.Mock()
        process.stdout = io.StringIO()
        process.wait.side_effect = KeyboardInterrupt
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        with mock.patch.object(build_process, 'command_log_path', return_value=Path(directory) / 'command.log'), mock.patch.object(build_process.subprocess, 'Popen', return_value=process), mock.patch.object(build_process, 'terminate_process_tree') as terminate:
            with pytest.raises(errors.BuildToolInterruptedError):
                build_process.run_command(['cmake', '--version'], environment={}, output=output)
        terminate.assert_called_once_with(process, cwd=settings.default_build_paths().root)
    def test_run_command_does_not_inherit_buildtool_handles(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        process = mock.Mock()
        process.stdout = io.StringIO()
        process.wait.return_value = 0
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        with mock.patch.object(build_process, 'command_log_path', return_value=Path(directory) / 'command.log'), mock.patch.object(build_process.subprocess, 'Popen', return_value=process) as popen:
            build_process.run_command(['cmake', '--version'], environment={}, output=output)
        assert popen.call_args.kwargs['close_fds']
        assert popen.call_args.kwargs['stdout'] is build_process.subprocess.PIPE
    def test_unavailable_command_log_prevents_child_launch(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        unavailable = Path(directory) / 'unavailable'
        unavailable.write_text('not a directory', encoding='utf-8')
        log_path = unavailable / 'command.log'
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        child = [os.sys.executable, '-c', "print('x' * 1_000_000)"]
        with mock.patch.object(build_process, 'command_log_path', return_value=log_path), mock.patch.object(build_process.subprocess, 'Popen') as popen, pytest.raises(errors.BuildToolError, match='Could not capture command output') as raised:
            build_process.run_command(child, environment=os.environ, output=output)
        popen.assert_not_called()
        assert raised.value.log_path == log_path
        assert str(log_path) in str(raised.value)
        assert isinstance(raised.value.__cause__, OSError)
    def test_command_timeout_terminates_child_process_tree(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        process = mock.Mock()
        process.stdout = io.StringIO('compiler.cpp(7): error C1234: broken\n')
        process.wait.side_effect = build_process.subprocess.TimeoutExpired(['CoreTests'], 0)
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        with mock.patch.object(build_process, 'command_log_path', return_value=Path(directory) / 'command.log'), mock.patch.object(build_process.subprocess, 'Popen', return_value=process), mock.patch.object(build_process, 'terminate_process_tree') as terminate, pytest.raises(errors.BuildToolError, match='timed out'):
            build_process.run_command(['CoreTests'], environment={}, output=output, timeout_seconds=0.001)
        terminate.assert_called_once_with(process, cwd=settings.default_build_paths().root)
    def test_compact_command_output_is_logged_and_failure_is_summarized(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        stdout = io.StringIO()
        output = BuildOutput(plain=True, output_mode=models.OutputMode.COMPACT, stdout=stdout, stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        log_path = Path(directory) / 'command.log'
        with mock.patch.object(build_process, 'command_log_path', return_value=log_path):
            with pytest.raises(errors.BuildToolError) as raised:
                build_process.run_command([os.sys.executable, '-c', "print('noise'); print('source.cpp(9): error C1000: failed'); raise SystemExit(1)"], environment=os.environ, output=output)
        assert 'noise' in log_path.read_text(encoding='utf-8')
        assert '\nnoise\n' not in stdout.getvalue()
        assert 'error C1000' in raised.value.output_excerpt
        assert raised.value.log_path == log_path
    def test_full_command_output_streams_and_is_logged(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        stdout = io.StringIO()
        output = BuildOutput(plain=True, output_mode=models.OutputMode.FULL, stdout=stdout, stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        log_path = Path(directory) / 'command.log'
        with mock.patch.object(build_process, 'command_log_path', return_value=log_path):
            build_process.run_command([os.sys.executable, '-c', "print('visible child output')"], environment=os.environ, output=output)
        assert 'visible child output' in log_path.read_text(encoding='utf-8')
        assert 'visible child output' in stdout.getvalue()
    def test_compact_command_output_preserves_gtest_summary(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        stdout = io.StringIO()
        output = BuildOutput(plain=True, output_mode=models.OutputMode.COMPACT, stdout=stdout, stderr=io.StringIO())
        child_script = "print('[==========] 122 tests from 25 test suites ran. (100 ms total)'); print('[  PASSED  ] 122 tests.')"
        directory = tmp_path_factory.mktemp('case')
        with mock.patch.object(build_process, 'command_log_path', return_value=Path(directory) / 'command.log'):
            build_process.run_command([os.sys.executable, '-c', child_script], environment=os.environ, output=output)
        assert '122 tests from 25 test suites ran' in stdout.getvalue()
        assert '[  PASSED  ] 122 tests.' in stdout.getvalue()
