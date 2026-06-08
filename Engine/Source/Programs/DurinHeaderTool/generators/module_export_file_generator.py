import logging
import time

from extractors.module_export_info_extractor import extract_module_export_info
import models.export_infos

def generate_module_export_file(module_name):
    start_time = time.perf_counter()
    logging.info("[DHT] Export %s: scanning reflected headers", module_name)
    export_info = extract_module_export_info(module_name)
    models.export_infos.save_module_export_file(export_info)
    elapsed_ms = (time.perf_counter() - start_time) * 1000.0
    logging.info("[DHT] Export %s: wrote %d symbols in %.0f ms", module_name, len(export_info.symbols), elapsed_ms)
