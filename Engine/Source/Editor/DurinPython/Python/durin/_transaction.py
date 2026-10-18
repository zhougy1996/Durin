"""Scopes use the admitting editor's native transaction buffer."""

from _durin import _request


class ScopedEditorTransaction:
    """Record ordinary native edits; cancellation discards recording, not edits."""

    def __init__(self, desc):
        if not isinstance(desc, str):
            raise TypeError("desc must be a string")
        self._desc = desc
        self._identity = None

    def __enter__(self):
        if self._identity is not None:
            raise RuntimeError("Transaction is already entered")
        self._identity = _request("transaction_begin", (self._desc,))
        return self

    def __exit__(self, type, value, traceback):
        if self._identity is not None:
            _request("transaction_end", self._identity)
            self._identity = None
        # UE 5.3 returns None, including on exception. Applied edits remain.

    def cancel(self):
        if self._identity is not None:
            _request("transaction_cancel", self._identity)
            self._identity = None
