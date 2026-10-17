"""Configuration-first selection validates candidates before offering text choices."""

import io
import json
import os
import subprocess
from pathlib import Path
from unittest import mock

import pytest

from durin_dev_tool import toolchain
from durin_dev_tool.bootstrap import preflight, setup, toolchain_setup as wizard
from durin_dev_tool.bootstrap.models import BootstrapError
from durin_dev_tool.bootstrap.preflight import PreflightError
from durin_dev_tool.bootstrap.toolchain_selection import DEFAULT_ENVIRONMENT_ARGUMENTS
from durin_dev_tool.context import CommandIO, RepositoryContext


@pytest.fixture
def command_io():
    return CommandIO(io.StringIO(), io.StringIO())


def make_script(root):
    script = root / 'Common7/Tools/VsDevCmd.bat'
    script.parent.mkdir(parents=True)
    script.touch()
    return script


def test_vswhere_lists_all_cpp_installations_in_order(tmp_path):
    vswhere = tmp_path / 'Microsoft Visual Studio/Installer/vswhere.exe'
    vswhere.parent.mkdir(parents=True)
    vswhere.touch()
    first = make_script(tmp_path / 'new VS')
    second = make_script(tmp_path / 'older VS')
    completed = subprocess.CompletedProcess([], 0, f'{first.parents[2]}\n{tmp_path / "incomplete"}\n{second.parents[2]}\n', '')
    with mock.patch.object(toolchain, '_program_files_roots', return_value=(tmp_path,)), mock.patch.object(toolchain.subprocess, 'run', return_value=completed) as run:
        assert toolchain.find_vsdevcmds({}) == (first, second)
    arguments = run.call_args.args[0]
    assert '-sort' in arguments and '-latest' not in arguments
    assert 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' in arguments


def test_vs_candidates_deduplicate(tmp_path, command_io):
    script = make_script(tmp_path / 'VS')
    with mock.patch.object(wizard, 'find_vsdevcmds', return_value=(script, script)):
        assert wizard.visual_studio_candidates(command_io) == (wizard.Candidate(script, 'Visual Studio installation'),)


def test_failed_discovery_returns_no_candidates(tmp_path, command_io):
    with mock.patch.object(wizard, 'find_vsdevcmds', side_effect=PreflightError('no installer')):
        assert wizard.visual_studio_candidates(command_io) == ()
    assert 'no installer' in command_io.stdout.getvalue()


def test_cmake_candidates_include_vs_and_every_path_installation(tmp_path):
    script = make_script(tmp_path / 'VS')
    bundled = script.parents[2] / 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
    first = tmp_path / 'one/cmake.exe'
    second = tmp_path / 'two/cmake.exe'
    for path in (bundled, first, second):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
    environment = {'Path': os.pathsep.join(map(str, (first.parent, second.parent, first.parent)))}
    candidates = wizard.cmake_candidates(script, environment)
    assert [candidate.path for candidate in candidates] == [bundled, first, second]
    assert [candidate.source for candidate in candidates] == ['selected Visual Studio', 'PATH', 'PATH']


def test_custom_environment_script_uses_reported_vs_installation(tmp_path):
    installation = tmp_path / 'VS'
    bundled = installation / 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
    bundled.parent.mkdir(parents=True)
    bundled.touch()
    with mock.patch.dict(os.environ, {}, clear=True):
        candidates = wizard.cmake_candidates(Path('custom.bat'), {'VSINSTALLDIR': str(installation)})
        assert [candidate.path for candidate in candidates] == [bundled]
        assert wizard.cmake_candidates(Path('custom.bat'), {}) == ()


def component(command_io, discover, validate, *, interactive=True, preferred=None, directory=False):
    return wizard._select_component('Tool', discover, validate, command_io,
                                    preferred=preferred, interactive=interactive, field='test.field', directory=directory)


@pytest.mark.parametrize('interactive', [False, True])
def test_single_valid_candidate_is_automatic_and_invalid_candidates_are_excluded(tmp_path, command_io, interactive):
    candidates = [wizard.Candidate(tmp_path / name, 'discovered') for name in ('old', 'good')]
    validate = mock.Mock(side_effect=[PreflightError('too old'), 'ready'])
    with mock.patch('builtins.input', side_effect=AssertionError('must not prompt')):
        assert component(command_io, lambda: candidates, validate, interactive=interactive) == 'ready'
    assert validate.call_count == 2
    assert 'Skipping Tool' in command_io.stdout.getvalue()
    assert 'Automatically selected Tool' in command_io.stdout.getvalue()


