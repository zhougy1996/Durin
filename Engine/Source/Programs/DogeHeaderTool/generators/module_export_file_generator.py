from extractors.module_export_info_extractor import extract_module_export_info
import models.export_infos

def generate_module_export_file(module_name):
    export_info = extract_module_export_info(module_name)
    models.export_infos.save_module_export_file(export_info)