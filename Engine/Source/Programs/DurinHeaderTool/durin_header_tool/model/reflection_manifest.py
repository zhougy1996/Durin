from collections.abc import Iterable
from dataclasses import dataclass, field
import json
from pathlib import Path

from durin_header_tool.io import FileFingerprint
from durin_header_tool import io as utils
from durin_header_tool.model.reflection_info import SYMBOL_NAME_SCHEME, TOOL_VERSION

REFLECTION_MANIFEST_SCHEMA_VERSION = 6


def make_generated_output_names(module_name: str, reflect_headers: Iterable[str]) -> list[str]:
    outputs = {f"{module_name}.module.gen.cpp"}
    for header in reflect_headers:
        header_filename = Path(header).stem
        outputs.add(f"{header_filename}.gen.h")
        outputs.add(f"{header_filename}.gen.cpp")
    return sorted(outputs)


def _load_generated_output_names(
    data: dict[str, object],
    field_name: str,
    manifest_file_path: Path,
    *,
    fallback: list[str] | None = None,
) -> list[str]:
    raw_outputs = data.get(field_name)
    if raw_outputs is None and fallback is not None:
        return fallback
    if not isinstance(raw_outputs, list):
        raise ValueError(f"Field '{field_name}' in module manifest file '{manifest_file_path}' must be an array.")

    outputs = []
    for output in raw_outputs:
        if (
            not isinstance(output, str)
            or not output
            or "/" in output
            or "\\" in output
            or not (output.endswith(".gen.h") or output.endswith(".gen.cpp"))
        ):
            raise ValueError(
                f"Field '{field_name}' in module manifest file '{manifest_file_path}' contains an invalid generated output name."
            )
        outputs.append(output)
    return sorted(set(outputs))


@dataclass
class ModuleManifest:
    module_name: str
    schema_version: int = REFLECTION_MANIFEST_SCHEMA_VERSION
    tool_version: str = TOOL_VERSION
    tool_fingerprint: str = ""
    symbol_name_scheme: str = SYMBOL_NAME_SCHEME
    runtime_variant: str = ""
    platform: str = ""
    generator_options_hash: str = ""
    dep_module_exports: dict[str, str] = field(default_factory=dict)
    reflect_headers: dict[str, FileFingerprint] = field(default_factory=dict)
    resolved_symbol_dependencies: dict[str, dict[str, dict[str, str]]] = field(default_factory=dict)
    generated_outputs: list[str] = field(default_factory=list)
    generated_output_digests: dict[str, str] = field(default_factory=dict)
    pending_cleanup_outputs: list[str] = field(default_factory=list)


