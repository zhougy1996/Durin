"""One explicitly registered callback or generator step per script-pump round."""

from types import GeneratorType
from _durin import _request
from ._invocation import _Invocation


class _Continuation:
    def __init__(self, work):
        if not callable(work) and not isinstance(work, GeneratorType):
            raise TypeError("defer expects a callback or generator")
        self.work = work

    def step(self, value, error):
        if not isinstance(self.work, GeneratorType):
            result = self.work()
            if isinstance(result, GeneratorType):
                self.work = result
                return False, 0, 0
            self.work = None
            return True, 0, 0
        try:
            yielded = self.work.throw(error) if error is not None else self.work.send(value)
        except StopIteration:
            self.work = None
            return True, 0, 0
        if yielded is None:
            return False, 0, 0
        if isinstance(yielded, _Invocation):
            _request("owner", (yielded._identity[0],))
            return False, *yielded._identity
        raise TypeError("a Durin generator may yield None or a job-owned invocation")

    def close(self):
        work, self.work = self.work, None
        if isinstance(work, GeneratorType):
            work.close()


def defer(work):
    """Append one callback/generator to the current job; never enter it inline."""
    _request("defer", (_Continuation(work),))
    return work
