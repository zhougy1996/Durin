import json
import re
import subprocess
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
DEV_TOOL_ROOT = WORKSPACE_ROOT / "Tools" / "DurinDevTool"
if str(DEV_TOOL_ROOT) not in sys.path:
    sys.path.insert(0, str(DEV_TOOL_ROOT))

from durin_dev_tool.build.config import BuildToolError
from durin_dev_tool.build.descriptors import load_module_descriptor, load_project_descriptor


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
    path = _write_json(tmp_path / "Example.dproject", descriptor)
    assert load_json_descriptor(
        path,
        PROJECT_SCHEMA,
    ) == descriptor
    assert load_project_descriptor(path).name == "Example"


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
    with pytest.raises(BuildToolError, match=re.escape(expected_path)):
        load_module_descriptor(path, "Example")


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
    with pytest.raises(BuildToolError, match=r"\$\.ExtraModules\.DurinEditor"):
        load_project_descriptor(path)


def test_dht_and_devtool_reject_the_same_module_structure(tmp_path: Path):
    path = _write_json(
        tmp_path / "Invalid.dmodule",
        {"ModuleName": "Invalid", "PrivateDependecies": []},
    )
    with pytest.raises(ValueError, match="PrivateDependecies"):
        load_json_descriptor(path, MODULE_SCHEMA)
    with pytest.raises(BuildToolError, match="PrivateDependecies"):
        load_module_descriptor(path, "Example")


def test_duplicate_json_field_is_rejected(tmp_path: Path):
    path = tmp_path / "Duplicate.dmodule"
    path.write_text('{"ModuleName":"First","ModuleName":"Second"}', encoding="utf-8")
    with pytest.raises(ValueError, match='duplicate field "ModuleName"'):
        load_json_descriptor(path, MODULE_SCHEMA)
    with pytest.raises(BuildToolError, match='duplicate field "ModuleName"'):
        load_module_descriptor(path, "Example")


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


def test_vscode_template_associates_both_descriptor_schemas():
    settings = json.loads(
        (WORKSPACE_ROOT / "Templates" / "VSCode" / "settings.json").read_text(
            encoding="utf-8"
        )
    )
    associations = {
        tuple(entry["fileMatch"]): entry["url"] for entry in settings["json.schemas"]
    }
    assert associations[("*.dproject",)].endswith("/durin-project.schema.json")
    assert associations[("*.dmodule",)].endswith("/durin-module.schema.json")


@pytest.mark.parametrize(
    ("module_text", "expected_error"),
    [
        ('{"ModuleName":"Broken","PrivateDependecies":[]}', "PrivateDependecies"),
        ('{"ModuleName":"Broken","PrivateDependencies":[7]}', "PrivateDependencies[0]"),
        ('{"ModuleName":"Broken",}', "malformed JSON"),
        ('{"ModuleName":"Broken","ModuleName":"Duplicate"}', 'duplicate field "ModuleName"'),
    ],
)
def test_prepare_project_build_rejects_invalid_module_before_metadata_publication(
    tmp_path: Path,
    module_text: str,
    expected_error: str,
):
    project_root = tmp_path / "Invalid"
    module_root = project_root / "Source" / "Broken"
    module_root.mkdir(parents=True)
    project_path = _write_json(
        project_root / "Invalid.dproject",
        {
            "ProjectName": "Invalid",
            "ModuleDirs": {"Broken": "Source/Broken"},
            "BaseModules": ["Broken"],
        },
    )
    (module_root / "Broken.dmodule").write_text(module_text, encoding="utf-8")

    result = subprocess.run(
        [
            sys.executable,
            str(ROOT / "durin_header_tool" / "__main__.py"),
            "prepare_project_build",
            "--project",
            str(project_path),
            "--runtime-variant",
            "DurinEditor",
        ],
        cwd=WORKSPACE_ROOT,
        capture_output=True,
        text=True,
        timeout=60,
    )

    assert result.returncode != 0
    assert expected_error in result.stderr
    assert not list(project_root.rglob("*.cmake"))
    assert not list(project_root.rglob("*.stamp"))


def test_prepare_project_build_rejects_invalid_nested_project_field(tmp_path: Path):
    project_root = tmp_path / "Invalid"
    project_root.mkdir()
    project_path = _write_json(
        project_root / "Invalid.dproject",
        {
            "ProjectName": "Invalid",
            "ExtraModules": {"DurinEditor": {"Moduels": []}},
        },
    )

    result = subprocess.run(
        [
            sys.executable,
            str(ROOT / "durin_header_tool" / "__main__.py"),
            "prepare_project_build",
            "--project",
            str(project_path),
            "--runtime-variant",
            "DurinEditor",
        ],
        cwd=WORKSPACE_ROOT,
        capture_output=True,
        text=True,
        timeout=60,
    )

    assert result.returncode != 0
    assert "Moduels" in result.stderr
    assert not list(project_root.rglob("*.cmake"))
    assert not list(project_root.rglob("*.stamp"))


def test_prepare_project_build_accepts_valid_mounts(tmp_path: Path):
    project_root = tmp_path / "Mounted"
    module_root = project_root / "Source" / "Mounted"
    module_root.mkdir(parents=True)
    project_path = _write_json(
        project_root / "Mounted.dproject",
        {
            "ProjectName": "Mounted",
            "ModuleDirs": {"Mounted": "Source/Mounted"},
            "BaseModules": ["Mounted"],
            "Mounts": [
                {
                    "VirtualRoot": "/Plugins/Mounted/",
                    "Owner": "Extension",
                    "Root": "Plugins/Mounted",
                    "ContentPath": "Content",
                    "AutoScan": True,
                    "AuthoringWritable": False,
                    "Dependencies": ["/Engine/"],
                }
            ],
        },
    )
    _write_json(module_root / "Mounted.dmodule", {"ModuleName": "Mounted"})

    result = subprocess.run(
        [
            sys.executable,
            str(ROOT / "durin_header_tool" / "__main__.py"),
            "prepare_project_build",
            "--project",
            str(project_path),
            "--runtime-variant",
            "DurinEditor",
        ],
        cwd=WORKSPACE_ROOT,
        capture_output=True,
        text=True,
        timeout=60,
    )

    assert result.returncode == 0, result.stderr
    assert len(list(project_root.rglob("*.cmake"))) == 2
