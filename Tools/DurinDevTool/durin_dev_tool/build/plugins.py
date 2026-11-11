"""Plugin metadata facade; generation/loading remains with subsequent stages."""
from functools import lru_cache
import importlib.util
from pathlib import Path

from .errors import BuildToolError


@lru_cache(maxsize=1)
def _resolver():
    source = (Path(__file__).resolve().parents[4] /
              "Engine/Source/Programs/DurinHeaderTool/durin_header_tool/config/plugin_config.py")
    spec = importlib.util.spec_from_file_location("durin_plugin_metadata", source)
    if spec is None or spec.loader is None:
        raise BuildToolError(f"Plugin resolver unavailable: {source}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def resolve_plugins(engine_root, project_file, **kwargs):
    try:
        return _resolver().resolve_plugins(engine_root, project_file, **kwargs)
    except (ValueError, OSError) as error:
        raise BuildToolError(str(error)) from error


def read_plugin_overrides(value, descriptor):
    try:
        return _resolver().plugin_overrides(value, descriptor)
    except ValueError as error:
        raise BuildToolError(str(error)) from error
