import logging
import json

from config import *
import globals as g
import helper
import clang.cindex

_clang_args = [
    "-x",
    "c++",
    "-std=c++20",
    "-D_DHT_PARSER",
    "-DNDEBUG",
    "-D_MSC_VER=1930",
    "-w",
    "-MG",
    "-M",
    "-ferror-limit=0",
    "-o clangLog.txt"
]

class ClassSymbolInfo:
    name: str
    deps: list

    def __init__(self, name: str):
        self.name = name
        self.deps = []

class FileSymbolInfo:
    relative_filepath: str
    timestamp: float
    symbols: list

    enums: list
    classes: list
    structs: list

    def __init__(self, relative_filepath: str):
        self.relative_filepath = relative_filepath
        self.symbols = []

        self.enums = []
        self.classes = []
        self.structs = []

def collect_symbols_from_header(module_source_dir, relative_header_path: str) -> FileSymbolInfo:
    info = FileSymbolInfo(relative_header_path)
    full_header_path = os.path.join(module_source_dir, relative_header_path)
    if os.path.isfile(full_header_path):
        info.timestamp = os.path.getmtime(full_header_path)
        index = clang.cindex.Index.create()
        tu = index.parse(full_header_path, args=_clang_args)
        cursors = list(tu.cursor.get_children())
        i = 0
        while i < (len(cursors) - 1):
            cursor = cursors[i]
            if cursor.kind == clang.cindex.CursorKind.FUNCTION_DECL:
                added = False
                if cursor.spelling == "DCLASS":
                    class_cursor = cursors[i + 1]
                    if class_cursor.kind == clang.cindex.CursorKind.CLASS_DECL:
                        info.symbols.append(class_cursor.spelling)
                        info.classes.append(class_cursor.spelling)
                        added = True
                if added:
                    i += 1
            i += 1

    return info


class DHTModuleSymbolTable:
    module_name: str
    symbol_table_filepath: str

    files: dict
    enums: list
    classes: list
    structs: list

    def __init__(self, module_name: str, symbol_table_filepath: str):
        self.module_name = module_name
        self.symbol_table_filepath = symbol_table_filepath
        self.files = {}
        self.classes = []
        self.enums = []
        self.structs = []

    def load(self):
        if os.path.isfile(self.symbol_table_filepath):
            with open(self.symbol_table_filepath, "r") as f:
                try:
                    data = json.load(f)
                    self.module_name = data.get("ModuleName", self.module_name)
                    self.files = data.get("HeaderFiles", {})
                    self.classes = data.get("Classes", [])
                    self.enums = data.get("Enums", [])
                    self.structs = data.get("Structs", [])
                except json.JSONDecodeError as e:
                    logging.error(f"Failed to load symbol table from {self.symbol_table_filepath}: {e}")
                    logging.info("Starting with an empty symbol table.")

    def save(self):
        os.makedirs(os.path.dirname(self.symbol_table_filepath), exist_ok=True)
        with open(self.symbol_table_filepath, "w") as f:
            data = {
                "ModuleName": self.module_name,
                "HeaderFiles": self.files,
                "Classes": self.classes,
                "Enums": self.enums,
                "Structs": self.structs
            }
            json.dump(data, f, indent=4)

    def clear(self):
        self.files.clear()
        self.classes.clear()
        self.enums.clear()
        self.structs.clear()

    def update(self, input_headers: list[str], incremental: bool = True):
        self.clear()

        for header in input_headers:
            file_info = collect_symbols_from_header(g.module_source_dir, header)
            self.files[file_info.relative_filepath] = {"Timestamp": file_info.timestamp, "Symbols": file_info.symbols}
            self.enums.extend(file_info.enums)
            self.classes.extend(file_info.classes)
            self.structs.extend(file_info.structs)

if __name__ == "__main__":
    helper.init_logging()
    helper.init_clang()
    # Example usage
    module_name = "Engine"
    dht_symbol_table = DHTModuleSymbolTable(module_name, helper.get_symbol_table_filepath(module_name))

    dht_symbol_table.load()

    input_headers = [
        "Public/Engine/Actor.h",
        "Public/Actors/StaticMeshActor.h"
    ]

    dht_symbol_table.update(input_headers, incremental=False)
    dht_symbol_table.save()


