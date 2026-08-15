import pytest
import argparse
import io
import json
import os
import sys
from pathlib import Path
from unittest import mock
REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
PRODUCT_ROOT = REPOSITORY_ROOT / 'Tools' / 'DurinDevTool'
if str(PRODUCT_ROOT) not in sys.path:
    sys.path.insert(0, str(PRODUCT_ROOT))
from durin_dev_tool.bootstrap import agent_config, application as bootstrap_application, handler, manifests as dependency_manifests, preflight, setup, toolchain_selection
from durin_dev_tool.bootstrap.models import BootstrapError
from durin_dev_tool.build import config as build_config
from durin_dev_tool.context import CommandIO, RepositoryContext
from durin_dev_tool.registry import CommandRegistry
from durin_dev_tool.worktree import transactions as worktree_transactions

REPOSITORY = RepositoryContext.load(REPOSITORY_ROOT)

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
        with mock.patch.object(setup, 'load_local_config', return_value=build_config.LocalConfig()):
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
            'Win64-Release-DurinEditor-Profiling',
            'Win64-Debug-DurinGame',
            'Win64-Release-DurinGame',
            'Win64-Release-DurinGame-Profiling',
            'Win64-Shipping-DurinGame',
        )
        assert 'Win64-Debug-DurinEditor-FastConfigure' not in configurations
        assert configurations['Win64-Release-DurinEditor-Profiling']['program'] == (
            '${workspaceFolder}/Engine/Binaries/Win64/Release-Profiling/'
            'Runtime/DurinEditor/DurinEditor.exe'
        )
        assert configurations['Win64-Shipping-DurinGame']['program'] == (
            '${workspaceFolder}/Engine/Binaries/Win64/Shipping/'
            'Runtime/DurinGame/DurinGame.exe'
        )

    def test_launch_configuration_uses_lldb_for_macos_profile(self) -> None:
        with mock.patch.object(setup, 'load_local_config', return_value=build_config.LocalConfig()):
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
            }
        ]

class TestBootstrapRegistry:

    @pytest.fixture(autouse=True)
    def _setup_registry(self) -> None:
        self.registry = CommandRegistry()

    def test_bootstrap_commands_are_available_without_prepared_environment(self) -> None:
        commands = (['setup'], ['dependency', 'prepare', '--libs', 'tracy'], ['dependency', 'validate'], ['worktree', 'open', '--dry-run'], ['worktree', 'list'], ['worktree', 'add', 'feature'], ['worktree', 'prepare'], ['worktree', 'remove', 'feature'])
        for arguments in commands:
            specification, _ = self.registry.parse(arguments)
            assert not specification.required_modules

    def test_setup_accepts_non_interactive_mode(self) -> None:
        specification, namespace = self.registry.parse(['setup', '--non-interactive'])
        assert specification.name == 'setup'
        assert namespace.non_interactive

    def test_dependency_prepare_requires_exactly_one_selection_mode(self) -> None:
        for arguments in (['dependency', 'prepare'], ['dependency', 'prepare', '--all', '--libs', 'tracy']):
            specification, namespace = self.registry.parse(arguments)
            with pytest.raises(Exception, match='exactly one'):
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
            output = handler._BootstrapOutput(stream, plain=False)
            print('Durin setup completed successfully.', file=output)
        assert '\x1b[1;32m' in stream.getvalue()

    def test_setup_fallback_respects_plain_output(self) -> None:
        stream = io.StringIO()
        output = handler._BootstrapOutput(stream, plain=True)
        print('Durin setup completed successfully.', file=output)
        assert '\x1b[' not in stream.getvalue()

    def test_setup_fallback_exposes_child_process_file_descriptor(self) -> None:
        stream = mock.Mock()
        stream.fileno.return_value = 42
        output = handler._BootstrapOutput(stream, plain=True)
        assert output.fileno() == 42
        stream.fileno.assert_called_once_with()

    def test_preflight_runs_before_every_repository_mutation(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        events: list[str] = []
        python = root / '.venv' / 'Scripts' / 'python.exe'
        selection = toolchain_selection.ToolchainSelection('cmake.exe', root / 'VsDevCmd.bat', ('x64',), {'PATH': 'ready'})
        with mock.patch.object(setup, 'select_setup_toolchain', return_value=selection), mock.patch.object(setup, 'validate_prerequisites', side_effect=lambda _, **__: events.append('preflight')), mock.patch.object(setup, 'ensure_agent_config', side_effect=lambda *_args, **_kwargs: events.append('config')), mock.patch.object(setup, 'save_toolchain_config', side_effect=lambda *_args, **_kwargs: events.append('toolchain')), mock.patch.object(setup, 'ensure_vscode_configuration', side_effect=lambda *_args, **_kwargs: events.append('vscode')), mock.patch.object(setup, 'ensure_python_environment', side_effect=lambda *_args, **_kwargs: events.append('python') or python), mock.patch.object(setup, 'prepare_dependencies', side_effect=lambda *_args, **_kwargs: events.append('dependencies')):
            assert setup.setup_repository(root) == python
        assert events == ['preflight', 'config', 'toolchain', 'vscode', 'python', 'dependencies']

    def test_linked_worktree_setup_points_only_to_unified_prepare(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        (root / '.git').write_text('gitdir: elsewhere', encoding='utf-8')
        with pytest.raises(BootstrapError, match='DevTool worktree prepare') as raised:
            setup.setup_repository(root)
        assert 'WorktreeTool' not in str(raised.value)

    def test_setup_uses_complete_dependency_selection(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        python = root / '.venv' / 'Scripts' / 'python.exe'
        selection = toolchain_selection.ToolchainSelection('cmake.exe', root / 'VsDevCmd.bat', ('x64',), {'PATH': 'ready'})
        with mock.patch.object(setup, 'select_setup_toolchain', return_value=selection), mock.patch.object(setup, 'validate_prerequisites'), mock.patch.object(setup, 'ensure_agent_config'), mock.patch.object(setup, 'save_toolchain_config'), mock.patch.object(setup, 'ensure_vscode_configuration'), mock.patch.object(setup, 'ensure_python_environment', return_value=python), mock.patch.object(setup, 'prepare_dependencies') as prepare:
            setup.setup_repository(root)
        request = prepare.call_args.args[1]
        assert request.use_all
        assert request.with_tests
        assert request.with_development
        assert request.cmake_command == 'cmake.exe'
        assert prepare.call_args.kwargs['environment'] == {'PATH': 'ready'}

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
        namespace = argparse.Namespace(bootstrap_action='setup', plain=True)
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
        namespace = argparse.Namespace(bootstrap_action='setup', plain=True)
        failure = RuntimeError('unexpected defect')

        def fail_setup(
            _repository: RepositoryContext,
            _command_io: CommandIO,
            *,
            interactive: bool,
        ) -> Path:
            del interactive
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
        with mock.patch.object(bootstrap_application, 'setup_checkout', return_value=prepared_python), mock.patch.object(handler.subprocess, 'run', return_value=completed) as run:
            result = handler.run(namespace, repository_root=REPOSITORY_ROOT, stdout=io.StringIO(), stderr=io.StringIO(), session_state=session)
        assert result == 0
        assert session['exit_requested']
        assert run.call_args.args[0][0] == str(prepared_python)
        assert run.call_args.args[0][-1] == 'shell'
