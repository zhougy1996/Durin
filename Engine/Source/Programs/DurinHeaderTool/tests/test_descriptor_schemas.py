import json
import re
import sys
from pathlib import Path

import pytest
from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from durin_header_tool.io.json_helper import load_json_descriptor


PROJECT_SCHEMA = "durin-project.schema.json"
MODULE_SCHEMA = "durin-module.schema.json"
SCHEMA_DIR = ROOT / "schemas"
WORKSPACE_ROOT = ROOT.parents[3]


def _write_json(path: Path, value: object) -> Path:
    path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
    return path


@pytest.mark.parametrize("schema_name", [PROJECT_SCHEMA, MODULE_SCHEMA])
def test_descriptor_schema_is_valid_draft_2020_12(schema_name: str):
    schema = json.loads((SCHEMA_DIR / schema_name).read_text(encoding="utf-8"))
    Draft202012Validator.check_schema(schema)


def test_all_tracked_descriptors_match_their_schema():
    project_paths = sorted(
        [WORKSPACE_ROOT / "Engine" / "Engine.dproject", WORKSPACE_ROOT / "Sandbox" / "Sandbox.dproject"]
    )
    module_paths = sorted(
        [
            *WORKSPACE_ROOT.glob("Engine/**/*.dmodule"),
            *WORKSPACE_ROOT.glob("Sandbox/**/*.dmodule"),
        ]
    )

    assert len(project_paths) == 2
    assert len(module_paths) == 22
    for path in project_paths:
        load_json_descriptor(path, PROJECT_SCHEMA)
    for path in module_paths:
        load_json_descriptor(path, MODULE_SCHEMA)


def test_minimal_descriptors_are_valid(tmp_path: Path):
    assert load_json_descriptor(
        _write_json(tmp_path / "Minimal.dproject", {"ProjectName": "Minimal"}),
        PROJECT_SCHEMA,
    ) == {"ProjectName": "Minimal"}
    assert load_json_descriptor(
        _write_json(tmp_path / "Minimal.dmodule", {"ModuleName": "Minimal"}),
        MODULE_SCHEMA,
    ) == {"ModuleName": "Minimal"}


def test_complete_project_with_mounts_is_valid(tmp_path: Path):
    descriptor = {
        "$schema": "durin-project.schema.json",
        "ProjectName": "Example",
        "ModuleDirs": {"Example": "Source/Runtime/Example"},
        "BaseModules": ["Example"],
        "ExtraModules": {"DurinEditor": {"Modules": ["Example"]}},
        "Mounts": [
            {
                "VirtualRoot": "/Plugins/PCG/",
                "Owner": "Extension",
                "Root": "Plugins/PCG",
                "ContentPath": "Content",
                "AutoScan": True,
                "AuthoringWritable": False,
                "Dependencies": ["/Engine/"],
            }
        ],
    }
    assert load_json_descriptor(
        _write_json(tmp_path / "Example.dproject", descriptor),
        PROJECT_SCHEMA,
    ) == descriptor


def test_rendered_scaffolding_descriptors_are_valid(tmp_path: Path):
    template_root = WORKSPACE_ROOT / "Templates" / "Scaffolding"
    project_text = (template_root / "project" / "descriptor.json.template").read_text(
        encoding="utf-8"
    ).replace("{{PROJECT_NAME}}", "Example")
    module_replacements = {
        "{{MODULE_NAME}}": "Example",
        "{{LINK_TYPE}}": "Shared",
        "{{PCH}}": "Self",
        "{{PRIVATE_DEPENDENCIES}}": '["Core"]',
        "{{PUBLIC_DEPENDENCIES}}": "[]",
        "{{OPTIONAL_PRIVATE_DEPENDENCIES}}": "[]",
        "{{OPTIONAL_PUBLIC_DEPENDENCIES}}": "[]",
    }
    module_text = (template_root / "module" / "descriptor.json.template").read_text(
        encoding="utf-8"
    )
    for placeholder, value in module_replacements.items():
        module_text = module_text.replace(placeholder, value)

    project_path = tmp_path / "Example.dproject"
    project_path.write_text(project_text, encoding="utf-8")
    module_path = tmp_path / "Example.dmodule"
    module_path.write_text(module_text, encoding="utf-8")

    load_json_descriptor(project_path, PROJECT_SCHEMA)
    load_json_descriptor(module_path, MODULE_SCHEMA)


@pytest.mark.parametrize(
    ("descriptor", "expected_path"),
    [
        ({"ModuleName": "Example", "PrivateDependecies": []}, "$"),
        ({"ModuleName": "Example", "PrivateDependencies": "Core"}, "$.PrivateDependencies"),
        ({"ModuleName": "Example", "PrivateDependencies": ["Core", 7]}, "$.PrivateDependencies[1]"),
        ({"ModuleName": "Example", "PCH": None}, "$.PCH"),
        ({"ModuleName": "Example", "PrivateDependencies": ["Core", "Core"]}, "$.PrivateDependencies"),
    ],
)
def test_invalid_module_descriptor_reports_json_path(
    tmp_path: Path,
    descriptor: dict[str, object],
    expected_path: str,
):
    path = _write_json(tmp_path / "Invalid.dmodule", descriptor)
    with pytest.raises(ValueError, match=re.escape(expected_path)):
        load_json_descriptor(path, MODULE_SCHEMA)


def test_unknown_nested_project_field_reports_json_path(tmp_path: Path):
    path = _write_json(
        tmp_path / "Invalid.dproject",
        {
            "ProjectName": "Example",
            "ExtraModules": {"DurinEditor": {"Moduels": []}},
        },
    )
    with pytest.raises(ValueError, match=r"\$\.ExtraModules\.DurinEditor"):
        load_json_descriptor(path, PROJECT_SCHEMA)


def test_duplicate_json_field_is_rejected(tmp_path: Path):
    path = tmp_path / "Duplicate.dmodule"
    path.write_text('{"ModuleName":"First","ModuleName":"Second"}', encoding="utf-8")
    with pytest.raises(ValueError, match='duplicate field "ModuleName"'):
        load_json_descriptor(path, MODULE_SCHEMA)


def test_malformed_json_reports_line_and_column(tmp_path: Path):
    path = tmp_path / "Malformed.dmodule"
    path.write_text('{\n  "ModuleName":\n}', encoding="utf-8")
    with pytest.raises(ValueError, match=r"line 3, column 1"):
        load_json_descriptor(path, MODULE_SCHEMA)


def test_non_utf8_descriptor_is_rejected(tmp_path: Path):
    path = tmp_path / "Encoded.dmodule"
    path.write_bytes(b'{"ModuleName":"\xff"}')
    with pytest.raises(ValueError, match="not valid UTF-8"):
        load_json_descriptor(path, MODULE_SCHEMA)


def test_cmake_fingerprint_tracks_descriptor_schemas():
    project_setup = (WORKSPACE_ROOT / "CMake" / "Project" / "ProjectSetup.cmake").read_text(
        encoding="utf-8"
    )
    assert '"${DHT_DIR}/schemas/*.json"' in project_setup
