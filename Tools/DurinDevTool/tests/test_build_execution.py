from . import build_request_fixtures as request_fixtures
import pytest
import io
from pathlib import Path
from unittest import mock
from durin_dev_tool.build import config as build_config
from durin_dev_tool.build import core as build_core
from durin_dev_tool.build.output import BuildOutput


class TestCore:
    make_profile = staticmethod(request_fixtures.make_profile)
    make_preset = staticmethod(request_fixtures.make_preset)

    def test_native_test_failure_does_not_leave_recovery_marker(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = self.make_preset()
        context = build_config.BuildContext(request_fixtures.command_request(build_config.Action.TEST, options=request_fixtures.TestActionOptions(target='CoreTests')), build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=1, environment={})
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        marker = root / 'interrupted.json'
        lock = root / 'checkout.lock'
        with mock.patch.object(build_core, 'interruption_marker_path', return_value=marker), mock.patch.object(build_core, 'lock_file_path', return_value=lock), mock.patch.object(build_core, 'perform_action'), mock.patch.object(build_core, 'run_exact_native_test', side_effect=build_config.BuildToolError('test failed')), pytest.raises(build_config.BuildToolError, match='test failed'):
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