def load_module_manifest_file(module_name: str) -> ModuleManifest:
    manifest_file_path = utils.get_module_manifest_file_path(module_name)
    if not manifest_file_path.exists():
        raise FileNotFoundError(f"Module manifest file for module '{module_name}' not found at expected path: {manifest_file_path}")
    try:
        data = json.loads(manifest_file_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        raise ValueError(f"Error parsing JSON in module manifest file '{manifest_file_path}': {e}")

    if not isinstance(data, dict):
        raise ValueError(f"Module manifest file '{manifest_file_path}' must contain a JSON object.")

    object_fields = (
        "DependencyExports",
        "ReflectHeaders",
        "ResolvedSymbolDependencies",
        "GeneratedOutputDigests",
    )
    for field_name in object_fields:
        if not isinstance(data.get(field_name, {}), dict):
            raise ValueError(f"Field '{field_name}' in module manifest file '{manifest_file_path}' must be a JSON object.")

    module_name_from_file = data.get("ModuleName", module_name)
    reflect_headers = {
        key: _file_fingerprint_from_json(value)
        for key, value in data.get("ReflectHeaders", {}).items()
    }
    fallback_outputs = None
    if data.get("SchemaVersion", 0) < REFLECTION_MANIFEST_SCHEMA_VERSION:
        fallback_outputs = make_generated_output_names(module_name_from_file, reflect_headers)

    schema_version = data.get("SchemaVersion", 0)
    dependency_exports = data.get("DependencyExports", {})
    if schema_version >= REFLECTION_MANIFEST_SCHEMA_VERSION:
        for module, digest in dependency_exports.items():
            if not isinstance(module, str) or not module or not isinstance(digest, str):
                raise ValueError(
                    f"Field 'DependencyExports' in module manifest file '{manifest_file_path}' "
                    "must contain string content digests."
                )
    else:
        # Preserve legacy output ownership long enough for the schema contract
        # check and cleanup path. Legacy timestamp records are never treated as
        # current dependency identities.
        dependency_exports = {
            module: json.dumps(fingerprint, sort_keys=True, separators=(",", ":"))
            for module, fingerprint in dependency_exports.items()
        }

    generated_output_digests = data.get("GeneratedOutputDigests", {})
    for output_name, digest in generated_output_digests.items():
        if (
            not isinstance(output_name, str)
            or not output_name
            or "/" in output_name
            or "\\" in output_name
            or not isinstance(digest, str)
        ):
            raise ValueError(
                f"Field 'GeneratedOutputDigests' in module manifest file '{manifest_file_path}' "
                "contains an invalid output digest."
            )

    return ModuleManifest(
        module_name=module_name_from_file,
        schema_version=schema_version,
        tool_version=data.get("ToolVersion", ""),
        tool_fingerprint=data.get("ToolFingerprint", ""),
        symbol_name_scheme=data.get("SymbolNameScheme", ""),
        runtime_variant=data.get("RuntimeVariant", ""),
        platform=data.get("Platform", ""),
        generator_options_hash=data.get("GeneratorOptionsHash", ""),
        dep_module_exports=dict(dependency_exports),
        reflect_headers=reflect_headers,
        resolved_symbol_dependencies=data.get("ResolvedSymbolDependencies", {}),
        generated_outputs=_load_generated_output_names(
            data,
            "GeneratedOutputs",
            manifest_file_path,
            fallback=fallback_outputs,
        ),
        generated_output_digests=dict(generated_output_digests),
        pending_cleanup_outputs=_load_generated_output_names(
            data,
            "PendingCleanupOutputs",
            manifest_file_path,
            fallback=[],
        ),
    )


def save_module_manifest_file(manifest: ModuleManifest) -> str:
    output_path = utils.get_module_manifest_file_path(manifest.module_name)
    json_data = {
        "SchemaVersion": manifest.schema_version,
        "ToolVersion": manifest.tool_version,
        "ToolFingerprint": manifest.tool_fingerprint,
        "SymbolNameScheme": manifest.symbol_name_scheme,
        "ModuleName": manifest.module_name,
        "RuntimeVariant": manifest.runtime_variant,
        "Platform": manifest.platform,
        "GeneratorOptionsHash": manifest.generator_options_hash,
        "ReflectHeaders": {key: _file_fingerprint_to_json(value) for key, value in sorted(manifest.reflect_headers.items())},
        "GeneratedOutputs": sorted(set(manifest.generated_outputs)),
        "GeneratedOutputDigests": dict(sorted(manifest.generated_output_digests.items())),
        "PendingCleanupOutputs": sorted(set(manifest.pending_cleanup_outputs)),
        "DependencyExports": dict(sorted(manifest.dep_module_exports.items())),
        "ResolvedSymbolDependencies": {
            header: {
                symbol_name: dict(sorted(symbol_data.items()))
                for symbol_name, symbol_data in sorted(symbols.items())
            }
            for header, symbols in sorted(manifest.resolved_symbol_dependencies.items())
        },
    }
    content = json.dumps(json_data, indent=4)
    utils.generate_file(output_path, content)
    return content


def _file_fingerprint_to_json(fingerprint: FileFingerprint) -> dict[str, object]:
    return {
        "Timestamp": fingerprint.timestamp,
        "FileSize": fingerprint.file_size,
        "MD5": fingerprint.md5,
    }


def _file_fingerprint_from_json(data: dict[str, object]) -> FileFingerprint:
    return FileFingerprint(
        timestamp=data.get("Timestamp", 0.0),
        file_size=data.get("FileSize", 0),
        md5=data.get("MD5", ""),
    )