def test_multiple_valid_candidates_require_an_explicit_number_and_reuse_validation(tmp_path, command_io):
    candidates = [wizard.Candidate(tmp_path / name, 'PATH') for name in ('first', 'second')]
    validate = mock.Mock(side_effect=['one', 'two'])
    with mock.patch('builtins.input', side_effect=['', '0', '99', '2']):
        assert component(command_io, lambda: candidates, validate) == 'two'
    assert validate.call_count == 2
    assert 'Usable Tool candidates' in command_io.stdout.getvalue()


@pytest.mark.parametrize('count', [0, 2])
def test_noninteractive_missing_or_ambiguous_candidates_are_actionable(tmp_path, command_io, count):
    candidates = [wizard.Candidate(tmp_path / str(number), 'PATH') for number in range(count)]
    with mock.patch('builtins.input', side_effect=AssertionError('must not prompt')), pytest.raises(BootstrapError, match='test.field') as raised:
        component(command_io, lambda: candidates, str, interactive=False)
    assert 'setup --check' in str(raised.value)


def test_valid_explicit_configuration_bypasses_discovery(tmp_path, command_io):
    preferred = wizard.Candidate(tmp_path / 'configured', 'configuration')
    discover = mock.Mock(side_effect=AssertionError('must not discover'))
    with mock.patch('builtins.input', side_effect=AssertionError('must not prompt')):
        assert component(command_io, discover, str, preferred=preferred) == str(preferred.path)


def test_invalid_explicit_configuration_is_not_silently_replaced_noninteractively(tmp_path, command_io):
    discover = mock.Mock(side_effect=AssertionError('must not discover'))
    with pytest.raises(BootstrapError, match='Configured Tool is not usable'):
        component(command_io, discover, mock.Mock(side_effect=PreflightError('bad path')),
                  preferred=wizard.Candidate(tmp_path / 'missing', 'configuration'), interactive=False)


def test_interactive_invalid_configuration_can_recover_with_auto_discovery(tmp_path, command_io):
    preferred = wizard.Candidate(tmp_path / 'old', 'configuration')
    candidates = [preferred, wizard.Candidate(tmp_path / 'good', 'PATH')]
    validate = mock.Mock(side_effect=[PreflightError('old installation'), 'ready'])
    with mock.patch('builtins.input', side_effect=AssertionError('single valid candidate must not prompt')):
        assert component(command_io, lambda: candidates, validate, preferred=preferred) == 'ready'
    assert validate.call_count == 2


def test_text_fallback_retries_and_accepts_quoted_expanded_installation_directory(tmp_path, command_io):
    script = make_script(tmp_path / 'VS installation')
    validate = mock.Mock(side_effect=[PreflightError('wrong path'), script])
    with mock.patch.dict(os.environ, {'DURIN_SETUP_TEST_ROOT': str(script.parents[2])}), mock.patch('builtins.input', side_effect=['', 'wrong', '"$DURIN_SETUP_TEST_ROOT"']):
        assert component(command_io, lambda: (), validate, directory=True) == script
    assert validate.call_args.args[0] == script


def test_eof_is_actionable_and_does_not_loop(command_io):
    with mock.patch('builtins.input', side_effect=EOFError), pytest.raises(BootstrapError, match='setup --check'):
        component(command_io, lambda: (), str)


def test_cmake_retry_preserves_validated_saved_vs_and_custom_arguments(tmp_path, command_io):
    repository = RepositoryContext.load().at_root(tmp_path)
    script = make_script(tmp_path / 'VS')
    executable = tmp_path / 'cmake.exe'
    arguments = ('-arch=x64', '-host_arch=x64', '-vcvars_ver=14.44')
    with mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(wizard, 'visual_studio_candidates', side_effect=AssertionError('saved VS must be reused')), mock.patch.object(wizard, 'cmake_candidates', return_value=()), mock.patch('builtins.input', side_effect=['wrong', str(executable)]), mock.patch.object(wizard, 'capture_windows_environment', return_value={}) as capture, mock.patch.object(preflight, 'check_msvc_version', return_value=None), mock.patch.object(preflight, 'find_ninja', return_value='ninja'), mock.patch.object(wizard, 'resolve_cmake_executable', side_effect=[PreflightError('missing executable'), str(executable)]), mock.patch.object(wizard, 'check_cmake', return_value=None):
        selection = wizard.select_windows_toolchain(repository, command_io, configured_script=script, configured_cmake='', configured_arguments=arguments, interactive=True)
    capture.assert_called_once_with(script, arguments)
    assert selection.environment_arguments == arguments
    assert selection.cmake_command == str(executable)


