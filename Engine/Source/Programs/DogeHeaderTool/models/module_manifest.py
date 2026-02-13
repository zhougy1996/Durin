from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List
import json

from utils import FileFingerprint, LightFileFingerprint
import utils

@dataclass
class ModuleManifest:
    module_name: str
    dep_module_exports: Dict[str, LightFileFingerprint] = field(default_factory=dict) # Key: module name, Value: fingerprint of the module's export file
    reflect_headers: Dict[str, FileFingerprint] = field(default_factory=dict) # Key: header file path, Value: fingerprint of the header file

def load_module_manifest_file(module_name: str) -> ModuleManifest:
    manifest_file_path = utils.get_module_manifest_file_path(module_name)
    if not manifest_file_path.exists():
        raise FileNotFoundError(f"Module manifest file for module '{module_name}' not found at expected path: {manifest_file_path}")
    try:
        with manifest_file_path.open("r") as f:
            json_data = json.load(f)
            manifest = utils.dataclass_from_dict(ModuleManifest, json_data, auto_convert=True)
            return manifest
    except json.JSONDecodeError as e:
        raise ValueError(f"Error parsing JSON in module manifest file '{manifest_file_path}': {e}")

def save_module_manifest_file(manifest: ModuleManifest) -> str:
    module_name = manifest.module_name
    output_path = utils.get_module_manifest_file_path(module_name)
    json_data = utils.dict_from_dataclass(ModuleManifest, manifest, auto_convert=True)
    content = json.dumps(json_data, indent=4)
    utils.generate_file(output_path, content)
    return content