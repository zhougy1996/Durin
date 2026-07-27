import multiprocessing
import queue
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from durin_header_tool import io as utils


def _lock_worker(lock_path: str, operation: str, release_event, result_queue) -> None:
    with utils.acquire_output_lock(Path(lock_path), operation):
        result_queue.put((operation, "acquired"))
        release_event.wait(10)
    result_queue.put((operation, "released"))


class TestOutputLock:
    def test_same_output_lock_serializes_processes(self, tmp_path):
        context = multiprocessing.get_context("spawn")
        lock_path = str(tmp_path / "shared.lock")
        first_release = context.Event()
        second_release = context.Event()
        results = context.Queue()
        first = context.Process(target=_lock_worker, args=(lock_path, "first", first_release, results))
        second = context.Process(target=_lock_worker, args=(lock_path, "second", second_release, results))
        try:
            first.start()
            assert results.get(timeout=5) == ("first", "acquired")
            second.start()
            with pytest.raises(queue.Empty):
                results.get(timeout=0.4)

            first_release.set()
            assert results.get(timeout=5) == ("first", "released")
            assert results.get(timeout=5) == ("second", "acquired")
            second_release.set()
            assert results.get(timeout=5) == ("second", "released")
        finally:
            first_release.set()
            second_release.set()
            first.join(timeout=5)
            second.join(timeout=5)
            if first.is_alive():
                first.terminate()
            if second.is_alive():
                second.terminate()

        assert first.exitcode == 0
        assert second.exitcode == 0

    def test_different_output_locks_can_be_acquired_together(self, tmp_path):
        context = multiprocessing.get_context("spawn")
        release = context.Event()
        results = context.Queue()
        first = context.Process(
            target=_lock_worker,
            args=(str(tmp_path / "first.lock"), "first", release, results),
        )
        second = context.Process(
            target=_lock_worker,
            args=(str(tmp_path / "second.lock"), "second", release, results),
        )
        try:
            first.start()
            second.start()
            acquired = {results.get(timeout=5), results.get(timeout=5)}
            assert acquired == {("first", "acquired"), ("second", "acquired")}
            release.set()
            released = {results.get(timeout=5), results.get(timeout=5)}
            assert released == {("first", "released"), ("second", "released")}
        finally:
            release.set()
            first.join(timeout=5)
            second.join(timeout=5)
            if first.is_alive():
                first.terminate()
            if second.is_alive():
                second.terminate()

        assert first.exitcode == 0
        assert second.exitcode == 0
