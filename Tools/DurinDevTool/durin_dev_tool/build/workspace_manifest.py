"""Workspace membership and project test ownership shared by CMake and DevTool."""

from __future__ import annotations

import hashlib
import json
import sys
from dataclasses import dataclass
from pathlib import Path

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from durin_dev_tool.build.descriptors import load_project_descriptor
from durin_dev_tool.build.errors import BuildToolError
from durin_dev_tool.json_contract import JsonContractError, load_json_contract


SCHEMAS = Path(__file__).resolve().parents[4] / "Engine/Source/Programs/DurinHeaderTool/schemas"


@dataclass(frozen=True)
class WorkspaceProjectEntry:
    name: str
    descriptor: Path
    test_root: Path | None


def load_workspace_manifest(root: Path) -> tuple[WorkspaceProjectEntry, ...]:
    root = root.resolve()
    manifest = root / "Durin.dworkspace"
    try:
        data = load_json_contract(manifest, label="Workspace descriptor",
                                  schema_path=SCHEMAS / "durin-workspace.schema.json")
    except JsonContractError as error:
        raise BuildToolError(str(error)) from error
    entries = []
    names: set[str] = set()
    roots: list[Path] = []
    for relative in data["Projects"]:
        path = (root / relative).resolve()
        if Path(relative).is_absolute() or not path.is_relative_to(root):
            raise BuildToolError(f'Workspace project must stay inside "{root}": {relative}')
        if any(c in path.as_posix() for c in (';', '\n', '\r', '$', '"', ']')):
            raise BuildToolError(f'Unsupported CMake characters in project path: {path}')
        project = load_project_descriptor(path, schema_directory=SCHEMAS)
        if path.name != f"{project.name}.dproject":
            raise BuildToolError(f'Project descriptor must be named {project.name}.dproject: {path}')
        if project.name.casefold() in names or any(
            path.parent.is_relative_to(previous) or previous.is_relative_to(path.parent)
            for previous in roots
        ):
            raise BuildToolError(f'Duplicate or overlapping workspace project: {path}')
        if not (path.parent / "CMakeLists.txt").is_file():
            raise BuildToolError(f'Project CMakeLists.txt was not found: {path.parent}')
        names.add(project.name.casefold())
        roots.append(path.parent)
        entries.append(WorkspaceProjectEntry(project.name, path, project.native_test_root))
    if not entries or entries[0].descriptor != root / "Engine/Engine.dproject":
        raise BuildToolError("Workspace Projects must begin with Engine/Engine.dproject.")
    return tuple(entries)


def test_graph_fingerprint(root: Path, projects: tuple[WorkspaceProjectEntry, ...]) -> str:
    """Hash declarations and test file membership, including additions/deletions."""
    inputs = {root / "Durin.dworkspace"}
    members = []
    for project in projects:
        inputs.add(project.descriptor)
        if project.test_root and project.test_root.exists():
            for path in sorted(project.test_root.rglob("*")):
                if path.is_file():
                    members.append(path.relative_to(root).as_posix())
                    if path.name == "CMakeLists.txt" or path.suffix == ".cmake":
                        inputs.add(path)
    digest = hashlib.sha256()
    for path in sorted(inputs):
        digest.update(path.relative_to(root).as_posix().encode())
        digest.update(b"\0" + path.read_bytes() + b"\0")
    digest.update(json.dumps(sorted(members)).encode())
    return digest.hexdigest()


def write_cmake(root: Path, output: Path) -> None:
    projects = load_workspace_manifest(root)
    records = [{"name": p.name, "descriptor": p.descriptor.relative_to(root).as_posix(),
                "testRoot": p.test_root.relative_to(root).as_posix() if p.test_root else ""}
               for p in projects]
    lines = ["set(DURIN_WORKSPACE_PROJECT_FILES"]
    lines += [f'  "{p.descriptor.as_posix()}"' for p in projects]
    lines += [")", "set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${DURIN_WORKSPACE_PROJECT_FILES})",
              f'set_property(GLOBAL PROPERTY DURIN_WORKSPACE_TEST_PROJECTS [==[{json.dumps(records)}]==])',
              f'set_property(GLOBAL PROPERTY DURIN_TEST_GRAPH_FINGERPRINT "{test_graph_fingerprint(root, projects)}")']
    for p in projects:
        lines.append(f'add_subdirectory("{p.descriptor.parent.as_posix()}" "{p.descriptor.parent.relative_to(root).as_posix()}")')
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    write_cmake(Path(sys.argv[1]).resolve(), Path(sys.argv[2]))
