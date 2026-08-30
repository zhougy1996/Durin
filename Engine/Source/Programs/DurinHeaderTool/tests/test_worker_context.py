import json
import multiprocessing
import sys
from concurrent.futures import ProcessPoolExecutor
from functools import partial
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from durin_header_tool import config as configs
from durin_header_tool.generators import module_export_file_generator as export_generator
from durin_header_tool.model.export_info import load_module_export_file


def test_spawn_workers_load_external_project_for_eight_headers(tmp_path: Path):
    project_name = "SpawnWorkerProject"
    module_name = "SpawnWorkerModule"
    module_dir = tmp_path / "Source" / module_name
    public_dir = module_dir / "Public"
    public_dir.mkdir(parents=True)

    headers = []
    for index in range(8):
        header = f"Public/SpawnType{index}.h"
        headers.append(header)
        (module_dir / header).write_text(
            f"""#pragma once

namespace SpawnWorker
{{
    DSTRUCT()
    struct FSpawnType{index}
    {{
        GENERATED_BODY()
    }};
}}
""",
            encoding="utf-8",
        )

    (module_dir / f"{module_name}.dmodule").write_text(
        json.dumps(
            {
                "ModuleName": module_name,
                "ReflectHeaders": headers,
            }
        ),
        encoding="utf-8",
    )
    project_file = tmp_path / f"{project_name}.dproject"
    project_file.write_text(
        json.dumps(
            {
                "ProjectName": project_name,
                "ModuleDirs": {module_name: f"Source/{module_name}"},
            }
        ),
        encoding="utf-8",
    )

    previous_projects = dict(configs.project_config.PROJECT_CONFIGS)
    previous_modules = dict(configs.module_config.MODULE_CONFIGS)
    previous_enabled_modules = dict(configs.module_config.ENABLED_MODULES)
    try:
        configs.init_configs((project_file,))
        spawn_executor = partial(
            ProcessPoolExecutor,
            mp_context=multiprocessing.get_context("spawn"),
        )
        with mock.patch.object(
            export_generator,
            "ProcessPoolExecutor",
            spawn_executor,
        ):
            export_generator.generate_module_export_file(module_name, max_workers=2)

        export_info = load_module_export_file(
            tmp_path
            / "Intermediate"
            / "Build"
            / configs.ARCH
            / configs.RUNTIME_VARIANT
            / module_name
            / "DHT"
            / f"{module_name}.export"
        )
        assert set(export_info.Symbols) == {
            f"SpawnWorker::FSpawnType{index}"
            for index in range(8)
        }
    finally:
        configs.project_config.PROJECT_CONFIGS.clear()
        configs.project_config.PROJECT_CONFIGS.update(previous_projects)
        configs.module_config.MODULE_CONFIGS.clear()
        configs.module_config.MODULE_CONFIGS.update(previous_modules)
        configs.module_config.ENABLED_MODULES.clear()
        configs.module_config.ENABLED_MODULES.update(previous_enabled_modules)
