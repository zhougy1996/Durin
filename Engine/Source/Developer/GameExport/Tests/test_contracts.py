"""Stage 0 golden-format checks; native execution gates remain in the plan."""
import copy
import json
from pathlib import Path

import pytest
from jsonschema import Draft202012Validator
from referencing import Registry, Resource


OWNER = Path(__file__).resolve().parents[1]
ENGINE = OWNER.parents[2]
CONTRACTS = OWNER / "Contracts"
FIXTURES = ENGINE / "Tests/Fixtures/GameExport"


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


SCHEMAS = {p.name: load(p) for p in CONTRACTS.glob("*.schema.json")}
REGISTRY = Registry().with_resources(
    (name, Resource.from_contents(value)) for name, value in SCHEMAS.items()
)
CASES = load(FIXTURES / "cases.json")


def validator(name):
    return Draft202012Validator(SCHEMAS[name], registry=REGISTRY)


@pytest.mark.parametrize("name", sorted(SCHEMAS))
def test_schemas_are_valid_and_all_references_resolve_offline(name):
    Draft202012Validator.check_schema(SCHEMAS[name])

    def walk(value):
        if isinstance(value, dict):
            if "$ref" in value:
                REGISTRY.resolver(name).lookup(value["$ref"])
            for child in value.values():
                walk(child)
        elif isinstance(value, list):
            for child in value:
                walk(child)

    walk(SCHEMAS[name])


GOLDENS = [
    *[("runtime-v2.schema.json", f"runtime/{name}.json") for name in
      ["no-plugin", "no-plugin-debug", "content-only", "dependent-runtime-editor", "dynamic-native-data"]],
    ("runtime-cook-v2.schema.json", "runtime-cook.json"),
    ("target-receipt-v1.schema.json", "receipts/dynamic-native-data.json"),
    *[("request-v1.schema.json", f"protocol/{name}-request.json") for name in
      ["editor", "devtool", "direct"]],
    ("cancel-v1.schema.json", "protocol/cancel.json"),
    ("event-v1.schema.json", "protocol/version-mismatch-result.json"),
    ("event-v1.schema.json", "protocol/unsupported-backend-result.json"),
    ("component-v1.schema.json", "component.json"),
]


@pytest.mark.parametrize("schema,fixture", GOLDENS)
def test_portable_goldens(schema, fixture):
    validator(schema).validate(load(FIXTURES / fixture))


@pytest.mark.parametrize("case", CASES["StructuralInvalid"], ids=lambda c: c["Name"])
def test_negative_goldens_rejected(case):
    value = load(FIXTURES / case["Fixture"])
    parent = value
    for field in case["Field"][:-1]:
        parent = parent[field]
    key = case["Field"][-1]
    if case.get("Remove"):
        del parent[key]
    else:
        parent[key] = case["Value"]
    assert not validator(case["Schema"]).is_valid(value)


@pytest.mark.parametrize("relative", CASES["InvalidRelativePaths"])
def test_manifest_and_receipt_reject_lexical_escapes(relative):
    value = load(FIXTURES / "runtime/no-plugin.json")
    value["Executable"] = relative
    assert not validator("runtime-v2.schema.json").is_valid(value)
    receipt = load(FIXTURES / "receipts/dynamic-native-data.json")
    receipt["Products"][0]["Destination"] = relative
    assert not validator("target-receipt-v1.schema.json").is_valid(receipt)


@pytest.mark.parametrize("schema,fixture", GOLDENS)
def test_unknown_fields_are_rejected(schema, fixture):
    value = load(FIXTURES / fixture)
    value["FutureField"] = True
    assert not validator(schema).is_valid(value)


def test_clients_submit_equivalent_requests_except_correlation():
    requests = [load(FIXTURES / f"protocol/{client}-request.json")
                for client in ["editor", "devtool", "direct"]]
    for request in requests:
        del request["RequestId"]
    assert requests[0] == requests[1] == requests[2]


def test_stage_request_cannot_request_implicit_build_or_cook():
    request = load(FIXTURES / "protocol/direct-request.json")
    request.update(Operation="stage", Receipt="/inputs/receipt.json", CookDirectory="/inputs/Cook")
    validator("request-v1.schema.json").validate(request)
    for field in ["Receipt", "CookDirectory"]:
        invalid = copy.deepcopy(request)
        del invalid[field]
        assert not validator("request-v1.schema.json").is_valid(invalid)
    for field in ["Build", "Cook"]:
        invalid = copy.deepcopy(request)
        invalid[field] = "auto"
        assert not validator("request-v1.schema.json").is_valid(invalid)


