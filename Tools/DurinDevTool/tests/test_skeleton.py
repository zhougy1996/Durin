import pytest
import io
import subprocess
import sys
from pathlib import Path
from unittest import mock
PRODUCT_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = PRODUCT_ROOT.parents[1]
if str(PRODUCT_ROOT) not in sys.path:
    sys.path.insert(0, str(PRODUCT_ROOT))
from durin_dev_tool import cli
from durin_dev_tool.python_environment import launcher_command, prepared_python_path
from durin_dev_tool.registry import CommandRegistry, require_prepared_environment
from durin_dev_tool.repository import find_repository_root

class TestRepositoryDiscovery:

    def test_discovers_repository_from_nested_directory(self) -> None:
        assert find_repository_root(PRODUCT_ROOT / 'tests') == REPOSITORY_ROOT

    def test_rejects_directory_without_repository_markers(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        with mock.patch('durin_dev_tool.repository.is_repository_root', return_value=False), pytest.raises(RuntimeError, match='Could not find'):
            find_repository_root(Path(directory))

    def test_cli_uses_package_location_when_working_directory_is_unrelated(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        completed = subprocess.run([sys.executable, str(PRODUCT_ROOT / 'durin_dev_tool' / '__main__.py'), 'help'], cwd=directory, text=True, capture_output=True, check=False)
        assert completed.returncode == 0, completed.stderr
        assert 'DurinDevTool commands:' in completed.stdout

class TestCommandRegistry:

    def test_direct_help_and_shell_help_share_registry_output(self) -> None:
        registry = CommandRegistry()
        direct_output = io.StringIO()
        assert cli.run(['help'], registry=registry, repository_root=REPOSITORY_ROOT, stdout=direct_output) == 0
        assert direct_output.getvalue().strip() == registry.format_help()

    def test_missing_environment_stops_before_placeholder_import(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        output = io.StringIO()
        errors = io.StringIO()
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        with mock.patch('durin_dev_tool.registry.importlib.import_module') as import_module:
            with pytest.raises(RuntimeError, match='DevTool setup'):
                cli.run(['build'], repository_root=root, stdout=output, stderr=errors)
        import_module.assert_not_called()

    def test_system_python_is_rejected_when_prepared_interpreter_exists(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        interpreter = prepared_python_path(root, Path('.venv'))
        interpreter.parent.mkdir(parents=True)
        interpreter.touch()
        expected_launcher = launcher_command().replace('.', r'\.')
        with mock.patch('durin_dev_tool.registry.sys.executable', str(root / 'system-python.exe')), mock.patch('durin_dev_tool.registry.importlib.import_module') as import_module, pytest.raises(RuntimeError, match=rf'Restart through.*{expected_launcher}'):
            cli.run(['build'], repository_root=root)
        import_module.assert_not_called()

    def test_incomplete_environment_is_rejected_before_handler_import(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        interpreter = prepared_python_path(root, Path('.venv'))
        interpreter.parent.mkdir(parents=True)
        interpreter.touch()
        with mock.patch('durin_dev_tool.registry.sys.executable', str(interpreter)), mock.patch('durin_dev_tool.registry.importlib.util.find_spec', return_value=None), mock.patch('durin_dev_tool.registry.importlib.import_module') as import_module, pytest.raises(RuntimeError, match='incomplete.*DevTool setup'):
            cli.run(['build'], repository_root=root)
        import_module.assert_not_called()

    def test_prepared_environment_accepts_matching_interpreter_and_dependencies(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        interpreter = prepared_python_path(root, Path('.venv'))
        interpreter.parent.mkdir(parents=True)
        interpreter.touch()
        with mock.patch('durin_dev_tool.registry.sys.executable', str(interpreter)), mock.patch('durin_dev_tool.registry.importlib.util.find_spec', return_value=object()):
            require_prepared_environment(root, required_modules=('rich',))

    def test_no_arguments_selects_shell(self) -> None:
        spec, _namespace = CommandRegistry().parse([])
        assert spec.name == 'shell'

    def test_shell_startup_uses_durin_prompt(self) -> None:
        from durin_dev_tool.shell import run_shell
        stdout = io.StringIO()
        result = run_shell(registry=CommandRegistry(), repository_root=REPOSITORY_ROOT, stdout=stdout, stderr=io.StringIO(), input_func=mock.Mock(side_effect=EOFError))
        assert result == 0
        assert 'Durin Developer Tool shell' in stdout.getvalue()

class TestLauncher:

    def test_launcher_prefers_venv_then_python_launcher_then_path_python(self) -> None:
        content = (REPOSITORY_ROOT / 'DevTool.bat').read_text(encoding='utf-8')
        venv = content.index('if exist "%VENV_PYTHON%"')
        launcher = content.index('where py')
        path_python = content.index('where python')
        assert venv < launcher
        assert launcher < path_python
        assert 'py -3 "%ENTRY_POINT%" %*' in content
        assert 'python "%ENTRY_POINT%" %*' in content
        assert 'EnableDelayedExpansion' in content
        assert content.count('exit /b !ERRORLEVEL!') >= 3
        assert 'Tools\\DurinDevTool\\durin_dev_tool\\__main__.py' in content

    def test_posix_launcher_prefers_prepared_python_and_preserves_arguments(self) -> None:
        launcher = REPOSITORY_ROOT / 'DevTool'
        content = launcher.read_text(encoding='utf-8')
        assert content.startswith('#!/bin/sh\n')
        assert launcher.stat().st_mode & 0o111
        assert content.index('if [ -x "$VENV_PYTHON" ]') < content.index(
            'for PYTHON_NAME in python3'
        )
        assert 'exec "$VENV_PYTHON" "$ENTRY_POINT" "$@"' in content
        assert 'exec "$PYTHON_COMMAND" "$ENTRY_POINT" "$@"' in content
        assert 'command -v "$PYTHON_NAME"' in content
        assert 'sys.version_info >= (3, 10)' in content
        assert 'Tools/DurinDevTool/durin_dev_tool/__main__.py' in content

    def test_posix_launcher_runs_from_outside_the_repository(
        self, tmp_path_factory: pytest.TempPathFactory
    ) -> None:
        completed = subprocess.run(
            [str(REPOSITORY_ROOT / 'DevTool'), 'help'],
            cwd=tmp_path_factory.mktemp('case'),
            text=True,
            capture_output=True,
            check=False,
        )
        assert completed.returncode == 0, completed.stderr
        assert 'DurinDevTool commands:' in completed.stdout
