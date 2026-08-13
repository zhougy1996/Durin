from . import build_request_fixtures as request_fixtures
import pytest
import io
import os
from pathlib import Path
from unittest import mock
REPO_ROOT = Path(__file__).resolve().parents[3]
DEV_TOOL_DIR = REPO_ROOT / 'Tools' / 'DurinDevTool'
if str(DEV_TOOL_DIR) not in os.sys.path:
    os.sys.path.insert(0, str(DEV_TOOL_DIR))
from durin_dev_tool.build import config as build_config
from durin_dev_tool.build import core as build_core
from durin_dev_tool.build.handler import request_from_namespace
from durin_dev_tool.build.output import BuildOutput
from durin_dev_tool.registry import CommandRegistry

def parse_build_request(arguments: list[str]) -> build_config.ConcreteRequest:
    _spec, namespace = CommandRegistry().parse(arguments)
    if getattr(namespace, 'selected_preset', ''):
        namespace.preset = namespace.selected_preset
    return request_from_namespace(namespace)


class TestCore:
    def make_profile(self) -> build_config.BuildProfile:
        return build_config.BuildProfile('test-profile', 'windows', 'debug', ('debug', 'release'), build_config.EnvironmentProvider.INHERIT, 'Win64', '.exe', True, ())
    def make_preset(self, name: str='debug', testing: str='ON', runtime_variant: str='DurinEditor') -> build_config.ConfigurePreset:
        return build_config.ConfigurePreset(name, {'name': name, 'binaryDir': '${sourceDir}/Build/${presetName}', 'cacheVariables': {'CMAKE_BUILD_TYPE': 'Debug', 'DURIN_RUNTIME_VARIANT': runtime_variant, 'BUILD_TESTING': testing}})
    def test_native_test_failure_does_not_leave_recovery_marker(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = self.make_preset()
        context = build_config.BuildContext(request_fixtures.command_request(build_config.Action.TEST, options=request_fixtures.TestActionOptions(target='CoreTests')), build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=1, environment={})
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        marker = root / 'interrupted.json'
        lock = root / 'checkout.lock'
        with mock.patch.object(build_core, 'interruption_marker_path', return_value=marker), mock.patch.object(build_core, 'lock_file_path', return_value=lock), mock.patch.object(build_core, 'perform_action'), mock.patch.object(build_core, 'run_native_test', side_effect=build_config.BuildToolError('test failed')), pytest.raises(build_config.BuildToolError, match='test failed'):
            build_core.execute_context(context, output, confirm_purge=lambda _paths, _all: False)
        assert not marker.exists()
    def test_all_native_test_failure_does_not_leave_recovery_marker(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = self.make_preset()
        context = build_config.BuildContext(request_fixtures.command_request(build_config.Action.TEST, options=request_fixtures.TestActionOptions(target='all')), build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=1, environment={})
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        root = Path(tmp_path_factory.mktemp('case'))
        marker = root / 'interrupted.json'
        lock = root / 'checkout.lock'
        with mock.patch.object(build_core, 'interruption_marker_path', return_value=marker), mock.patch.object(build_core, 'lock_file_path', return_value=lock), mock.patch.object(build_core, 'perform_action'), mock.patch.object(build_core, 'run_all_native_tests', side_effect=build_config.BuildToolError('test failed')), pytest.raises(build_config.BuildToolError, match='test failed'):
            build_core.execute_context(context, output, confirm_purge=lambda _paths, _all: False)
        assert not marker.exists()