@pytest.mark.parametrize("configuration,crt", [
    ("Release", "developer-debug-crt"), ("Debug", "vcredist-x64"),
])
def test_target_configuration_requires_its_declared_crt_policy(configuration, crt):
    value = load(FIXTURES / "runtime/no-plugin.json")
    value["Identity"]["Configuration"] = configuration
    value["Prerequisites"]["CRT"] = crt
    assert not validator("runtime-v2.schema.json").is_valid(value)


def test_system_policy_does_not_authorize_optional_dlls_or_wildcard_api_sets():
    policy = load(CONTRACTS / "win64-system-v1.json")
    names = policy["SystemLibraries"] + policy["ApiSetContracts"]
    assert len(names) == len(set(names))
    assert all(n == n.lower() and "/" not in n and "\\" not in n and "*" not in n for n in names)
    assert "ucrtbase.dll" in names and "kernel32.dll" in names
    assert "vulkan-1.dll" not in names and "tracyclient.dll" not in names
    assert not set(policy["ReleaseCRTLibraries"]) & set(names)
    assert "api-ms-win-optional-fixture-l1-1-0.dll" not in names
    assert not policy["SystemDirectoryProbeAuthorizesImport"]
    assert not policy["ApiSetPrefixAuthorizesImport"]


def test_cancel_acknowledgement_follows_cleanup_and_precedes_terminal_result():
    events = load(FIXTURES / "protocol/cancelled-events.json")
    for event in events:
        validator("event-v1.schema.json").validate(event)
    assert [event["Sequence"] for event in events] == list(range(len(events)))
    assert {event["RequestId"] for event in events} == {"Export1"}
    assert [event["Kind"] for event in events] == ["progress", "cancelAcknowledged", "result"]
    assert events[0]["Phase"] == "cleanup"
    assert events[1]["ChildrenRetired"] and events[2]["ChildrenRetired"]
    assert events[2]["ExitCode"] == 130 and events[2]["Output"] is None
    invalid = copy.deepcopy(events[1])
    invalid["ChildrenRetired"] = False
    assert not validator("event-v1.schema.json").is_valid(invalid)


@pytest.mark.parametrize("status,code,category", [
    ("succeeded", 8, "stage"), ("failed", 0, "none"),
    ("cancelled", 0, "none"), ("failed", 6, "capability"),
])
def test_result_exit_mapping_cannot_report_false_success(status, code, category):
    value = load(FIXTURES / "protocol/unsupported-backend-result.json")
    value.update(Status=status, ExitCode=code, Category=category)
    assert not validator("event-v1.schema.json").is_valid(value)


@pytest.mark.parametrize("output,fingerprint,valid", [
    (None, None, True), (None, "a" * 64, False),
    ("/exports/fresh", "a" * 64, True), ("/exports/fresh", None, False),
])
def test_success_fingerprint_distinguishes_configuration_preview_from_publication(output, fingerprint, valid):
    value = load(FIXTURES / "protocol/unsupported-backend-result.json")
    value.update(Status="succeeded", ExitCode=0, Category="none", Code="completed",
                 Output=output, PlanFingerprint=fingerprint)
    assert validator("event-v1.schema.json").is_valid(value) == valid


@pytest.mark.parametrize("fixture", sorted((FIXTURES / "runtime").glob("*.json")), ids=lambda p:p.stem)
def test_runtime_goldens_bind_required_files_mounts_and_selected_owners(fixture):
    value = load(fixture)
    files = {f["Path"]: f["Sha256"] for f in value["Files"]}
    assert len(files) == len(value["Files"])
    assert value["Executable"] in files
    assert all(p in files for p in value["ConfigurationFiles"].values())
    for key in ["Manifest", "Snapshot", "ShaderLibrary"]:
        binding = value["Cook"][key]
        assert files[binding["Path"]] == binding["Sha256"]
    mounts = {m["VirtualRoot"] for m in value["Mounts"]}
    assert len(mounts) == len(value["Mounts"])
    assert {"/Engine/", "/Game/"} <= mounts
    for mount in value["Mounts"]:
        assert set(mount["CanReference"]) <= mounts
        if mount["VirtualRoot"].startswith("/Plugins/"):
            assert mount["VirtualRoot"].split("/")[2] in value["Plugins"]
    for binary in value["Binaries"]:
        assert binary["Owner"] in value["Plugins"]
        assert binary["Path"] in files
        assert all(p in files for p in binary["RuntimeDependencies"])
    assert "Editor" not in json.dumps(value["Binaries"])


