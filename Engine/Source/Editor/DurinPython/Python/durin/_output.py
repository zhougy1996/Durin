"""Forward stdout and stderr to the native bounded job output buffer."""

from _durin import _request


class _Output:
    def write(self, text):
        return _request("write", (text,))

    def flush(self):
        pass
