from dataclasses import dataclass
from typing import Dict

RUNTIME_VARIANT_CONFIGS: Dict[tuple[str, str], "DurinRuntimeVariantConfig"] = {}
BUILTIN_RUNTIME_VARIANTS: Dict[str, "DurinRuntimeVariantConfig"] = {}


@dataclass
class DurinRuntimeVariantConfig:
    runtime_variant: str = ""
    with_editor: bool = False
    project_name: str = ""


def _build_builtin_runtime_variants() -> Dict[str, DurinRuntimeVariantConfig]:
    return {
        "DurinEditor": DurinRuntimeVariantConfig(
            runtime_variant="DurinEditor",
            with_editor=True,
        ),
        "DurinGame": DurinRuntimeVariantConfig(
            runtime_variant="DurinGame",
            with_editor=False,
        ),
    }


def get_runtime_variant_config(
    project_name: str,
    runtime_variant: str,
) -> DurinRuntimeVariantConfig | None:
    cache_key = (project_name, runtime_variant)
    if cache_key in RUNTIME_VARIANT_CONFIGS:
        return RUNTIME_VARIANT_CONFIGS[cache_key]

    builtin_runtime_variant = BUILTIN_RUNTIME_VARIANTS.get(runtime_variant)
    if builtin_runtime_variant is None:
        return None

    runtime_variant_config = DurinRuntimeVariantConfig(
        runtime_variant=builtin_runtime_variant.runtime_variant,
        with_editor=builtin_runtime_variant.with_editor,
        project_name=project_name,
    )
    RUNTIME_VARIANT_CONFIGS[cache_key] = runtime_variant_config
    return runtime_variant_config


BUILTIN_RUNTIME_VARIANTS = _build_builtin_runtime_variants()
