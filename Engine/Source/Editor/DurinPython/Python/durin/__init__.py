"""Editor Toolset access for scripts in Durin's embedded interpreter."""

from _durin import CancelledError, NotReady, ToolError, ScriptAccessError, _request

from ._invocation import _Invocation
from ._output import _Output
from ._tools import _Tools
from ._transaction import ScopedEditorTransaction
from ._continuation import defer

tools = _Tools(0)


def get_type(qualified_name):
    """Return an exposed native reflected type by its canonical qualified name."""
    return _request("reflect_type", (qualified_name,))


def get_editor_subsystem(subsystem_type):
    """Return the selected editor Host's existing subsystem instance."""
    return _request("editor_subsystem", (subsystem_type,))


def generate_stubs():
    """Return deterministic .pyi declarations for the currently published native API.

    Save as Build/PythonStubs/durin/__init__.pyi and add Build/PythonStubs to
    your IDE's Python search paths. Regenerate after loading native modules.
    """
    return _request("reflection_stubs", ())
