"""Build request normalization and validation policy."""

from __future__ import annotations

from pathlib import Path

from .errors import BuildToolError
from .models import Action, ConfigurePreset, TestMode
from .requests import ConcreteRequest
from .selection import preset_cache_bool, preset_cache_string


def validate_target(target: str, *, action: Action) -> None:
    if not target:
        raise BuildToolError(f"{action.value} requires --target <target-name>.")
    allowed = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.+-")
    if any(character not in allowed for character in target):
        raise BuildToolError(f'Build target contains unsupported characters: "{target}"')


def normalize_run_request(
    request: ConcreteRequest,
    *,
    preset: ConfigurePreset | None = None,
    root: Path | None = None,
    default_project: Path | None = None,
) -> ConcreteRequest:
    if request.action is not Action.RUN:
        return request

    project_selectors = [
        argument
        for argument in request.run_arguments
        if argument == "--project" or argument.startswith("--project=")
    ]
    project_path = request.project_path
    if (
        project_path is None
        and not project_selectors
        and default_project is not None
        and preset is not None
        and preset_cache_string(preset, "DURIN_RUNTIME_VARIANT") == "DurinGame"
    ):
        project_path = default_project
    if project_path is None:
        return request

    if not project_path.is_absolute():
        if root is None:
            raise BuildToolError("A repository root is required to resolve a relative project path.")
        project_path = root / project_path
    project_path = project_path.resolve()
    if project_path.suffix.casefold() != ".dproject":
        raise BuildToolError(
            f'Project descriptor must use the .dproject extension: "{project_path}".'
        )
    if not project_path.is_file():
        raise BuildToolError(f'Project descriptor was not found: "{project_path}".')

    if project_selectors:
        raise BuildToolError(
            "run accepts project selection either through --project or through "
            "--args, but not both."
        )
    return request.with_project_path(project_path)


def validate_request(request: ConcreteRequest, preset: ConfigurePreset) -> None:
    if request.action is Action.BUILD:
        validate_target(request.target, action=request.action)
    if request.action is Action.TEST:
        if request.test_operation == "explain" and not request.target:
            raise BuildToolError("test explain requires a target or @set selection.")
        if request.test_operation == "run" and not request.target:
            raise BuildToolError(
                "test requires a target, @set selector, or all.",
                recovery="Run .\\DevTool.bat test list to inspect configured choices.",
            )
        if request.test_operation not in {"run", "list", "explain", "affected"}:
            raise BuildToolError(f'Unknown test operation "{request.test_operation}".')
        if request.test_operation != "run" and request.test_mode is not TestMode.ROUTINE:
            raise BuildToolError("test list, explain, and affected do not accept --mode.")
        if request.test_operation != "affected" and (request.test_base or request.test_explain_affected):
            raise BuildToolError("--base and --explain are accepted only by test affected.")
        if request.test_operation == "affected" and request.test_report_path is not None:
            raise BuildToolError("test affected does not accept --report; select --mode report explicitly on a bounded set.")
    if request.action is Action.REBUILD and request.target:
        validate_target(request.target, action=request.action)
    if (
        request.action is Action.TEST
        and request.test_operation == "run"
        and request.target.casefold() == "all"
        and request.test_filter
    ):
        raise BuildToolError(
            "--filter requires a single native test target and cannot be used with "
            "test all."
        )
    if request.action is Action.TEST and request.test_operation == "run":
        if request.test_mode is TestMode.ISOLATION:
            if not request.test_filter or request.target.casefold() == "all":
                raise BuildToolError(
                    "isolation mode requires a bounded selection and one case filter.",
                    recovery="Run test <target-or-@set> <suite.case> --mode isolation.",
                )
        elif request.test_filter and (
            request.target.startswith("@") or request.target.casefold() == "fast-all"
        ):
            raise BuildToolError(
                "A case filter on a set requires --mode isolation.",
                recovery=f"Run test {request.target} {request.test_filter} --mode isolation.",
            )
        if request.test_report_path is not None and request.test_mode is not TestMode.REPORT:
            raise BuildToolError("--report requires --mode report.")
        if request.test_mode in {TestMode.CHARACTERIZATION, TestMode.QUALIFICATION} and request.target.casefold() == "all":
            raise BuildToolError(
                f"{request.test_mode.value} mode requires an explicit target or @set."
            )
    if request.action is Action.TEST and not preset_cache_bool(preset, "BUILD_TESTING"):
        raise BuildToolError(f'Preset "{preset.name}" does not enable BUILD_TESTING.')
