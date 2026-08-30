import io
from dataclasses import replace
from unittest import mock

import pytest

from durin_dev_tool.bootstrap.models import BootstrapError
from durin_dev_tool.build import build_context, errors, models
from durin_dev_tool.build import dependencies
from durin_dev_tool.build.output import BuildOutput

from . import build_request_fixtures as request_fixtures


def make_context(
    *,
    configuration: str = "Debug",
    testing: str = "ON",
    tracy: str = "OFF",
    defines: tuple[str, ...] = (),
) -> build_context.BuildContext:
    preset = request_fixtures.make_preset(testing=testing)
    cache = dict(preset.values["cacheVariables"])
    cache["CMAKE_BUILD_TYPE"] = configuration
    cache["DURIN_ENABLE_TRACY"] = tracy
    preset = replace(preset, values={**preset.values, "cacheVariables": cache})
    request = request_fixtures.command_request(
        models.Action.CONFIGURE,
        options=request_fixtures.BuildActionOptions(defines=defines),
    )
    return build_context.BuildContext(
        request,
        models.LocalConfig(),
        request_fixtures.make_profile(),
        {preset.name: preset},
        preset,
        "windows",
        cmake="cmake",
        environment={"PATH": "ready"},
    )


def make_output() -> BuildOutput:
    return BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())


def test_debug_configure_prepares_only_debug_ordinary_and_test_dependencies() -> None:
    context = make_context()
    with mock.patch.object(dependencies.RepositoryContext, "load", return_value=mock.sentinel.repository), mock.patch.object(dependencies, "prepare_dependencies") as prepare:
        dependencies.prepare_configure_dependencies(context, make_output())
    request = prepare.call_args.args[1]
    assert request.use_all
    assert request.config == "Debug"
    assert request.with_tests
    assert not request.with_development
    assert prepare.call_args.kwargs["environment"] == {"PATH": "ready"}


def test_shipping_reuses_release_dependencies() -> None:
    assert dependencies.dependency_configuration(make_context(configuration="Shipping")) == "Release"


def test_profiling_configure_prepares_tracy_client_without_tools() -> None:
    context = make_context(configuration="Release", tracy="ON")
    with mock.patch.object(dependencies.RepositoryContext, "load", return_value=mock.sentinel.repository), mock.patch.object(dependencies, "prepare_dependencies") as prepare:
        dependencies.prepare_configure_dependencies(context, make_output())
    assert prepare.call_count == 2
    tracy_request = prepare.call_args_list[1].args[1]
    assert tracy_request.libraries == "tracy"
    assert not tracy_request.with_development
    assert "tracy-tools" not in tracy_request.libraries


def test_configure_definitions_override_preset_dependency_selection() -> None:
    context = make_context(
        defines=(
            "CMAKE_BUILD_TYPE:STRING=Release",
            "BUILD_TESTING:BOOL=OFF",
            "DURIN_ENABLE_TRACY:BOOL=ON",
        )
    )
    with mock.patch.object(dependencies.RepositoryContext, "load", return_value=mock.sentinel.repository), mock.patch.object(dependencies, "prepare_dependencies") as prepare:
        dependencies.prepare_configure_dependencies(context, make_output())
    ordinary_request = prepare.call_args_list[0].args[1]
    assert ordinary_request.config == "Release"
    assert not ordinary_request.with_tests
    assert prepare.call_args_list[1].args[1].libraries == "tracy"


def test_dependency_failure_is_reported_as_build_failure() -> None:
    context = make_context()
    with mock.patch.object(dependencies.RepositoryContext, "load", return_value=mock.sentinel.repository), mock.patch.object(dependencies, "prepare_dependencies", side_effect=BootstrapError("download failed")), pytest.raises(errors.BuildToolError, match="Could not prepare configure dependencies"):
        dependencies.prepare_configure_dependencies(context, make_output())
