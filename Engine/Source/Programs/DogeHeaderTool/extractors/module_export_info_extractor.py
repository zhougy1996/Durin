
import clang.cindex
from clang.cindex import TokenKind

import configs
from models.export_infos import *

_clang_args = [
    "-x",
    "c++",
    "-std=c++20",
    "-DNDEBUG",
    "-D_MSC_VER=1930",
    "-w",
    "-MG",
    "-ferror-limit=0",
    "-D_DHT_EXPORTS_PARSER="
]

def extract_header_export_info(header_path: Path) -> HeaderExportInfo:
    header_exports = HeaderExportInfo()

    index = clang.cindex.Index.create()
    tu = index.parse(str(header_path), args=_clang_args)
    cursors = list(tu.cursor.get_children())
    i = 0
    while i < (len(cursors) - 1):
        cursor = cursors[i]
        if cursor.kind == clang.cindex.CursorKind.FUNCTION_DECL:
            added = False
            if cursor.spelling == "DCLASS":
                class_cursor = cursors[i + 1]
                if class_cursor.kind == clang.cindex.CursorKind.CLASS_DECL:
                    class_name = class_cursor.spelling
                    header_exports.classes[class_name] = ExportedClassInfo()
                    added = True
            if added:
                i += 1
        i += 1

    return header_exports

def extract_module_export_info(module_name) -> ModuleExportInfo:
    module_config = configs.get_module_config(module_name)
    module_export_info = ModuleExportInfo(module_name=module_name)
    for header in module_config.reflect_headers:
        full_header_path = (module_config.module_dir / header).resolve()
        header_export_info = extract_header_export_info(full_header_path)
        module_export_info.exports[header] = header_export_info
    return module_export_info