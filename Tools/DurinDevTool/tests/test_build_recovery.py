from . import build_request_fixtures as request_fixtures
import pytest
import io
import json
from pathlib import Path
from unittest import mock
from durin_dev_tool.build import build_context, errors, models, requests, settings
from durin_dev_tool.build import core as build_core
from durin_dev_tool.build import recovery as build_recovery
from durin_dev_tool.build.output import BuildOutput


class TestCore:
    make_profile = staticmethod(request_fixtures.make_profile)
    make_preset = staticmethod(request_fixtures.make_preset)

    def test_interruption_marker_requires_rebuild(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        marker = Path(directory) / 'interrupted.json'

        def interrupt(_target_override: str | None) -> None:
            raise errors.BuildToolInterruptedError('interrupted')
        with pytest.raises(errors.BuildToolInterruptedError):
            build_recovery.execute_with_recovery_marker(action=models.Action.BUILD, marker_file=marker, metadata={'pid': 1, 'action': 'build', 'target': 'Core'}, operation=interrupt)
        with pytest.raises(errors.BuildToolError, match='did not return normally') as blocked:
            build_recovery.execute_with_recovery_marker(action=models.Action.BUILD, marker_file=marker, metadata={'pid': 1, 'action': 'build', 'target': 'Core'}, operation=lambda _target: None)
        assert 'run recover' in blocked.value.recovery
        build_recovery.execute_with_recovery_marker(action=models.Action.REBUILD, marker_file=marker, metadata={'pid': 1, 'action': 'rebuild', 'target': 'Core'}, operation=lambda _target: None)
        assert not marker.exists()
    def test_rebuild_rejects_an_unrelated_recovery_target(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        marker = Path(directory) / 'interrupted.json'
        marker.write_text(json.dumps({'pid': 1, 'action': 'build', 'target': 'Core'}), encoding='utf-8')
        operation = mock.Mock()
        with pytest.raises(errors.BuildToolError, match='Interrupted target "Core"'):
            build_recovery.execute_with_recovery_marker(action=models.Action.REBUILD, marker_file=marker, metadata={'pid': 2, 'action': 'rebuild', 'target': 'Editor'}, operation=operation)
        operation.assert_not_called()
        assert build_recovery.recovery_target(marker) == 'Core'
    def test_selected_target_set_is_recoverable_as_one_build(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        marker = Path(directory) / 'interrupted.json'
        marker.write_text(
            json.dumps({'pid': 1, 'action': 'test', 'target': 'WorldTests;ViewportTests'}),
            encoding='utf-8',
        )
        operation = mock.Mock()
        build_recovery.execute_with_recovery_marker(
            action=models.Action.RECOVER,
            marker_file=marker,
            metadata={'pid': 2, 'action': 'recover', 'target': 'recorded-target'},
            operation=operation,
        )
        operation.assert_called_once_with('WorldTests;ViewportTests')
    def test_rebuild_all_does_not_claim_to_recover_an_excluded_test_target(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        marker = Path(directory) / 'interrupted.json'
        marker.write_text(json.dumps({'pid': 1, 'action': 'test', 'target': 'DurinNativeTests'}), encoding='utf-8')
        operation = mock.Mock()
        with pytest.raises(errors.BuildToolError, match='DurinNativeTests') as raised:
            build_recovery.execute_with_recovery_marker(action=models.Action.REBUILD, marker_file=marker, metadata={'pid': 2, 'action': 'rebuild', 'target': 'all'}, operation=operation)
        operation.assert_not_called()
        assert raised.value.recovery == 'Run rebuild --target DurinNativeTests.'
    def test_invalid_or_non_target_recovery_state_falls_back_to_all(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        marker = Path(directory) / 'interrupted.json'
        marker.write_text(json.dumps({'action': 'unknown', 'target': 'Core'}), encoding='utf-8')
        assert build_recovery.recoverable_target(marker) is None
        assert build_recovery.recovery_target(marker) == 'all'
        marker.write_text('{invalid', encoding='utf-8')
        assert build_recovery.recoverable_target(marker) is None
        assert build_recovery.recovery_target(marker) == 'all'
    def test_recover_clears_a_valid_interruption_marker(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        marker = Path(directory) / 'interrupted.json'
        marker.write_text(json.dumps({'pid': 1, 'action': 'build', 'target': 'Core'}), encoding='utf-8')
        operation = mock.Mock()
        build_recovery.execute_with_recovery_marker(action=models.Action.RECOVER, marker_file=marker, metadata={'pid': 2, 'action': 'recover', 'target': 'recorded-target'}, operation=operation)
        operation.assert_called_once_with('Core')
        assert not marker.exists()
    def test_recover_validation_failure_does_not_execute_or_replace_marker(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        marker = Path(directory) / 'interrupted.json'
        original = b'{"action": "unknown", "target": "Core"}'
        marker.write_bytes(original)
        operation = mock.Mock()
        with pytest.raises(errors.BuildToolError, match='cannot be resumed safely'):
            build_recovery.execute_with_recovery_marker(action=models.Action.RECOVER, marker_file=marker, metadata={'pid': 2, 'action': 'recover', 'target': 'recorded-target'}, operation=operation)
        operation.assert_not_called()
        assert marker.read_bytes() == original
    def test_recover_builds_incrementally_without_cleaning(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = self.make_preset()
        context = build_context.BuildContext(request_fixtures.command_request(models.Action.RECOVER), models.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=4, environment={'PATH': 'cached'})
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        build_directory = Path(directory)
        with mock.patch.object(build_core, 'preset_build_directory', return_value=build_directory), mock.patch.object(build_core, 'cache_is_usable', return_value=True), mock.patch.object(build_core, 'ninja_uses_english_msvc_prefix', return_value=True), mock.patch.object(build_core, 'run_command') as run:
            build_core.perform_action(context, output, target_override='Core')
        run.assert_called_once_with(['cmake', '--build', str(build_directory), '--target', 'Core', '-j', '4'], environment={'PATH': 'cached'}, output=output, show_heartbeat=False, cwd=settings.default_build_paths().root, state_directory=settings.default_build_paths().state_directory)
    def test_agent_build_enables_child_process_heartbeat(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = self.make_preset()
        request = request_fixtures.command_request(
            models.Action.BUILD,
            output=requests.OutputOptions(agent=True),
            options=request_fixtures.BuildActionOptions(target='Core'),
        )
        context = build_context.BuildContext(request, models.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=4, environment={'PATH': 'cached'})
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        build_directory = Path(tmp_path_factory.mktemp('case'))
        with mock.patch.object(build_core, 'preset_build_directory', return_value=build_directory), mock.patch.object(build_core, 'cache_is_usable', return_value=True), mock.patch.object(build_core, 'ninja_uses_english_msvc_prefix', return_value=True), mock.patch.object(build_core, 'run_command') as run:
            build_core.perform_action(context, output)
        assert run.call_args.kwargs['show_heartbeat']
    def test_normal_command_failure_restores_marker(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        marker = Path(directory) / 'interrupted.json'
        with pytest.raises(errors.BuildToolError, match='failed'):
            build_recovery.execute_with_recovery_marker(action=models.Action.BUILD, marker_file=marker, metadata={'pid': 1}, operation=lambda _target: (_ for _ in ()).throw(errors.BuildToolError('failed')))
        assert not marker.exists()
    def test_recover_command_failure_restores_interruption_marker(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        marker = Path(directory) / 'interrupted.json'
        original = b'{"pid": 1, "action": "build", "target": "Core"}'
        marker.write_bytes(original)
        with pytest.raises(errors.BuildToolError, match='failed'):
            build_recovery.execute_with_recovery_marker(action=models.Action.RECOVER, marker_file=marker, metadata={'pid': 2, 'action': 'recover', 'target': 'recorded-target'}, operation=lambda _target: (_ for _ in ()).throw(errors.BuildToolError('failed')))
        assert marker.read_bytes() == original
