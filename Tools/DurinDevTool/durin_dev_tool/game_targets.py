"""Read CMake-generated Game product sets; never infer target receipt locations."""
import json
from .build.selection import preset_build_directory
from .errors import DevToolError


def read_backend(selection):
    path = preset_build_directory(selection.preset, root=selection.repository.root) / "ExportBackend.json"
    if not path.is_file():
        raise DevToolError("Missing generated Game targets; configure/build the selected tool host: " + str(path))
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("SchemaVersion") != 1 or data.get("Kind") != "source-stage" or not isinstance(data.get("Games"), list):
        raise DevToolError("Invalid generated Game targets: " + str(path))
    return path, data


def select_game(data, *, project, platform, configuration=None, preset=None, allowed=None):
    name = json.loads(project.read_text(encoding="utf-8"))["ProjectName"]
    matches = [g for g in data["Games"] if g.get("Project", name) == name
               and g.get("Platform", platform).casefold() == platform.casefold()
               and (configuration is None or g["Configuration"] == configuration)
               and (not preset or g["Name"] == preset)
               and (allowed is None or g["Name"] in allowed)]
    if len(matches) != 1:
        raise DevToolError("Need one matching generated Game product set; select configuration/preset or an explicit target receipt")
    return matches[0]
