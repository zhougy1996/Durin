"""Script exposure is native metadata; Python declarations do not implement it."""
import pytest
from test_reflection_function import parse as parse_fixture, declaration

def parse(tmp_path, body):
    return parse_fixture(tmp_path, "namespace Durin {\n" + body.replace("GENERATED_BODY()", "GENERATED_BODY()\n") + "\n}")
from durin_header_tool.writers.reflection_source_writer import generate_cpp_content


def test_script_metadata_survives_generated_registration(tmp_path):
    info = parse(tmp_path, '''
DSTRUCT(ScriptType, ScriptName="Value") struct FValue { GENERATED_BODY() DPROPERTY(ScriptReadWrite) int32 Count; };
DENUM(ScriptType) enum class EMode : unsigned char { Idle, Ready DMETA(ScriptName="Available") };
DCLASS(ScriptType, ScriptName="Service") class DService { GENERATED_BODY()
 DPROPERTY(Edit, ScriptReadWrite, ScriptName="amount") int32 Count;
 DPROPERTY(ScriptNoExport) int32 Hidden;
 DFUNCTION(ScriptCallable, ScriptName="execute") void Run();
};
''')
    generated = generate_cpp_content(info, {})
    assert info.classes[0].script == {"ScriptType": True, "ScriptName": "Service"}
    assert info.classes[0].functions[0].script["ScriptCallable"]
    assert 'EScriptExposure::Type, "Service", "Fixture"' in generated
    assert 'EScriptExposure::Callable, "execute"' in generated
    assert 'EPropertyFlags::ScriptReadWrite' in generated
    assert 'EPropertyFlags::ScriptNoExport' in generated
    assert '{ "ScriptName", "amount" }' in generated
    assert '"Available"' in generated


@pytest.mark.parametrize("specifiers", [
    "ScriptType, ScriptType", "ScriptTypo", "ScriptName=17", "ScriptType=true", 'ScriptName="bad name"',
])
def test_invalid_type_exposure_is_rejected(tmp_path, specifiers):
    with pytest.raises(ValueError, match="[Ss]cript"):
        parse(tmp_path, f"DCLASS({specifiers}) class DInvalid {{ GENERATED_BODY() }};")


@pytest.mark.parametrize("specifiers", [
    "ScriptReadOnly, ScriptReadWrite", "ReadOnly, ScriptReadWrite", "NonTransactional, ScriptReadWrite",
    "ScriptReadWrite", "ScriptReadOnly, ScriptReadOnly", 'Edit, ScriptName="bad name"', "ScriptTypo",
])
def test_invalid_object_property_exposure_is_rejected(tmp_path, specifiers):
    with pytest.raises(ValueError, match="[Ss]cript"):
        parse(tmp_path, declaration(f"DPROPERTY({specifiers}) int32 Value;"))


@pytest.mark.parametrize("specifiers", [
    "ScriptCallable, ScriptCallable", "ScriptCallable=true", "ScriptReadWrite", 'ScriptName=""',
])
def test_invalid_function_exposure_is_rejected(tmp_path, specifiers):
    with pytest.raises(ValueError, match="[Ss]cript"):
        parse(tmp_path, declaration(f"DFUNCTION({specifiers}) void Run();"))
