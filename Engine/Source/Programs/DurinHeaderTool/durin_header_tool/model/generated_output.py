from collections.abc import Iterable
from pathlib import Path


def reflected_header_stem(logical_header: str) -> str:
    return Path(logical_header.replace("\\", "/")).stem


def validate_unique_reflected_header_stems(reflect_headers: Iterable[str]) -> None:
    headers_by_stem: dict[str, list[str]] = {}
    for header in reflect_headers:
        headers_by_stem.setdefault(reflected_header_stem(header), []).append(header)
    collisions = {
        stem: sorted(headers)
        for stem, headers in headers_by_stem.items()
        if len(headers) > 1
    }
    if collisions:
        details = "; ".join(
            f"'{stem}': {', '.join(headers)}"
            for stem, headers in sorted(collisions.items())
        )
        raise ValueError(f"Reflected header basenames must be unique ({details}).")


def module_generated_source_name(module_name: str) -> str:
    return f"{module_name}.module.gen.cpp"


def header_generated_names(logical_header: str) -> tuple[str, str]:
    stem = reflected_header_stem(logical_header)
    return f"{stem}.gen.h", f"{stem}.gen.cpp"


def generated_output_names(module_name: str, reflect_headers: Iterable[str]) -> list[str]:
    headers = list(reflect_headers)
    validate_unique_reflected_header_stems(headers)
    outputs = [module_generated_source_name(module_name)]
    for header in headers:
        header_name, source_name = header_generated_names(header)
        outputs.extend((source_name, header_name))
    return outputs


def header_generated_paths(output_dir: Path, logical_header: str) -> tuple[Path, Path]:
    header_name, source_name = header_generated_names(logical_header)
    return output_dir / header_name, output_dir / source_name


def generated_output_paths(
    output_dir: Path,
    module_name: str,
    reflect_headers: Iterable[str],
) -> list[Path]:
    return [output_dir / name for name in generated_output_names(module_name, reflect_headers)]