def test_dynamic_module_and_non_asset_data_match_receipt_and_cook_selection():
    runtime = load(FIXTURES / "runtime/dynamic-native-data.json")
    receipt = load(FIXTURES / "receipts/dynamic-native-data.json")
    snapshot = load(FIXTURES / "runtime-cook.json")
    assert runtime["Identity"] == receipt["Identity"]
    assert snapshot["Platform"] == runtime["Identity"]["Platform"]
    assert runtime["Cook"]["SelectionFingerprint"] == receipt["SelectionFingerprint"] == snapshot["SelectionFingerprint"]
    frozen = json.loads(snapshot["Selection"])
    assert runtime["Plugins"] == [p["Id"] for p in frozen["Plugins"]]
    assert [b["Module"] for b in runtime["Binaries"]] == [m["Name"] for m in frozen["Modules"]]
    products = {p["Destination"]: p for p in receipt["Products"]}
    modules = {m["Name"]: m for m in receipt["Modules"]}
    assert {p["Module"] for p in products.values()} == modules.keys()
    for module in modules.values():
        assert set(module["Linked"] + module["Dynamic"]) <= modules.keys()
        assert set(module["RuntimeFiles"]) <= products.keys()
    assert modules["WeatherRuntime"]["Dynamic"] == ["WeatherNative"]
    assert products["Data/weather.bin"]["Kind"] == "RuntimeData"
    assert products["Binaries/Win64/weather-native.dll"]["Kind"] == "NativeLibrary"
    assert all(p["Host"] == "Runtime" for p in products.values())
    authored = CASES["AuthoredPlugins"][1]
    assert authored["Dependencies"] == ["Scenery"]
    assert any(m["Host"] == "Editor" for m in authored["Modules"])
    assert all(m["Module"] != "WeatherEditor" for m in runtime["Binaries"])


def test_bootstrap_execution_matrix_has_portable_inputs_and_explicit_expected_failures():
    cases = {c["Name"]: c for c in CASES["Bootstrap"]}
    assert cases["missing-manifest"]["Manifest"] is None
    assert cases["beneath-source-without-manifest"]["SourceAncestor"]
    with pytest.raises(json.JSONDecodeError):
        load(FIXTURES / cases["corrupt-manifest"]["Fixture"])
    assert cases["symlink-escape"]["Alias"]["Target"] == "outside-deployment"
    assert cases["unrelated-cwd"]["Cwd"] == "unrelated"
    assert cases["unrelated-cwd"]["ExpectedExit"] == 0
    assert all(c["ExpectedExit"] == 2 for n, c in cases.items() if n != "unrelated-cwd")


def test_tool_component_binds_all_referenced_metadata():
    component = load(FIXTURES / "component.json")
    files = {f["Path"] for f in component["Files"]}
    assert len(files) == len(component["Files"])
    assert component["Worker"] in files
    assert set(component["Schemas"] + component["ProviderMetadata"] + [component["BackendMetadata"]]) <= files
    assert "build-source" not in component["Capabilities"]


def test_historical_baselines_remain_distinct_from_new_format_qualification():
    value = load(FIXTURES / "baseline.json")
    assert value["Revision"] == "871de3b06486954716dba968b25dad4163b904ad"
    assert {c["Configuration"] for c in value["Configurations"]} == {"Debug", "Release"}
    for receipt in value["Configurations"]:
        assert receipt["Status"] == "passed"
        assert {p["Name"] for p in receipt["Processes"]} == {"exported", "relocated"}
        assert all(p["Exit"] == 0 and p["Ticks"] == 120 for p in receipt["Processes"])
        for report in receipt["Reports"]:
            assert report["Path"].startswith("Build/Exports/")
            assert len(report["Sha256"]) == 64
    assert "not v2" in value["Purpose"]
