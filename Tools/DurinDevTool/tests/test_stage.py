"""Client protocol tests. Product Stage/PE/publication coverage lives in GameExportContractTests."""
import io
import json
from pathlib import Path
from unittest import mock

import pytest
from durin_dev_tool import stage
from durin_dev_tool.errors import DevToolError


class RecordingInput(io.StringIO):
    def close(self):
        self.sent = self.getvalue()
        super().close()


def terminal(**changes):
    event = dict(ProtocolVersion=1, RequestId="Client1", Sequence=0, Kind="result", Status="succeeded",
        ExitCode=0, Category="none", ChildrenRetired=True, Message="staged", PlanFingerprint="0"*64, Output="/out")
    event.update(changes)
    return event


def process(events, code=0):
    child = mock.MagicMock()
    child.__enter__.return_value = child
    child.stdin = RecordingInput()
    child.stdout = io.StringIO("".join(json.dumps(e)+"\n" for e in events))
    child.wait.return_value = code
    return child


def test_dispatch_keeps_pipe_owned_until_result():
    child = process([terminal()])
    request = dict(ProtocolVersion=1, Kind="request", RequestId="Client1")
    with mock.patch.object(stage.subprocess, "Popen", return_value=child) as launch:
        result = stage.dispatch_worker(Path("worker.exe"), request, stdout=io.StringIO(), stderr=io.StringIO())
    assert json.loads(child.stdin.sent) == request
    assert launch.call_args.args[0] == ["worker.exe", "--protocol=1"]
    assert result["Output"] == "/out"
    child.wait.assert_called_once_with(timeout=30)


@pytest.mark.parametrize("events,code", [
    ([], 0), ([terminal(ExitCode=8)], 0), ([terminal(ChildrenRetired=False)], 0),
    ([terminal(ProtocolVersion=2)], 0), ([terminal(RequestId="Wrong")], 0),
    ([terminal(), terminal(Sequence=1)], 0),
    ([terminal(Status="failed", ExitCode=8, Category="stage", Message="missing DLL")], 8),
])
def test_invalid_or_failed_worker_cannot_be_reported_as_success(events, code):
    child = process(events, code)
    with mock.patch.object(stage.subprocess, "Popen", return_value=child):
        with pytest.raises(DevToolError):
            stage.dispatch_worker(Path("worker.exe"), dict(RequestId="Client1"), stdout=io.StringIO(), stderr=io.StringIO())


def test_interrupt_sends_cancel_and_waits_for_terminal_cleanup():
    child = process([], 130)
    class InterruptedOutput:
        def __init__(self): self.count = 0
        def __iter__(self):
            self.count += 1
            if self.count == 1: raise KeyboardInterrupt
            return iter([json.dumps(terminal(Status="cancelled", ExitCode=130, Category="cancelled", Message="cancelled"))+"\n"])
    child.stdout = InterruptedOutput()
    with mock.patch.object(stage.subprocess, "Popen", return_value=child):
        with pytest.raises(DevToolError, match="cancelled"):
            stage.dispatch_worker(Path("worker.exe"), dict(RequestId="Client1"), stdout=io.StringIO(), stderr=io.StringIO())
    frames = [json.loads(line) for line in child.stdin.sent.splitlines()]
    assert frames[-1] == dict(ProtocolVersion=1, Kind="cancel", RequestId="Client1")
    child.wait.assert_called_once_with(timeout=30)
