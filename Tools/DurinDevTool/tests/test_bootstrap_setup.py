import pytest
import argparse
import io
import json
import os
import sys
from pathlib import Path
from unittest import mock
REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
from durin_dev_tool.bootstrap import agent_config, application as bootstrap_application, handler, manifests as dependency_manifests, preflight, setup, toolchain_selection
from durin_dev_tool.bootstrap.models import BootstrapError
from durin_dev_tool.build import models, settings
from durin_dev_tool.context import CommandIO, RepositoryContext
from durin_dev_tool import python_environment
from durin_dev_tool.registry import CommandRegistry
from durin_dev_tool.worktree import transactions as worktree_transactions

REPOSITORY = RepositoryContext.load(REPOSITORY_ROOT)

class TestPythonEnvironmentLifecycle:

    @pytest.mark.parametrize('pip_available', [True, False])
    def test_existing_environment_restores_pip_only_when_needed(
        self, tmp_path: Path, pip_available: bool,
    ) -> None:
        python = setup.prepared_python_path(
            tmp_path, REPOSITORY.config.worktrees.python_environment,
        )
        python.parent.mkdir(parents=True)
        python.touch()
        (tmp_path / 'requirements.txt').write_text('', encoding='utf-8')
        completed = setup.subprocess.CompletedProcess([], 0 if pip_available else 1)
        with mock.patch.object(setup.subprocess, 'run', return_value=completed) as probe, mock.patch.object(setup, 'run_command') as run:
            assert setup.ensure_python_environment(tmp_path) == python

        assert probe.call_args.args[0] == [str(python), '-m', 'pip', '--version']
        commands = [call.args[0] for call in run.call_args_list]
        restore = [str(python), '-m', 'ensurepip', '--upgrade']
        assert (restore in commands) == (not pip_available)
        install_index = next(index for index, command in enumerate(commands) if command[1:4] == ['-m', 'pip', 'install'])
        if not pip_available:
            assert commands.index(restore) < install_index
        assert all('venv' not in command for command in commands)

    def test_failed_pip_restore_stops_before_dependency_install(
        self, tmp_path: Path,
    ) -> None:
        python = setup.prepared_python_path(
            tmp_path, REPOSITORY.config.worktrees.python_environment,
        )
        python.parent.mkdir(parents=True)
        python.touch()
        (tmp_path / 'requirements.txt').write_text('', encoding='utf-8')

        def run(command, **kwargs):
            if command[1:3] == ['-m', 'ensurepip']:
                raise BootstrapError('No module named ensurepip')

        with mock.patch.object(setup.subprocess, 'run', return_value=setup.subprocess.CompletedProcess([], 1)), mock.patch.object(setup, 'run_command', side_effect=run) as commands:
            with pytest.raises(BootstrapError, match='base Python installation includes ensurepip'):
                setup.ensure_python_environment(tmp_path)
        assert not any(call.args[0][1:4] == ['-m', 'pip', 'install'] for call in commands.call_args_list)