def test_discovered_vs_honors_explicit_arguments_without_a_script_override(tmp_path, command_io):
    script = make_script(tmp_path / 'VS')
    executable = tmp_path / 'cmake.exe'
    arguments = ('-arch=x64', '-vcvars_ver=14.44')
    with mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(wizard, 'visual_studio_candidates', return_value=(wizard.Candidate(script, 'installation'),)), mock.patch.object(wizard, 'capture_windows_environment', return_value={}) as capture, mock.patch.object(preflight, 'check_msvc_version', return_value=None), mock.patch.object(preflight, 'find_ninja', return_value='ninja'), mock.patch.object(wizard, 'find_command', return_value=str(executable)), mock.patch.object(wizard, 'resolve_cmake_executable', return_value=str(executable)), mock.patch.object(wizard, 'check_cmake', return_value=None):
        selected = wizard.select_windows_toolchain(RepositoryContext.load().at_root(tmp_path), command_io, configured_script=None, configured_cmake=str(executable), configured_arguments=arguments, interactive=False)
    capture.assert_called_once_with(script, arguments)
    assert selected.environment_arguments == arguments


@pytest.mark.parametrize('msvc_error,ninja', [('MSVC too old', 'ninja'), (None, None)])
def test_invalid_compiler_or_missing_ninja_are_excluded(tmp_path, command_io, msvc_error, ninja):
    repository = RepositoryContext.load().at_root(tmp_path)
    script = make_script(tmp_path / 'VS')
    with mock.patch.object(wizard, 'visual_studio_candidates', return_value=(wizard.Candidate(script, 'installation'),)), mock.patch.object(wizard, 'capture_windows_environment', return_value={}), mock.patch.object(preflight, 'check_msvc_version', return_value=msvc_error), mock.patch.object(preflight, 'find_ninja', return_value=ninja), mock.patch.object(wizard, 'cmake_candidates') as cmakes, pytest.raises(BootstrapError, match='No usable candidate'):
        wizard.select_windows_toolchain(repository, command_io, configured_script=None, configured_cmake='', configured_arguments=[], interactive=False)
    cmakes.assert_not_called()


@pytest.mark.parametrize('interactive', [False, True])
def test_windows_router_passes_configuration_and_interaction_mode(tmp_path, command_io, interactive):
    config = tmp_path / '.agents/DevTool.user.json'
    config.parent.mkdir()
    script = make_script(tmp_path / 'VS')
    config.write_text(json.dumps({'version': 1, 'cmake': {'command': 'cmake'}, 'toolchain': {'environmentScript': str(script)}}), encoding='utf-8')
    with mock.patch.object(setup.sys, 'platform', 'win32'), mock.patch.object(wizard, 'select_windows_toolchain', return_value=mock.sentinel.selection) as select:
        assert setup.select_setup_toolchain(tmp_path, command_io, interactive=interactive) is mock.sentinel.selection
    assert select.call_args.kwargs == dict(configured_script=script, configured_cmake='cmake', configured_arguments=[], interactive=interactive)


def test_environment_cmake_override_takes_precedence_over_saved_config(tmp_path, command_io):
    repository = RepositoryContext.load().at_root(tmp_path)
    script = make_script(tmp_path / 'VS')
    override = tmp_path / 'override.exe'
    with mock.patch.dict(os.environ, {'CMAKE_COMMAND': str(override)}), mock.patch.object(wizard, 'capture_windows_environment', return_value={}), mock.patch.object(preflight, 'check_msvc_version', return_value=None), mock.patch.object(preflight, 'find_ninja', return_value='ninja'), mock.patch.object(wizard, 'find_command', return_value=str(override)), mock.patch.object(wizard, 'resolve_cmake_executable', return_value=str(override)) as resolve, mock.patch.object(wizard, 'check_cmake', return_value=None), mock.patch('builtins.input', side_effect=AssertionError('must not prompt')):
        selection = wizard.select_windows_toolchain(repository, command_io, configured_script=script, configured_cmake='saved.exe', configured_arguments=[], interactive=False)
    resolve.assert_called_once_with(str(override), {'VSLANG': '1033'})
    assert selection.cmake_command == str(override)
