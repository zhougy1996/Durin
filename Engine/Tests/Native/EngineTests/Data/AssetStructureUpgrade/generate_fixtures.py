from pathlib import Path
import shutil
import struct


REPOSITORY_ROOT = Path(__file__).resolve().parents[6]
FIXTURE_ROOT = Path(__file__).resolve().parent
LEGACY_SOURCE = FIXTURE_ROOT / "LegacyStaticMeshLevel.dasset"
MESH_SOURCE = REPOSITORY_ROOT / "Sandbox/Content/Models/Mesh_Teapot.dasset"
MODEL_SOURCE = REPOSITORY_ROOT / "Sandbox/SourceAssets/Models/Models/Mesh_Teapot.obj"


def main() -> None:
    legacy_bytes = LEGACY_SOURCE.read_bytes()
    actors_record = struct.pack("<Q", len("Actors")) + b"Actors"
    future_record = struct.pack("<Q", len("Future")) + b"Future"
    if legacy_bytes.count(actors_record) != 1:
        raise RuntimeError("Expected exactly one serialized Actors field.")

    (FIXTURE_ROOT / "UnknownNewerLevel.dasset").write_bytes(
        legacy_bytes.replace(actors_record, future_record)
    )
    shutil.copyfile(MESH_SOURCE, FIXTURE_ROOT / "Mesh_Teapot.dasset")
    shutil.copyfile(MODEL_SOURCE, FIXTURE_ROOT / "Mesh_Teapot.obj")


if __name__ == "__main__":
    main()
