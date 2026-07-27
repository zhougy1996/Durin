from __future__ import annotations
import pytest
import io
import sys
from pathlib import Path
from unittest import mock
REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
PRODUCT_ROOT = REPOSITORY_ROOT / 'Tools' / 'DurinDevTool'
if str(PRODUCT_ROOT) not in sys.path:
    sys.path.insert(0, str(PRODUCT_ROOT))
from durin_dev_tool.build import handler
from durin_dev_tool.build import operations as build_operations
from durin_dev_tool.build.config import Action, CommandRequest, CreateKind, LinkType, LocalConfig, ModuleKind, OutputMode
from durin_dev_tool.registry import CommandRegistry
from durin_dev_tool.shell import normalize_compact_build_command, split_shell_command

class TestBuildRegistry:

    @pytest.fixture(autouse=True)
    def _setup_registry(self) -> None:
        self.registry = CommandRegistry()

    def parse(self, arguments: list[str]):
        _spec, namespace = self.registry.parse(arguments)
        return namespace

    def test_every_build_command_uses_the_build_handler(self) -> None:
        expected = {'stop', 'presets', 'preset', 'status', 'open-runtime', 'configure', 'build', 'clean', 'recover', 'purge', 'rebuild', 'test', 'run', 'create'}
        specifications = {specification.name: specification for specification in self.registry.specifications if specification.name in expected}
        assert set(specifications) == expected
        for name, specification in specifications.items():
            if name == 'create':
                assert specification.subcommands
                assert all((child.handler == 'durin_dev_tool.build.handler:run' for child in specification.subcommands))
            else:
                assert specification.handler == 'durin_dev_tool.build.handler:run'

    def test_direct_and_shell_tokens_produce_identical_namespaces(self) -> None:
        commands = ('stop --plain', 'presets --profile windows-msvc-x64 --preset win-msvc-x64-debug', 'preset win-msvc-x64-release --plain', 'status --output full', 'open-runtime --preset win-msvc-x64-debug', 'configure --fresh --jobs 8', 'build --target Core --output compact', 'clean --plain', 'recover --cmake cmake', 'purge --all-presets --yes', 'rebuild --target all', 'test --target CoreTests --filter Core.* --timeout 45', 'run --project "Examples/Sandbox/Sandbox.dproject" --args --scene Sample', 'create module Sample --project Examples/Sandbox/Sandbox.dproject --kind editor --link static --public-dependency Core --enable base --dry-run', 'create project Sample --path Examples/Sample --dry-run')
        for command in commands:
            direct = vars(self.parse(split_shell_command(command)))
            shell = vars(self.parse(split_shell_command(command)))
            assert direct == shell

    def test_build_commands_are_case_insensitive_and_keep_slash_aliases(self) -> None:
        canonical = vars(self.parse(['build', '--target', 'Core']))
        assert canonical == vars(self.parse(['BUILD', '--target', 'Core']))
        assert canonical == vars(self.parse(['/build', '--target', 'Core']))

    def test_shell_compact_build_forms_normalize_to_canonical_requests(self) -> None:
        pairs = (('build Core --plain', 'build --target Core --plain'), ('rebuild Core', 'rebuild --target Core'), ('test CoreTests Core.* --timeout 20', 'test --target CoreTests --filter Core.* --timeout 20'), ('run --hidden-window', 'run --args --hidden-window'))
        for compact, canonical in pairs:
            compact_parts = normalize_compact_build_command(split_shell_command(compact))
            assert vars(self.parse(compact_parts)) == vars(self.parse(split_shell_command(canonical)))

    def test_build_namespace_constructs_typed_request_without_reparsing(self) -> None:
        spec, namespace = self.registry.parse(['create', 'module', 'Sample', '--project', 'Examples/Sandbox/Sandbox.dproject', '--kind', 'editor', '--link', 'static', '--enable', 'base', '--dry-run'])
        captured = []
        with mock.patch.object(handler, 'execute_request', side_effect=lambda request, **_kwargs: captured.append(request) or 0):
            result = handler.run(namespace, registry=self.registry, repository_root=REPOSITORY_ROOT, stdout=io.StringIO(), stderr=io.StringIO())
        assert result == 0
        request = captured[0]
        assert request.action is Action.CREATE_MODULE
        assert request.create_kind is CreateKind.MODULE
        assert request.module_kind is ModuleKind.EDITOR
        assert request.link_type is LinkType.STATIC
        assert request.output_mode is OutputMode.AUTO
        assert request.enablements == ('base',)

    def test_preset_command_preserves_inspection_semantics(self) -> None:
        spec, namespace = self.registry.parse(['preset', 'Win64-Debug-DurinEditor'])
        captured = []
        with mock.patch.object(handler, 'execute_request', side_effect=lambda request, **_kwargs: captured.append(request) or 0):
            result = handler.run(namespace, registry=self.registry, repository_root=REPOSITORY_ROOT, stdout=io.StringIO(), stderr=io.StringIO())
        assert result == 0
        assert captured[0].action is Action.PRESET
        assert captured[0].preset == 'Win64-Debug-DurinEditor'

    def test_shell_session_reuses_toolchain_after_preset_switch(self) -> None:
        base = mock.Mock()
        base.request = CommandRequest(Action.SHELL)
        base.preset.name = 'debug'
        base.current_host = 'windows'
        base.environment = None
        base.cmake = ''
        base.jobs = 0
        base.profile.name = 'profile'
        base.profile.presets = ('debug', 'release')
        base.config = LocalConfig()
        cached_environment = {'PATH': 'cached'}

        def prepare_environment(context: mock.Mock) -> None:
            context.environment = cached_environment

        def prepare_command(context: mock.Mock) -> None:
            context.cmake = 'cmake'
            context.jobs = 8

        def derive(context: mock.Mock, request: CommandRequest) -> mock.Mock:
            child = mock.Mock()
            child.request = request
            child.preset.name = request.preset
            child.target = request.target
            child.environment = context.environment
            child.cmake = context.cmake
            child.jobs = context.jobs
            child.config = context.config
            return child
        state: dict[str, object] = {}
        with mock.patch.object(build_operations, 'create_context', return_value=base), mock.patch.object(build_operations, 'prepare_toolchain_environment', side_effect=prepare_environment) as prepare_environment_context, mock.patch.object(build_operations, 'prepare_command_context', side_effect=prepare_command) as prepare_command_context, mock.patch.object(build_operations, 'derive_context', side_effect=derive), mock.patch.object(build_operations, 'execute_context') as execute:
            for request in (CommandRequest(Action.BUILD, target='Core', plain=True), CommandRequest(Action.PRESET, preset='release', plain=True), CommandRequest(Action.BUILD, target='Core', plain=True)):
                assert build_operations.execute_shell_request(request, session_state=state, stdout=io.StringIO(), stderr=io.StringIO()) == 0
        prepare_environment_context.assert_called_once_with(base)
        prepare_command_context.assert_called_once()
        assert [call.args[0].preset.name for call in execute.call_args_list] == ['debug', 'release']
        assert all((call.args[0].environment is cached_environment for call in execute.call_args_list))
