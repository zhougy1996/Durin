from . import build_request_fixtures as request_fixtures
import pytest
import io
import os
from pathlib import Path
from unittest import mock
REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
from durin_dev_tool.build import handler
from durin_dev_tool.build import operations as build_operations
from durin_dev_tool.build.models import Action, CreateKind, LinkType, LocalConfig, ModuleKind, OutputMode
from durin_dev_tool.build.requests import ConcreteRequest, OutputOptions, RequestContext
from durin_dev_tool.errors import DevToolError
from durin_dev_tool.registry import CommandRegistry
from durin_dev_tool.build.requests import LocationRequest, SimpleRequest
from durin_dev_tool.shell import run_shell, split_shell_command

EXPECTED_LOCATION_CONTRACT = (
    ('root', ()),
    ('build', ()),
    ('binaries', ('bin',)),
    ('output', ()),
    ('runtime', ()),
    ('tests', ()),
    ('logs', ()),
)
class TestBuildRegistry:

    @pytest.fixture(autouse=True)
    def _setup_registry(self) -> None:
        self.registry = CommandRegistry()

    def parse(self, arguments: list[str]):
        _spec, namespace = self.registry.parse(arguments)
        return namespace

    @staticmethod
    def shell_context() -> mock.Mock:
        context = mock.Mock()
        context.request = request_fixtures.command_request(Action.SHELL)
        context.preset.name = 'debug'
        context.current_host = 'windows'
        context.environment = None
        context.cmake = ''
        context.jobs = 0
        context.profile.name = 'profile'
        context.profile.default_preset = 'debug'
        context.profile.presets = ('debug', 'release')
        context.config = LocalConfig()
        return context

    def test_every_build_command_uses_the_build_handler(self) -> None:
        expected = {'stop', 'presets', 'preset', 'status', 'path', 'open', 'configure', 'build', 'clean', 'recover', 'purge', 'rebuild', 'test', 'run', 'create'}
        specifications = {specification.name: specification for specification in self.registry.specifications if specification.name in expected}
        assert set(specifications) == expected
        for name, specification in specifications.items():
            if name == 'create':
                assert specification.subcommands
                assert all((child.handler == 'durin_dev_tool.build.handler:run' for child in specification.subcommands))
            else:
                assert specification.handler == 'durin_dev_tool.build.handler:run'


    def test_direct_and_shell_entry_paths_dispatch_identical_requests(self) -> None:
        commands = ('stop --plain', 'presets --profile windows-msvc-x64 --preset win-msvc-x64-debug', 'preset win-msvc-x64-release --plain', 'status --jobs 8', 'path runtime --preset win-msvc-x64-debug', 'open runtime --preset win-msvc-x64-debug', 'configure --fresh -DFEATURE=ON --define LIMIT=4 --jobs 8', 'build --target Core --output compact', 'clean --plain', 'recover --cmake cmake', 'purge --all-presets --yes', 'rebuild --target all --agent', 'test CoreTests Core.* --timeout 45', 'test all --mode report --report Build/results.xml', 'run --project "Examples/Sandbox/Sandbox.dproject" --args --scene Sample', 'create module Sample --project Examples/Sandbox/Sandbox.dproject --kind editor --link static --public-dependency Core --enable base --dry-run', 'create project Sample --path Examples/Sample --dry-run')
        stdout = io.StringIO()
        stderr = io.StringIO()

        def acquire(
            request: ConcreteRequest,
            *,
            session: build_operations.BuildSession | None,
            repository_context: object,
        ):
            del repository_context
            current_preset = request.preset or 'current'
            return build_operations.AcquiredRequest(request, mock.sentinel.context, current_preset)

        with mock.patch.object(build_operations, 'acquire_request_context', side_effect=acquire) as acquire_context, mock.patch.object(build_operations, 'dispatch_request') as dispatch:
            for command in commands:
                specification, namespace = self.registry.parse(split_shell_command(command))
                assert self.registry.execute(
                    specification,
                    namespace,
                    repository_root=REPOSITORY_ROOT,
                    stdout=stdout,
                    stderr=stderr,
                ) == 0
            direct_requests = [call.args[0].request for call in dispatch.call_args_list]
            assert all(call.kwargs['session'] is None for call in acquire_context.call_args_list)

            dispatch.reset_mock()
            acquire_context.reset_mock()
            shell_input = []
            for command in commands:
                shell_input.append(command)
            shell_lines = iter((*shell_input, 'exit'))
            assert run_shell(
                registry=self.registry,
                repository_root=REPOSITORY_ROOT,
                stdout=stdout,
                stderr=stderr,
                input_func=lambda _prompt: next(shell_lines),
            ) == 0
            shell_requests = [call.args[0].request for call in dispatch.call_args_list]
            assert all(
                isinstance(call.kwargs['session'], build_operations.BuildSession)
                for call in acquire_context.call_args_list
            )

        assert direct_requests == shell_requests

    def test_native_test_positional_and_scenario_grammar(self) -> None:
        request = handler.request_from_namespace(
            self.parse(["test", "@domain=viewport+world,kind=feature", "--mode", "stress"])
        )
        assert request.target == "@domain=viewport+world,kind=feature"
        assert request.test_mode.value == "stress"

        focused = handler.request_from_namespace(
            self.parse(["test", "CoreUtilityTests", "Suite.Case"])
        )
        assert focused.target == "CoreUtilityTests"
        assert focused.test_filter == "Suite.Case"

        fast = handler.request_from_namespace(self.parse(["test", "fast-all"]))
        assert fast.target == "fast-all"

        listing = handler.request_from_namespace(self.parse(["test", "list", "viewport"]))
        assert listing.test_operation == "list"
        assert listing.test_query == "viewport"

        explaining = handler.request_from_namespace(
            self.parse(["test", "explain", "@viewport"])
        )
        assert explaining.test_operation == "explain"
        assert explaining.target == "@viewport"

        affected = handler.request_from_namespace(
            self.parse(["test", "affected", "--base", "origin/main", "--explain"])
        )
        assert affected.test_operation == "affected"
        assert affected.test_base == "origin/main"
        assert affected.test_explain_affected
        assert not affected.requires_toolchain

    def test_stage_zero_location_contract_has_stable_unique_names(self) -> None:
        canonical_names = tuple(name for name, _aliases in EXPECTED_LOCATION_CONTRACT)
        all_names = [
            candidate
            for name, aliases in EXPECTED_LOCATION_CONTRACT
            for candidate in (name, *aliases)
        ]
        assert canonical_names == (
            'root',
            'build',
            'binaries',
            'output',
            'runtime',
            'tests',
            'logs',
        )
        assert len(all_names) == len(set(all_names))


    def test_discovery_commands_reject_child_output_mode(self) -> None:
        for command in ('presets', 'status', 'path root', 'open root', 'purge'):
            with pytest.raises(DevToolError, match='unrecognized arguments'):
                self.registry.parse([*split_shell_command(command), '--output', 'full'])

    def test_removed_compatibility_inputs_are_rejected(self) -> None:
        for command in (
            'open-runtime',
            'test all --include-direct',
            'test --target CoreUtilityTests',
            'test all --granularity hybrid',
            'test all --ctest-regex Core',
            'test all --schedule-random',
            'test all --output-junit Build/results.xml',
        ):
            with pytest.raises(DevToolError):
                self.registry.parse(split_shell_command(command))

    def test_shell_uses_the_shared_direct_command_grammar(self) -> None:
        for command in ('build Core', 'rebuild Core', 'run --hidden-window'):
            with pytest.raises(DevToolError):
                self.registry.parse(split_shell_command(command))


    def test_path_request_requires_one_location_or_all(self) -> None:
        assert handler.request_from_namespace(self.parse(['path', 'root'])) == LocationRequest(
            action=Action.PATH, location='root'
        )
        assert handler.request_from_namespace(self.parse(['path', '--all'])) == LocationRequest(
            action=Action.PATH, all_locations=True
        )
        with pytest.raises(build_operations.BuildToolError, match='either one location or --all'):
            handler.request_from_namespace(self.parse(['path']))
        with pytest.raises(build_operations.BuildToolError, match='either one location or --all'):
            handler.request_from_namespace(self.parse(['path', 'root', '--all']))

    def test_help_accepts_nested_command_operands(self) -> None:
        specification, namespace = self.registry.parse(['help', 'worktree', 'add'])
        assert specification.name == 'help'
        assert namespace.command_path == ['worktree', 'add']

    def test_request_rejects_options_for_an_unrelated_action(self) -> None:
        with pytest.raises(TypeError, match='options'):
            SimpleRequest(action=Action.STATUS, options=request_fixtures.BuildActionOptions())

    def test_omitted_status_context_options_remain_empty(self) -> None:
        request = handler.request_from_namespace(self.parse(['status']))
        assert request.context == RequestContext()

    def test_build_commands_are_case_insensitive_and_keep_slash_aliases(self) -> None:
        canonical = vars(self.parse(['build', '--target', 'Core']))
        assert canonical == vars(self.parse(['BUILD', '--target', 'Core']))
        assert canonical == vars(self.parse(['/build', '--target', 'Core']))


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

    def test_developer_module_kind_is_typed(self) -> None:
        _spec, namespace = self.registry.parse(['create', 'module', 'Recipes', '--project', 'Engine/Engine.dproject', '--kind', 'developer', '--dry-run'])
        request = handler.request_from_namespace(namespace)
        assert request.module_kind is ModuleKind.DEVELOPER

    def test_agent_build_preset_selects_plain_compact_output_and_allows_override(self) -> None:
        request = handler.request_from_namespace(self.parse(['build', '--agent']))
        assert request.agent
        assert request.plain
        assert request.output_mode is OutputMode.COMPACT

        overridden = handler.request_from_namespace(
            self.parse(['build', '--agent', '--output', 'full'])
        )
        assert overridden.agent
        assert overridden.plain
        assert overridden.output_mode is OutputMode.FULL

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
        base.request = request_fixtures.command_request(Action.SHELL)
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

        def derive(context: mock.Mock, request: ConcreteRequest) -> mock.Mock:
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
        with mock.patch.object(build_operations, 'create_build_context', return_value=base), mock.patch.object(build_operations, 'prepare_toolchain_environment', side_effect=prepare_environment) as prepare_environment_context, mock.patch.object(build_operations, 'prepare_command_context', side_effect=prepare_command) as prepare_command_context, mock.patch.object(build_operations, 'derive_build_context', side_effect=derive), mock.patch.object(build_operations, 'execute_context') as execute:
            for request in (
                request_fixtures.command_request(Action.BUILD, output=OutputOptions(plain=True), options=request_fixtures.BuildActionOptions(target='Core')),
                request_fixtures.command_request(Action.PRESET, context=RequestContext(preset='release'), output=OutputOptions(plain=True)),
                request_fixtures.command_request(Action.BUILD, output=OutputOptions(plain=True), options=request_fixtures.BuildActionOptions(target='Core')),
            ):
                assert build_operations.execute_request(request, session_state=state, stdout=io.StringIO(), stderr=io.StringIO()) == 0
        prepare_environment_context.assert_called_once_with(base)
        prepare_command_context.assert_called_once()
        assert [call.args[0].preset.name for call in execute.call_args_list] == ['debug', 'release']
        assert all((call.args[0].environment is cached_environment for call in execute.call_args_list))
        assert state == {
            'build_session': build_operations.BuildSession(base, 'release'),
        }

    def test_presets_lists_and_explicit_numeric_preset_selects(self) -> None:
        base = self.shell_context()
        stdout = io.StringIO()
        stderr = io.StringIO()
        prompts: list[str] = []
        responses = iter(('presets', 'preset 2', 'preset', 'exit'))

        def read_input(prompt: str) -> str:
            prompts.append(prompt)
            return next(responses)

        with mock.patch.object(build_operations, 'create_build_context', return_value=base), mock.patch.object(
            build_operations, 'derive_build_context', return_value=base
        ):
            assert run_shell(
                registry=self.registry,
                repository_root=REPOSITORY_ROOT,
                stdout=stdout,
                stderr=stderr,
                input_func=read_input,
            ) == 0

        assert prompts == ['DurinDevTool> '] * 4
        assert 'CMake preset selected: "release"' in stdout.getvalue()
        assert 'CMake preset: "release"' in stdout.getvalue()
        assert stderr.getvalue() == ''

    def test_command_after_presets_is_parsed_normally(self) -> None:
        base = self.shell_context()
        stdout = io.StringIO()
        stderr = io.StringIO()
        responses = iter(('presets', 'status', 'exit'))

        with mock.patch.object(build_operations, 'create_build_context', return_value=base), mock.patch.object(
            build_operations, 'derive_build_context', return_value=base
        ), mock.patch.object(build_operations, 'show_status') as show_status:
            assert run_shell(
                registry=self.registry,
                repository_root=REPOSITORY_ROOT,
                stdout=stdout,
                stderr=stderr,
                input_func=lambda _prompt: next(responses),
            ) == 0

        show_status.assert_called_once()
        assert stderr.getvalue() == ''

    def test_direct_numeric_preset_uses_registered_order(self) -> None:
        base = self.shell_context()
        stdout = io.StringIO()
        stderr = io.StringIO()
        request = request_fixtures.command_request(
            Action.PRESET,
            context=RequestContext(preset='2'),
            output=OutputOptions(plain=True),
        )
        with mock.patch.object(build_operations, 'create_build_context', return_value=base):
            assert build_operations.execute_request(
                request,
                stdout=stdout,
                stderr=stderr,
            ) == 0
        assert 'CMake preset selected: "release"' in stdout.getvalue()
        assert stderr.getvalue() == ''

    def test_nested_help_uses_registered_leaf_parser(self) -> None:
        build_help = self.registry.format_command_help(('build',))
        worktree_help = self.registry.format_command_help(('worktree', 'add'))
        assert 'DevTool build' in build_help
        assert '--target' in build_help
        assert 'DevTool worktree add' in worktree_help
        assert '--branch' in worktree_help

    def test_shell_bare_group_displays_help_and_continues(self) -> None:
        stdout = io.StringIO()
        stderr = io.StringIO()
        responses = iter(('dependency', 'exit'))
        assert run_shell(
            registry=self.registry,
            repository_root=REPOSITORY_ROOT,
            stdout=stdout,
            stderr=stderr,
            input_func=lambda _prompt: next(responses),
        ) == 0
        assert 'DevTool dependency' in stdout.getvalue()
        assert stderr.getvalue() == ''

    def test_styled_preset_table_highlights_state_without_changing_plain_output(self) -> None:
        context = self.shell_context()
        styled_stdout = io.StringIO()
        with mock.patch.dict(os.environ):
            os.environ.pop('NO_COLOR', None)
            os.environ['TERM'] = 'xterm-256color'
            styled_output = build_operations.BuildOutput(
                stdout=styled_stdout,
                stderr=io.StringIO(),
                force_terminal=True,
            )
        build_operations.show_presets(styled_output, context, 'debug')

        plain_stdout = io.StringIO()
        plain_output = build_operations.BuildOutput(
            plain=True,
            stdout=plain_stdout,
            stderr=io.StringIO(),
        )
        build_operations.show_presets(plain_output, context, 'debug')

        assert '\x1b[' in styled_stdout.getvalue()
        assert '\x1b[1;32m' in styled_stdout.getvalue()
        assert plain_stdout.getvalue().splitlines() == [
            '   1  debug [default, current]',
            '   2  release',
        ]
