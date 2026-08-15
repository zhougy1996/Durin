import pytest
import json
import os
from pathlib import Path
from unittest import mock
REPO_ROOT = Path(__file__).resolve().parents[3]
DEV_TOOL_DIR = REPO_ROOT / 'Tools' / 'DurinDevTool'
if str(DEV_TOOL_DIR) not in os.sys.path:
    os.sys.path.insert(0, str(DEV_TOOL_DIR))
from durin_dev_tool.build import config as build_config
from durin_dev_tool.build import locking as build_locking
from durin_dev_tool.build.handler import request_from_namespace
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
    def test_checkout_lock_is_exclusive_across_presets(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        path = build_locking.lock_file_path(Path(directory))
        with build_locking.BuildToolLock(path, {'pid': 1}):
            with pytest.raises(build_config.BuildToolError, match='already owns'):
                with build_locking.BuildToolLock(path, {'pid': 2}):
                    pass
    def test_inaccessible_lock_reports_acl_recovery(self) -> None:
        path = Path('checkout.lock')
        denied = PermissionError(13, 'Permission denied', str(path))
        with mock.patch.object(Path, 'open', side_effect=denied), mock.patch.object(build_locking, 'recover_inaccessible_windows_lock', return_value=False), pytest.raises(build_config.BuildToolError) as raised:
            build_locking.open_checkout_lock(path)
        assert 'file-permission problem' in str(raised.value)
        assert 'icacls' in raised.value.recovery
        assert 'Remove-Item' in raised.value.recovery
    def test_inaccessible_windows_lock_is_reopened_after_stale_recovery(self) -> None:
        path = Path('checkout.lock')
        handle = mock.Mock()
        denied = PermissionError(13, 'Permission denied', str(path))
        with mock.patch.object(Path, 'open', side_effect=[denied, handle]), mock.patch.object(build_locking, 'recover_inaccessible_windows_lock', return_value=True):
            assert build_locking.open_checkout_lock(path) is handle
    def test_windows_lock_acl_is_reset_to_directory_inheritance(self) -> None:
        result = mock.Mock(returncode=0)
        cwd = Path.cwd()
        with mock.patch.object(build_locking.os, 'name', 'nt'), mock.patch.object(build_locking.subprocess, 'run', return_value=result) as run:
            assert build_locking.normalize_windows_lock_acl(Path('checkout.lock'), cwd=cwd)
        assert run.call_args.args[0] == ['icacls', 'checkout.lock', '/reset', '/q']
    def test_windows_lock_acl_reset_is_best_effort(self) -> None:
        cwd = Path.cwd()
        with mock.patch.object(build_locking.os, 'name', 'nt'), mock.patch.object(build_locking.subprocess, 'run', return_value=mock.Mock(returncode=5)):
            assert not build_locking.normalize_windows_lock_acl(Path('checkout.lock'), cwd=cwd)
    def test_stop_ignores_stale_unowned_lock(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        path = Path(directory) / 'checkout.lock'
        path.write_text(json.dumps({'pid': 424242}), encoding='utf-8')
        with mock.patch.object(build_locking, 'lock_file_path', return_value=path), mock.patch.object(build_locking.subprocess, 'run') as run, mock.patch.object(build_locking.os, 'killpg', create=True) as killpg:
            assert not build_locking.stop_active_operation()
        run.assert_not_called()
        killpg.assert_not_called()
    def test_stop_terminates_process_recorded_by_owned_lock(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        path = Path(directory) / 'checkout.lock'
        with build_locking.BuildToolLock(path, {'pid': 424242}), mock.patch.object(build_locking, 'lock_file_path', return_value=path):
            if os.name == 'nt':
                result = mock.Mock(returncode=0)
                with mock.patch.object(build_locking.subprocess, 'run', return_value=result) as run:
                    assert build_locking.stop_active_operation()
                assert run.call_args.args[0][:3] == ['taskkill', '/PID', '424242']
            else:
                with mock.patch.object(build_locking.os, 'killpg') as killpg:
                    assert build_locking.stop_active_operation()
                killpg.assert_called_once_with(424242, build_locking.signal.SIGTERM)
