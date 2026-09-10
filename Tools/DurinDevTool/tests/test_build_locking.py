import pytest
import errno
import json
import os
from pathlib import Path
from unittest import mock
from . import build_request_fixtures as request_fixtures
from durin_dev_tool.build import errors
from durin_dev_tool.build import locking as build_locking


class TestCore:
    make_profile = staticmethod(request_fixtures.make_profile)
    make_preset = staticmethod(request_fixtures.make_preset)

    def test_checkout_lock_is_exclusive_across_presets(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        path = build_locking.lock_file_path(Path(directory))
        with build_locking.BuildToolLock(path, {'pid': 1}):
            with pytest.raises(errors.BuildToolError, match='already owns'):
                with build_locking.BuildToolLock(path, {'pid': 2}):
                    pass
    def test_inaccessible_shared_lock_preserves_file_and_reports_both_causes(self, tmp_path) -> None:
        path = tmp_path / 'dependencies.lock'
        path.write_bytes(b'original metadata')
        denied = PermissionError(13, 'Permission denied', str(path))
        with mock.patch.object(Path, 'open', side_effect=denied) as open_file, mock.patch.object(build_locking.os, 'replace') as replace, mock.patch.object(build_locking.subprocess, 'run') as run:
            with pytest.raises(errors.BuildToolError) as raised:
                with build_locking.BuildToolLock(path, {}, scope='shared dependency store'):
                    pass
        assert 'shared dependency store' in str(raised.value)
        assert 'cannot distinguish' in str(raised.value)
        assert 'Execution environment restrictions:' in raised.value.recovery
        assert 'File permissions:' in raised.value.recovery
        assert str(path.parent) in raised.value.recovery
        assert 'outside the current worktree' in raised.value.recovery
        assert 'icacls' not in raised.value.recovery
        assert 'Remove-Item' not in raised.value.recovery
        open_file.assert_called_once_with('a+b')
        replace.assert_not_called()
        run.assert_not_called()
        assert path.read_bytes() == b'original metadata'

    def test_lock_directory_access_denied_uses_scoped_diagnostic(self, tmp_path) -> None:
        path = tmp_path / 'dependencies.lock'
        with mock.patch.object(Path, 'mkdir', side_effect=PermissionError(13, 'denied')):
            with pytest.raises(errors.BuildToolError, match='shared dependency store') as raised:
                with build_locking.BuildToolLock(path, {}, scope='shared dependency store'):
                    pass
        assert 'sandbox' in raised.value.recovery

    def test_acquiring_lock_does_not_change_acl(self, tmp_path) -> None:
        with mock.patch.object(build_locking.subprocess, 'run') as run:
            with build_locking.BuildToolLock(tmp_path / 'checkout.lock', {}):
                pass
        run.assert_not_called()

    @pytest.mark.parametrize('error_number', [errno.EIO, errno.EBADF])
    def test_lock_io_failure_is_not_reported_as_contention(self, tmp_path, error_number) -> None:
        path = tmp_path / 'checkout.lock'
        path.write_bytes(b'\0{}')
        lock = build_locking.BuildToolLock(path, {})
        primitive = 'msvcrt.locking' if os.name == 'nt' else 'fcntl.flock'
        with mock.patch(primitive, side_effect=OSError(error_number, 'I/O failure')):
            with pytest.raises(errors.BuildToolError, match='Could not acquire'):
                lock.__enter__()
            assert lock.handle is None
            with pytest.raises(errors.BuildToolError, match='Could not probe'):
                build_locking.lock_is_owned(path)

    def test_windows_access_denied_is_not_a_lock_conflict(self) -> None:
        denied = PermissionError(errno.EACCES, 'denied')
        denied.winerror = 5
        assert not build_locking.lock_contention(denied)

    def test_lock_probe_access_denied_does_not_imply_ownership(self, tmp_path) -> None:
        with mock.patch.object(Path, 'open', side_effect=PermissionError(13, 'denied')):
            with pytest.raises(errors.BuildToolError, match='cannot distinguish'):
                build_locking.lock_is_owned(tmp_path / 'checkout.lock')

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
