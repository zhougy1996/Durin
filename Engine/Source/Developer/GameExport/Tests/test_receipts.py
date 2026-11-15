"""Completed-build receipt publication, independent of source-dev orchestration."""
import json
from pathlib import Path
import runpy
import pytest
from test_contracts import validator

PUBLISH = runpy.run_path(str(Path(__file__).resolve().parents[4] / 'Scripts/Build/target_receipt.py'))['publish']


def fixture(tmp_path):
    engine=tmp_path/'Engine'; project=tmp_path/'Game/Game.dproject'; artifacts=tmp_path/'Build'
    engine.mkdir(); project.parent.mkdir(); artifacts.mkdir()
    project.write_text('{"ProjectName":"Game"}')
    (engine/'Core.dll').write_bytes(b'completed core'); (engine/'Game.exe').write_bytes(b'completed launcher')
    spec=artifacts/'Build.inputs'
    spec.write_text('1.0\nWin64\nx64\nDebug\nDurinGame\nfixture-abi\nGame\ncrt\t14.44.35207.0\ncompat\t0000000000000000000000000000000000000000000000000000000000000000\n'+
        'module\tCore\tEngine\t\t\nmodule\tDurinLauncher\tEngine\tCore\t\n'+
        'product\tCore\tNativeLibrary\t'+str(engine/'Core.dll')+'\tBinaries/Win64/Core.dll\t\t\n'+
        'product\tDurinLauncher\tExecutable\t'+str(engine/'Game.exe')+'\tBinaries/Win64/Game.exe\t\t\n'+
        'input\t'+str(project)+'\n')
    return dict(inputs=spec, workspace=tmp_path, artifacts=artifacts, project=project,
        catalog=artifacts/'missing-catalog.json', output=artifacts/'TargetReceipt.json')


def test_completed_receipt_is_deterministic_bounded_and_schema_valid(tmp_path):
    args=fixture(tmp_path); receipt=PUBLISH(**args)
    validator('target-receipt-v1.schema.json').validate(receipt)
    original=args['output'].read_bytes()
    (tmp_path/'Engine/Unrelated.dll').write_bytes(b'unrelated/stale')
    PUBLISH(**args)
    assert args['output'].read_bytes()==original
    assert len(receipt['Products'])==2
    assert receipt['Identity']['EngineBuild'].startswith('1.0:')
    assert any(i['Path']=='Build.inputs' for i in receipt['Inputs'])


def test_missing_declared_product_retires_success_receipt(tmp_path):
    args=fixture(tmp_path); PUBLISH(**args)
    (tmp_path/'Engine/Core.dll').unlink()
    with pytest.raises(FileNotFoundError): PUBLISH(**args)
    assert not args['output'].exists()


def test_collision_is_rejected_without_leaving_success(tmp_path):
    args=fixture(tmp_path); PUBLISH(**args)
    with args['inputs'].open('a') as out:
        out.write('file\tCore\t'+str(tmp_path/'Engine/Core.dll')+'\tBinaries/Win64/game.EXE\tRuntimeData\t\t\n')
    with pytest.raises(ValueError,match='Colliding'): PUBLISH(**args)
    assert not args['output'].exists()


def test_workspace_build_input_root_cannot_authorize_a_runtime_product(tmp_path):
    args=fixture(tmp_path)
    loose=tmp_path/'Loose.dll'; loose.write_bytes(b'outside product roots')
    text=args['inputs'].read_text().replace(str(tmp_path/'Engine/Core.dll'),str(loose))
    args['inputs'].write_text(text)
    with pytest.raises(ValueError,match='outside declared roots'): PUBLISH(**args)
    assert not args['output'].exists()


def test_receipt_carries_plugin_compatibility_and_binary_binding(tmp_path):
    args=fixture(tmp_path)
    with args['inputs'].open('a') as out:
        out.write('module\tWater\tWater\tCore\t\nproduct\tWater\tNativeLibrary\t'+str(tmp_path/'Engine/Core.dll')+'\tBinaries/Win64/Water.dll\t\t\n')
    args['catalog'].write_text(json.dumps(dict(Modules=[dict(Name='Water',Owner='Water',Host='Runtime',Phase='Providers')],Inputs=[dict(Path='Plugin/Water',Hash='descriptor-hash')])))
    receipt=PUBLISH(**args)
    validator('target-receipt-v1.schema.json').validate(receipt)
    water=next(m for m in receipt['Modules'] if m['Name']=='Water')
    assert water['Binary']=='Binaries/Win64/Water.dll' and water['Phase']=='Providers'
    assert receipt['BuildInputs']==[dict(Path='Plugin/Water',Hash='descriptor-hash')]
