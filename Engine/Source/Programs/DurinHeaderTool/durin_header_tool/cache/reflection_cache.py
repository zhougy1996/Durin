from durin_header_tool.model.reflection_manifest import ModuleManifest


def reflection_manifest_contract_changed(old_manifest: ModuleManifest, new_manifest: ModuleManifest) -> bool:
    return (
        old_manifest.schema_version != new_manifest.schema_version
        or old_manifest.tool_version != new_manifest.tool_version
        or old_manifest.tool_fingerprint != new_manifest.tool_fingerprint
        or old_manifest.symbol_name_scheme != new_manifest.symbol_name_scheme
        or old_manifest.runtime_variant != new_manifest.runtime_variant
        or old_manifest.platform != new_manifest.platform
        or old_manifest.generator_options_hash != new_manifest.generator_options_hash
    )


def dependency_exports_changed(old_manifest: ModuleManifest, new_manifest: ModuleManifest) -> bool:
    if set(old_manifest.dep_module_exports.keys()) != set(new_manifest.dep_module_exports.keys()):
        return True
    for dep_module, new_fingerprint in new_manifest.dep_module_exports.items():
        old_fingerprint = old_manifest.dep_module_exports.get(dep_module)
        if old_fingerprint != new_fingerprint:
            return True
    return False
