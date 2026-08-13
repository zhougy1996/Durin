from typing import NamedTuple

class DurinRuntimeVariantConfig(NamedTuple):
    runtime_variant: str
    with_editor: bool
    project_name: str


_BUILTIN_WITH_EDITOR = {
    "DurinEditor": True,
    "DurinGame": False,
}


def get_runtime_variant_config(
    project_name: str,
    runtime_variant: str,
) -> DurinRuntimeVariantConfig | None:
    with_editor = _BUILTIN_WITH_EDITOR.get(runtime_variant)
    if with_editor is None:
        return None
    return DurinRuntimeVariantConfig(
        runtime_variant=runtime_variant,
        with_editor=with_editor,
        project_name=project_name,
    )