class TestAgentConfigLifecycle:

    def test_template_path_matches_repository_layout(self) -> None:
        repo_root = Path(__file__).resolve().parents[3]
        assert agent_config.template_path(repo_root).is_file()

    @staticmethod
    def create_repo(root: Path) -> None:
        template = root / REPOSITORY.config.paths.local_build_config_template
        template.parent.mkdir(parents=True)
        template.write_text('{"version": 1}\n', encoding='utf-8')

    def test_initialize_is_idempotent(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        self.create_repo(root)
        target = agent_config.ensure_agent_config(root)
        target.write_text('local edit\n', encoding='utf-8')
        agent_config.ensure_agent_config(root)
        assert target.read_text(encoding='utf-8') == 'local edit\n'

    def test_dry_run_does_not_create_config(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        self.create_repo(root)
        target = agent_config.ensure_agent_config(root, dry_run=True)
        assert not target.exists()

class TestVSCodeConfigLifecycle:

    @staticmethod
    def create_repo(root: Path) -> None:
        template_directory = root / REPOSITORY.config.paths.vscode_templates
        template_directory.mkdir(parents=True)
        for file_name in setup.VSCODE_TEMPLATE_FILES:
            (template_directory / file_name).write_text(
                f'template {file_name}\n',
                encoding='utf-8',
            )

    def test_initialize_copies_only_missing_files(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        self.create_repo(root)
        vscode = root / '.vscode'
        vscode.mkdir()
        settings = vscode / 'settings.json'
        settings.write_text('local settings\n', encoding='utf-8')

        launch = {'version': '0.2.0', 'configurations': [{'name': 'generated'}]}
        with mock.patch.object(setup, 'generate_vscode_launch_configuration', return_value=launch):
            assert setup.ensure_vscode_configuration(root) == vscode

        assert settings.read_text(encoding='utf-8') == 'local settings\n'
        assert json.loads((vscode / 'launch.json').read_text(encoding='utf-8')) == launch
        assert (vscode / 'extensions.json').read_text(encoding='utf-8') == 'template extensions.json\n'

    def test_existing_launch_configuration_is_not_generated_or_overwritten(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        self.create_repo(root)
        vscode = root / '.vscode'
        vscode.mkdir()
        launch = vscode / 'launch.json'
        launch.write_text('local launch\n', encoding='utf-8')
        with mock.patch.object(setup, 'generate_vscode_launch_configuration') as generate:
            setup.ensure_vscode_configuration(root)
        generate.assert_not_called()
        assert launch.read_text(encoding='utf-8') == 'local launch\n'

    def test_missing_template_does_not_create_target_directory(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        with pytest.raises(BootstrapError, match='templates are missing'):
            setup.ensure_vscode_configuration(root)
        assert not (root / '.vscode').exists()

    def test_launch_configuration_covers_registered_windows_presets(self) -> None:
        with mock.patch.object(setup, 'load_local_config', return_value=models.LocalConfig()):
            launch = setup.generate_vscode_launch_configuration(
                REPOSITORY_ROOT,
                current_host='windows',
                environment={},
            )
        configurations = {
            item['name']: item
            for item in launch['configurations']
        }
        assert tuple(configurations) == (
            'Win64-Debug-DurinEditor',
            'Win64-Release-DurinEditor',
            'Win64-Debug-DurinGame',
            'Win64-Release-DurinGame',
            'Win64-Shipping-DurinGame',
        )
        assert 'Win64-Debug-DurinEditor-FastConfigure' not in configurations
        assert configurations['Win64-Release-DurinEditor']['program'] == (
            '${workspaceFolder}/Engine/Binaries/Win64/Release/'
            'Runtime/DurinEditor/DurinEditor.exe'
        )
        assert configurations['Win64-Shipping-DurinGame']['program'] == (
            '${workspaceFolder}/Engine/Binaries/Win64/Shipping/'
            'Runtime/DurinGame/DurinGame.exe'
        )

    def test_launch_configuration_uses_lldb_for_macos_profile(self) -> None:
        with mock.patch.object(setup, 'load_local_config', return_value=models.LocalConfig()):
            launch = setup.generate_vscode_launch_configuration(
                REPOSITORY_ROOT,
                current_host='macos',
                environment={},
            )
        assert launch['configurations'] == [
            {
                'name': 'MacOS-arm64-Debug-DurinEditor',
                'type': 'cppdbg',
                'request': 'launch',
                'program': '${workspaceFolder}/Engine/Binaries/MacOS/Debug/Runtime/DurinEditor/DurinEditor',
                'cwd': '${workspaceFolder}',
                'stopAtEntry': False,
                'console': 'integratedTerminal',
                'MIMode': 'lldb',
            },
            {
                'name': 'MacOS-arm64-Release-DurinEditor',
                'type': 'cppdbg',
                'request': 'launch',
                'program': '${workspaceFolder}/Engine/Binaries/MacOS/Release/Runtime/DurinEditor/DurinEditor',
                'cwd': '${workspaceFolder}',
                'stopAtEntry': False,
                'console': 'integratedTerminal',
                'MIMode': 'lldb',
            },
            {
                'name': 'MacOS-arm64-Debug-DurinGame',
                'type': 'cppdbg',
                'request': 'launch',
                'program': '${workspaceFolder}/Engine/Binaries/MacOS/Debug/Runtime/DurinGame/DurinGame',
                'cwd': '${workspaceFolder}',
                'stopAtEntry': False,
                'console': 'integratedTerminal',
                'MIMode': 'lldb',
            },
        ]

class TestBootstrapRegistry:

    @pytest.fixture(autouse=True)
    def _setup_registry(self) -> None:
        self.registry = CommandRegistry()

    def test_bootstrap_commands_are_available_without_prepared_environment(self) -> None:
        commands = (['setup'], ['dependency', 'prepare', 'tracy'], ['dependency', 'validate'], ['worktree', 'open', '--dry-run'], ['worktree', 'list'], ['worktree', 'add', 'feature'], ['worktree', 'prepare'], ['worktree', 'remove', 'feature'])
        for arguments in commands:
            specification, _ = self.registry.parse(arguments)
            assert not specification.required_modules

    def test_setup_accepts_non_interactive_mode(self) -> None:
        specification, namespace = self.registry.parse(['setup', '--non-interactive'])
        assert specification.name == 'setup'
        assert namespace.non_interactive
        assert not namespace.skip_development_tools

    def test_check_routes_to_read_only_validation_without_setup_or_shell_restart(self) -> None:
        specification, namespace = self.registry.parse(['setup', '--check'])
        with mock.patch.object(bootstrap_application, 'check_setup_configuration') as check, mock.patch.object(bootstrap_application, 'setup_checkout') as initialize, mock.patch.object(handler, 'restart_prepared_shell') as restart:
            self.registry.execute(specification, namespace, repository_root=REPOSITORY_ROOT,
                                  stdout=io.StringIO(), stderr=io.StringIO(), session_state={})
        check.assert_called_once()
        initialize.assert_not_called()
        restart.assert_not_called()

    @pytest.mark.parametrize('valid', [True, False])
    def test_check_never_writes_checkout_files(self, tmp_path, valid) -> None:
        repository = RepositoryContext.load().at_root(tmp_path)
        config = tmp_path / '.agents/DevTool.user.json'
        config.parent.mkdir()
        original = b'{"version": 1, "build": {"parallelJobs": 7}}'
        config.write_bytes(original)
        selection = toolchain_selection.ToolchainSelection('cmake', None, (), {})
        output = io.StringIO()
        with mock.patch.object(setup, 'select_setup_toolchain', return_value=selection) as select, mock.patch.object(setup, 'validate_prerequisites', side_effect=None if valid else setup.PreflightError('invalid SDK')):
            if valid:
                setup.check_setup_configuration(repository, CommandIO(output, io.StringIO()))
            else:
                with pytest.raises(setup.PreflightError):
                    setup.check_setup_configuration(repository, CommandIO(output, io.StringIO()))
        assert select.call_args.kwargs['interactive'] is False
        assert config.read_bytes() == original
        assert sorted(path.relative_to(tmp_path).as_posix() for path in tmp_path.rglob('*')) == ['.agents', '.agents/DevTool.user.json']
        assert 'Template:' in output.getvalue() and 'Schema:' in output.getvalue()

    def test_check_rejects_invalid_config_before_toolchain_discovery(self, tmp_path) -> None:
        config = tmp_path / '.agents/DevTool.user.json'
        config.parent.mkdir()
        config.write_text('{"version": 2}', encoding='utf-8')
        output = io.StringIO()
        with mock.patch('durin_dev_tool.bootstrap.toolchain_setup.visual_studio_candidates') as discover, pytest.raises(setup.DevToolError):
            setup.check_setup_configuration(RepositoryContext.load().at_root(tmp_path), CommandIO(output, io.StringIO()))
        discover.assert_not_called()
        assert 'Schema:' in output.getvalue()
        assert config.read_text(encoding='utf-8') == '{"version": 2}'

    def test_setup_forwards_development_tools_opt_out(self) -> None:
        specification, namespace = self.registry.parse(['setup', '--non-interactive', '--skip-development-tools'])
        with mock.patch.object(bootstrap_application, 'setup_checkout') as run:
            self.registry.execute(specification, namespace, repository_root=REPOSITORY_ROOT, stdout=io.StringIO(), stderr=io.StringIO())
        assert run.call_args.kwargs['skip_development_tools'] is True
        assert run.call_args.kwargs['interactive'] is False

    def test_dependency_prepare_requires_exactly_one_selection_mode(self) -> None:
        for arguments in (['dependency', 'prepare'], ['dependency', 'prepare', '--all', 'tracy']):
            specification, namespace = self.registry.parse(arguments)
            with pytest.raises(BootstrapError, match='dependency names or --all'):
                self.registry.execute(specification, namespace, repository_root=REPOSITORY_ROOT, stdout=io.StringIO(), stderr=io.StringIO())

    def test_worktree_add_rejects_conflicting_modes(self) -> None:
        specification, namespace = self.registry.parse(['worktree', 'add', 'feature', '--branch', 'topic', '--detach'])
        with pytest.raises(Exception, match='cannot be used together'):
            self.registry.execute(specification, namespace, repository_root=REPOSITORY_ROOT, stdout=io.StringIO(), stderr=io.StringIO())

class TestSetupOrchestration:

    def test_setup_fallback_styles_tty_output_without_third_party_packages(self) -> None:
        class TtyBuffer(io.StringIO):
            def isatty(self) -> bool:
                return True

        stream = TtyBuffer()
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(
            handler, '_enable_virtual_terminal', return_value=True
        ):
            output = handler._BootstrapOutput(stream, color="auto")
            print('Durin setup completed successfully.', file=output)
        assert '\x1b[1;32m' in stream.getvalue()

    def test_setup_fallback_respects_plain_output(self) -> None:
        stream = io.StringIO()
        output = handler._BootstrapOutput(stream, color="never")
        print('Durin setup completed successfully.', file=output)
        assert '\x1b[' not in stream.getvalue()

    @pytest.mark.parametrize('color', ['auto', 'always', 'never'])
    def test_setup_fallback_color_policy_with_redirected_output(self, color) -> None:
        stream = io.StringIO()
        with mock.patch.dict(os.environ, {'NO_COLOR': '1', 'TERM': 'dumb'}):
            output = handler._BootstrapOutput(stream, color=color)
            output.write('Setup completed successfully.\n')
        assert ('\x1b[' in stream.getvalue()) == (color == 'always')

    def test_setup_fallback_exposes_child_process_file_descriptor(self) -> None:
        stream = mock.Mock()
        stream.fileno.return_value = 42
        output = handler._BootstrapOutput(stream, color="never")
        assert output.fileno() == 42
        stream.fileno.assert_called_once_with()

    def test_preflight_runs_before_every_repository_mutation(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        events: list[str] = []
        python = root / '.venv' / 'Scripts' / 'python.exe'
        selection = toolchain_selection.ToolchainSelection('cmake.exe', root / 'VsDevCmd.bat', ('x64',), {'PATH': 'ready'})
        with mock.patch.object(setup, 'select_setup_toolchain', return_value=selection), mock.patch.object(setup, 'validate_prerequisites', side_effect=lambda _, **__: events.append('preflight')), mock.patch.object(setup, 'ensure_agent_config', side_effect=lambda *_args, **_kwargs: events.append('config')), mock.patch.object(setup, 'save_toolchain_config', side_effect=lambda *_args, **_kwargs: events.append('toolchain')), mock.patch.object(setup, 'ensure_vscode_configuration', side_effect=lambda *_args, **_kwargs: events.append('vscode')), mock.patch.object(setup, 'ensure_python_environment', side_effect=lambda *_args, **_kwargs: events.append('python') or python), mock.patch.object(setup, 'ensure_development_tools', side_effect=lambda *_args: events.append('tools')):
            assert setup.setup_repository(root) == python
        assert events == ['preflight', 'config', 'toolchain', 'vscode', 'python', 'tools']

    @pytest.mark.parametrize('check_only', [False, True])
    def test_setup_and_check_use_the_same_preparation_stage(self, tmp_path, check_only) -> None:
        repository = RepositoryContext.load().at_root(tmp_path)
        selection = toolchain_selection.ToolchainSelection('cmake', None, (), {})
        command_io = CommandIO(io.StringIO(), io.StringIO())
        with mock.patch.object(setup, 'prepare_setup', return_value=selection) as prepare, mock.patch.object(setup, 'ensure_agent_config'), mock.patch.object(setup, 'save_toolchain_config'), mock.patch.object(setup, 'ensure_vscode_configuration'), mock.patch.object(setup, 'ensure_python_environment', return_value=tmp_path / 'python'), mock.patch.object(setup, 'ensure_development_tools'):
            if check_only:
                setup.check_setup_configuration(repository, command_io)
            else:
                setup.setup_repository(repository, command_io)
        prepare.assert_called_once_with(repository, command_io, interactive=False)

    def test_failed_preparation_prevents_setup_mutations(self, tmp_path) -> None:
        with mock.patch.object(setup, 'prepare_setup', side_effect=setup.PreflightError('invalid SDK')), mock.patch.object(setup, 'ensure_agent_config') as config, mock.patch.object(setup, 'ensure_vscode_configuration') as vscode, mock.patch.object(setup, 'ensure_python_environment') as python, mock.patch.object(setup, 'ensure_development_tools') as tools, pytest.raises(setup.PreflightError):
            setup.setup_repository(tmp_path)
        for mutation in (config, vscode, python, tools):
            mutation.assert_not_called()

    def test_linked_worktree_setup_points_only_to_unified_prepare(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        (root / '.git').write_text('gitdir: elsewhere', encoding='utf-8')
        with pytest.raises(BootstrapError, match='DevTool worktree prepare') as raised:
            setup.setup_repository(root)
        assert 'WorktreeTool' not in str(raised.value)

    @pytest.mark.parametrize('skip_tools', [False, True])
    def test_setup_prepares_development_tools_unless_skipped(self, tmp_path_factory: pytest.TempPathFactory, skip_tools: bool) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        python = root / '.venv' / 'Scripts' / 'python.exe'
        selection = toolchain_selection.ToolchainSelection('cmake.exe', root / 'VsDevCmd.bat', ('x64',), {'PATH': 'ready'})
        with mock.patch.object(setup, 'select_setup_toolchain', return_value=selection), mock.patch.object(setup, 'validate_prerequisites'), mock.patch.object(setup, 'ensure_agent_config'), mock.patch.object(setup, 'save_toolchain_config'), mock.patch.object(setup, 'ensure_vscode_configuration'), mock.patch.object(setup, 'ensure_python_environment', return_value=python), mock.patch.object(setup, 'ensure_development_tools') as prepare:
            setup.setup_repository(root, skip_development_tools=skip_tools)
        if skip_tools:
            prepare.assert_not_called()
        else:
            prepare.assert_called_once_with(mock.ANY, python, mock.ANY)

    def test_development_tools_use_prepared_python_and_only_tool_package(self) -> None:
        repository = RepositoryContext.load()
        python = Path('prepared/python')
        command_io = CommandIO(stdout=io.StringIO(), stderr=io.StringIO())
        with mock.patch.object(setup, 'run_command') as run:
            setup.ensure_development_tools(repository, python, command_io)
        run.assert_called_once_with(
            [str(python), str(repository.root / 'Tools/DurinDevTool/durin_dev_tool/__main__.py'),
             'dependency', 'prepare', 'tracy-tools'],
            cwd=repository.root, command_io=command_io,
        )

    def test_development_tool_installation_failure_is_reported(self) -> None:
        with mock.patch.object(setup.subprocess, 'run', return_value=mock.Mock(returncode=1)):
            with pytest.raises(BootstrapError, match='Command failed'):
                setup.ensure_development_tools(
                    RepositoryContext.load(), Path('prepared/python'),
                    CommandIO(stdout=io.StringIO(), stderr=io.StringIO()),
                )

    def test_worktree_preparation_uses_shared_python_preflight(self) -> None:
        target = Path('C:/repo-feature')
        with mock.patch.object(worktree_transactions, 'validate_prerequisites') as validate:
            worktree_transactions.run_preflight(target)
        validate.assert_called_once_with(
            target,
            repository_context=mock.ANY,
            command_io=mock.ANY,
        )

    def test_worktree_preflight_preserves_unexpected_runtime_error(self) -> None:
        failure = RuntimeError('unexpected defect')
        with mock.patch.object(worktree_transactions, 'validate_prerequisites', side_effect=failure), pytest.raises(RuntimeError) as raised:
            worktree_transactions.run_preflight(Path('C:/repo-feature'))
        assert raised.value is failure

    def test_expected_preflight_failure_is_reported_as_devtool_error(self) -> None:
        namespace = argparse.Namespace(bootstrap_action='setup', color="never")
        failure = preflight.PreflightError('missing prerequisite')
        with mock.patch.object(bootstrap_application, 'setup_checkout', side_effect=failure), pytest.raises(handler.DevToolError, match='missing prerequisite') as raised:
            handler.run(
                namespace,
                repository_root=REPOSITORY_ROOT,
                stdout=io.StringIO(),
                stderr=io.StringIO(),
            )
        assert raised.value is failure

    def test_unexpected_bootstrap_runtime_error_retains_identity_and_traceback(self) -> None:
        namespace = argparse.Namespace(bootstrap_action='setup', color="never")
        failure = RuntimeError('unexpected defect')

        def fail_setup(
            _repository: RepositoryContext,
            _command_io: CommandIO,
            *,
            interactive: bool,
            skip_development_tools: bool,
        ) -> Path:
            del interactive, skip_development_tools
            raise failure

        with mock.patch.object(bootstrap_application, 'setup_checkout', side_effect=fail_setup), pytest.raises(RuntimeError) as raised:
            handler.run(
                namespace,
                repository_root=REPOSITORY_ROOT,
                stdout=io.StringIO(),
                stderr=io.StringIO(),
            )
        assert raised.value is failure
        assert 'fail_setup' in {entry.name for entry in raised.traceback}

    def test_successful_system_python_setup_restarts_interactive_shell(self) -> None:
        namespace = argparse.Namespace(bootstrap_action='setup')
        session: dict[str, object] = {}
        prepared_python = Path(sys.executable).with_name('prepared-python.exe')
        completed = mock.Mock(returncode=0)
        with mock.patch.object(bootstrap_application, 'setup_checkout', return_value=prepared_python), mock.patch.object(python_environment, 'prepared_environment_is_active', return_value=False), mock.patch.object(python_environment.subprocess, 'run', return_value=completed) as run:
            result = handler.run(namespace, repository_root=REPOSITORY_ROOT, stdout=io.StringIO(), stderr=io.StringIO(), session_state=session)
        assert result == 0
        assert session['exit_requested']
        assert run.call_args.args[0][0] == str(prepared_python)
        assert run.call_args.args[0][-1] == 'shell'
