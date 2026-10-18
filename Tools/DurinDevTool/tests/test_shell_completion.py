"""Interactive completion uses the current grammar and repository selectors."""

from pathlib import Path
from unittest import mock

import pytest

from durin_dev_tool.context import RepositoryContext
from durin_dev_tool.registry import CommandRegistry
from durin_dev_tool.shell import shell_completion_candidates


ROOT = Path(__file__).resolve().parents[3]


@pytest.fixture(autouse=True)
def windows_profile_host():
    with mock.patch('durin_dev_tool.build.selection.host_name', return_value='windows'):
        yield


def complete(line, **kwargs):
    return shell_completion_candidates(CommandRegistry(), line, **kwargs)


@pytest.mark.parametrize('line', ['preset Win64-Release', 'build --preset Win64-Release', 'build Core --preset WIN64-RELEASE'])
def test_full_presets_complete_without_preparing_toolchain(line):
    with mock.patch('durin_dev_tool.build.operations.prepare_toolchain_context') as prepare:
        assert complete(line) == ('Win64-Release-DurinEditor', 'Win64-Release-DurinGame')
    prepare.assert_not_called()


def test_preset_numbers_full_names_and_equals_values():
    assert complete('preset 2') == ('2',)
    assert complete('build --preset=Win64-Release') == ('--preset=Win64-Release-DurinEditor', '--preset=Win64-Release-DurinGame')
    assert complete('build --preset Win64-Release-DurinE') == ('Win64-Release-DurinEditor',)
    assert complete('preset --profile=windows-msvc-x64 Win64-Release') == ('Win64-Release-DurinEditor', 'Win64-Release-DurinGame')
    assert complete('build --profile invalid --preset Win64-Release') == ()


@pytest.mark.parametrize('line', ['preset rel', 'build --preset debug-', 'build --preset=release-'])
def test_preset_aliases_are_not_completed(line):
    assert complete(line) == ()


def test_completion_uses_session_profile_and_explicit_override():
    from durin_dev_tool.build.config_io import load_profiles
    from durin_dev_tool.build.settings import BuildPaths
    from types import SimpleNamespace
    paths = BuildPaths.from_repository(RepositoryContext.load(ROOT))
    profiles = load_profiles(paths.profile_file)
    session = {'build_session': SimpleNamespace(context=SimpleNamespace(profile=profiles['macos-xcode-arm64']))}
    assert complete('preset Mac', session_state=session)
    assert complete('preset Win', session_state=session) == ()
    assert complete('preset --profile windows-msvc-x64 Win', session_state=session)


@pytest.mark.parametrize('flag', ['--deps', '--public-deps', '--optional-deps', '--optional-public-deps'])
def test_dependency_lists_complete_every_value_and_stop_at_options(flag):
    line = f'create module Gameplay --project Sandbox/Sandbox.dproject {flag}'
    assert 'Core' in complete(line + ' Co')
    assert 'Engine' in complete(line + ' Core En')
    assert 'Engine' in complete(line + ' Core InputCore En')
    assert complete(line + '=En') == ('--' + flag[2:] + '=Engine',)
    assert complete(line + ' Core --kind ed') == ('editor',)
    assert '--public-deps' in complete(line + ' Core --public')
    assert 'Core' in complete(line + ' Core --public-deps Co')


@pytest.mark.parametrize('line', ['build --h', 'create --h', 'create module Demo --deps Core --h', 'doc --h'])
def test_automatic_help_option_is_offered(line):
    assert complete(line) == ('--help',)


def test_help_paths_and_current_options_exclude_retired_syntax():
    assert complete('help create mo') == ('module',)
    assert complete('help doc plan val') == ('validate',)
    assert complete('test exp') == ()
    for line in ('build --target', 'build --plain', 'build --output', 'build --agent',
                 'create module Demo --private-dependency', 'create module Demo --public-dependency',
                 'create module Demo --optional-private-dependency', 'create module Demo --optional-public-dependency',
                 'asset resave --apply', 'test --parallel'):
        assert complete(line) == ()
    assert complete('run -- --preset Win64-Release') == ()


def test_cursor_position_and_incomplete_configuration_are_safe():
    line = 'build --preset Win64-Release Core'
    assert shell_completion_candidates(CommandRegistry(), line, len('build --preset Win64-Release')) == ('Win64-Release-DurinEditor', 'Win64-Release-DurinGame')
    repository = RepositoryContext.load(ROOT).at_root(ROOT / 'Build' / 'MissingCompletionCheckout')
    assert complete('preset Win64-Release', repository_context=repository) == ()
    assert complete('create module Demo --deps Co', repository_context=repository) == ()
    assert complete('build --h', repository_context=repository) == ('--help',)


def test_prompt_toolkit_completion_replaces_only_the_current_prefix():
    from prompt_toolkit.document import Document
    from durin_dev_tool.shell import _interactive_input
    repository = RepositoryContext.load(ROOT)
    state = {}
    with mock.patch('durin_dev_tool.shell.sys.stdin.isatty', return_value=True), mock.patch('prompt_toolkit.PromptSession') as prompt:
        _interactive_input(CommandRegistry(), mock.Mock(isatty=lambda: True), repository_context=repository, session_state=state)
    completer = prompt.call_args.kwargs['completer']
    matches = list(completer.get_completions(Document('build --preset=Win64-Release'), None))
    assert [item.text for item in matches] == ['--preset=Win64-Release-DurinEditor', '--preset=Win64-Release-DurinGame']
    assert all(item.start_position == -len('--preset=Win64-Release') for item in matches)


def test_readline_completion_uses_the_same_repository_values():
    import sys
    from durin_dev_tool.shell import _configure_readline_completion
    readline = mock.Mock(__doc__='readline', get_line_buffer=lambda: 'create module Demo --deps Core En', get_endidx=lambda: 33)
    with mock.patch.dict(sys.modules, {'readline': readline}):
        _configure_readline_completion(CommandRegistry(), repository_context=RepositoryContext.load(ROOT))
    complete_value = readline.set_completer.call_args.args[0]
    assert complete_value('', 0) == 'Engine'
    assert complete_value('', 1) is None


def test_shell_passes_live_session_state_to_completion():
    import io
    from durin_dev_tool.shell import run_shell
    repository = RepositoryContext.load(ROOT)
    read = mock.Mock(side_effect=['preset 2', 'exit'])
    with mock.patch('durin_dev_tool.shell._interactive_input', return_value=read) as interactive:
        assert run_shell(registry=CommandRegistry(), repository_root=ROOT, repository_context=repository,
                         stdout=io.StringIO(), stderr=io.StringIO()) == 0
    context = interactive.call_args.kwargs
    assert context['repository_context'] is repository
    session = context['session_state']['build_session']
    assert session.preset == session.context.profile.presets[1]
