"""Tool discovery and invocation through the current script job's endpoint."""

from _durin import _request

from ._invocation import _Invocation


class _Tools:
    def __init__(self, owner):
        self._owner = owner

    def _check(self):
        _request("owner", (self._owner,))

    def list(self):
        self._check()
        return _request("list", ())

    def describe(self, toolset, tool):
        self._check()
        return _request("describe", (toolset, tool))

    def submit(self, toolset, tool, arguments, *, version=None):
        self._check()
        return _Invocation(_request("submit", (toolset, tool, arguments, version)))

    def call(self, toolset, tool, arguments, *, version=None, timeout=30.0):
        return self.submit(toolset, tool, arguments, version=version).result(timeout)
