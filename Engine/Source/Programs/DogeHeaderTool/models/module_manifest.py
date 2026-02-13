from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List
import json

from utils import FileFingerprint

@dataclass
class ModuleManifest:
    module_name: str
    dep_module_exports: Dict[str, FileFingerprint] = field(default_factory=dict) # Key: module name, Value: fingerprint of the module's export file
    reflect_headers: Dict[str, FileFingerprint] = field(default_factory=dict) # Key: header file path, Value: fingerprint of the header file
