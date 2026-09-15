from . import build_request_fixtures as request_fixtures
import pytest
import io
from pathlib import Path
from types import SimpleNamespace
from unittest import mock
from durin_dev_tool.build import build_context, errors, models
from durin_dev_tool.build import core as build_core
from durin_dev_tool.build import operations, runtime, request_validation
from durin_dev_tool.build.requests import NativeTestRequest
from durin_dev_tool.build.output import BuildOutput


class TestCore:
    make_profile = staticmethod(request_fixtures.make_profile)
    make_preset = staticmethod(request_fixtures.make_preset)

    @pytest.mark.parametrize("selection", ["bounded", "all", "empty"])
    @pytest.mark.parametrize("custom_report", [False, True])
    def test_affected_preserves_scheduling_and_report_identity(self, tmp_path, selection, custom_report) -> None:
        report = tmp_path / "custom.xml" if custom_report else None
        request = NativeTestRequest(target="affected", test_operation="affected",
            test_report_enabled=True, test_report_path=report, test_parallel_jobs=3)
        preset = self.make_preset()
        request_validation.validate_request(request, preset)
        context = build_context.BuildContext(request, models.LocalConfig(), self.make_profile(),
            {'debug': preset}, preset, 'windows', cmake='cmake', jobs=8, environment={})
        affected = SimpleNamespace(run_all=selection == "all", targets=() if selection == "empty" else (object(),),
            names=("CoreTests",), reasons=("test impact",))
        output_stream = io.StringIO()
        output = BuildOutput(plain=True, stdout=output_stream)
        acquired = SimpleNamespace(request=request, context=context)
        with mock.patch.object(operations, 'discover_changed_paths', return_value=('Engine/Source/Core.cpp',)), \
             mock.patch.object(operations, 'load_affected_registry'), \
             mock.patch.object(operations, 'analyze_affected_tests', return_value=affected), \
             mock.patch.object(operations, 'execute_context') as execute:
            operations.dispatch_request(acquired, output, repository=SimpleNamespace(root=tmp_path))
        if selection == "empty":
            execute.assert_not_called()
            assert 'No report written' in output_stream.getvalue()
            assert not list(tmp_path.rglob('*.xml'))
            return
        execute.assert_called_once()
        assert context.request.test_parallel_jobs == 3
        assert context.jobs == 8
        assert context.request.target == ('all' if selection == 'all' else 'affected')
        with mock.patch.object(runtime, '_context_paths', return_value=SimpleNamespace(root=tmp_path)):
            expected = report or tmp_path / 'Build/NativeTestResults/debug/affected.xml'
            assert runtime._selected_report_path(context) == expected

    def test_native_test_failure_does_not_leave_recovery_marker(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = self.make_preset()
        context = build_context.BuildContext(request_fixtures.command_request(models.Action.TEST, options=request_fixtures.TestActionOptions(target='CoreTests')), models.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=1, environment={})
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        marker = root / 'interrupted.json'
        lock = root / 'checkout.lock'
        with mock.patch.object(build_core, 'interruption_marker_path', return_value=marker), mock.patch.object(build_core, 'lock_file_path', return_value=lock), mock.patch.object(build_core, 'perform_action'), mock.patch.object(build_core, 'run_exact_native_test', side_effect=errors.BuildToolError('test failed')), pytest.raises(errors.BuildToolError, match='test failed'):
            build_core.execute_context(context, output, confirm_purge=lambda _paths, _all: False)
        assert not marker.exists()
    def test_all_native_test_failure_does_not_leave_recovery_marker(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = self.make_preset()
        context = build_context.BuildContext(request_fixtures.command_request(models.Action.TEST, options=request_fixtures.TestActionOptions(target='all')), models.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=1, environment={})
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        root = Path(tmp_path_factory.mktemp('case'))
        marker = root / 'interrupted.json'
        lock = root / 'checkout.lock'
        with mock.patch.object(build_core, 'interruption_marker_path', return_value=marker), mock.patch.object(build_core, 'lock_file_path', return_value=lock), mock.patch.object(build_core, 'perform_action'), mock.patch.object(build_core, 'run_all_native_tests', side_effect=errors.BuildToolError('test failed')), pytest.raises(errors.BuildToolError, match='test failed'):
            build_core.execute_context(context, output, confirm_purge=lambda _paths, _all: False)
        assert not marker.exists()
