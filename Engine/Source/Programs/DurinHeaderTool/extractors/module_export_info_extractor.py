import configs
from models.export_infos import ExportedSymbolInfo, ModuleExportInfo
from models.reflection_info import parse_reflection_header


def extract_module_export_info(module_name: str) -> ModuleExportInfo:
    module_config = configs.get_module_config(module_name)
    export_info = ModuleExportInfo(module=module_name)

    for header in module_config.reflect_headers:
        header_info = parse_reflection_header(module_name, header, export_mode=True)
        for class_info in header_info.classes:
            export_info.symbols[class_info.qualified_name] = ExportedSymbolInfo(
                kind="class",
                shortName=class_info.short_name,
                namespace=class_info.namespace,
                qualifiedName=class_info.qualified_name,
                generatedHelperName=class_info.generated_helper_name,
                header=class_info.header,
                api=class_info.api,
                baseQualifiedName=class_info.base_qualified_name,
            )

    return export_info
