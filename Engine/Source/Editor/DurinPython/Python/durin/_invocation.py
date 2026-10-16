"""Job-bound handles for native editor tool invocations."""

from _durin import _request


class _Invocation:
    def __init__(self, identity):
        self._identity = identity

    def state(self):
        return _request("state", self._identity)

    def result(self, timeout=30.0):
        return _request("result", (*self._identity, timeout))

    def cancel(self):
        return _request("cancel", self._identity)
