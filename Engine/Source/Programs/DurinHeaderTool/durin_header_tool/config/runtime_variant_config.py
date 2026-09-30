from typing import NamedTuple

class DurinRuntimeVariantConfig(NamedTuple):
    runtime_variant: str
    with_editor: bool
    with_editor_only_data: bool
    project_name: str


_BUILTIN_WITH_EDITOR = {
    "DurinEditor": True,
    "DurinGame": False,
}

_BUILTIN_WITH_EDITOR_ONLY_DATA = {
    "DurinEditor": True,
    "DurinGame": False,
}


def get_runtime_variant_config(
    project_name: str,
    runtime_variant: str,
) -> DurinRuntimeVariantConfig | None:
    with_editor = _BUILTIN_WITH_EDITOR.get(runtime_variant)
    with_editor_only_data = _BUILTIN_WITH_EDITOR_ONLY_DATA.get(runtime_variant)
    if with_editor is None or with_editor_only_data is None:
        return None
    if with_editor and not with_editor_only_data:
        raise ValueError(
            f"runtime variant '{runtime_variant}' enables editor behavior without editor-only data"
        )
    return DurinRuntimeVariantConfig(
        runtime_variant=runtime_variant,
        with_editor=with_editor,
        with_editor_only_data=with_editor_only_data,
        project_name=project_name,
    )
