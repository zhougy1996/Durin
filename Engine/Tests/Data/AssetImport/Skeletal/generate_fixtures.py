#!/usr/bin/env python3
"""Generate the repository-owned skeletal glTF contract fixtures."""

from __future__ import annotations

import base64
import copy
import json
import math
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parent
EXPECTED_COMPATIBILITY_HASH = "be0f679ef83133e5acfab7f12b688f54"


def matrix_multiply(a: list[list[float]], b: list[list[float]]) -> list[list[float]]:
    return [[sum(a[row][index] * b[index][column] for index in range(4))
             for column in range(4)] for row in range(4)]


def matrix_inverse(value: list[list[float]]) -> list[list[float]]:
    augmented = [row[:] + [1.0 if row_index == column else 0.0 for column in range(4)]
                 for row_index, row in enumerate(value)]
    for column in range(4):
        pivot = max(range(column, 4), key=lambda row: abs(augmented[row][column]))
        if abs(augmented[pivot][column]) <= 1.0e-12:
            raise ValueError("fixture matrix is singular")
        augmented[column], augmented[pivot] = augmented[pivot], augmented[column]
        divisor = augmented[column][column]
        augmented[column] = [entry / divisor for entry in augmented[column]]
        for row in range(4):
            if row == column:
                continue
            factor = augmented[row][column]
            augmented[row] = [augmented[row][index] - factor * augmented[column][index]
                              for index in range(8)]
    return [row[4:] for row in augmented]


def quaternion_matrix(quaternion: list[float]) -> list[list[float]]:
    x, y, z, w = quaternion
    length = math.sqrt(x * x + y * y + z * z + w * w)
    x, y, z, w = x / length, y / length, z / length, w / length
    return [
        [1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - z * w),
         2.0 * (x * z + y * w), 0.0],
        [2.0 * (x * y + z * w), 1.0 - 2.0 * (x * x + z * z),
         2.0 * (y * z - x * w), 0.0],
        [2.0 * (x * z - y * w), 2.0 * (y * z + x * w),
         1.0 - 2.0 * (x * x + y * y), 0.0],
        [0.0, 0.0, 0.0, 1.0],
    ]


def matrix_quaternion(value: list[list[float]]) -> list[float]:
    trace = value[0][0] + value[1][1] + value[2][2]
    if trace > 0.0:
        scale = math.sqrt(trace + 1.0) * 2.0
        quaternion = [(value[2][1] - value[1][2]) / scale,
                      (value[0][2] - value[2][0]) / scale,
                      (value[1][0] - value[0][1]) / scale,
                      0.25 * scale]
    elif value[0][0] > value[1][1] and value[0][0] > value[2][2]:
        scale = math.sqrt(1.0 + value[0][0] - value[1][1] - value[2][2]) * 2.0
        quaternion = [0.25 * scale, (value[0][1] + value[1][0]) / scale,
                      (value[0][2] + value[2][0]) / scale,
                      (value[2][1] - value[1][2]) / scale]
    elif value[1][1] > value[2][2]:
        scale = math.sqrt(1.0 + value[1][1] - value[0][0] - value[2][2]) * 2.0
        quaternion = [(value[0][1] + value[1][0]) / scale, 0.25 * scale,
                      (value[1][2] + value[2][1]) / scale,
                      (value[0][2] - value[2][0]) / scale]
    else:
        scale = math.sqrt(1.0 + value[2][2] - value[0][0] - value[1][1]) * 2.0
        quaternion = [(value[0][2] + value[2][0]) / scale,
                      (value[1][2] + value[2][1]) / scale, 0.25 * scale,
                      (value[1][0] - value[0][1]) / scale]
    length = math.sqrt(sum(component * component for component in quaternion))
    return [component / length for component in quaternion]


def trs_matrix(node: dict) -> list[list[float]]:
    translation = node.get("translation", [0.0, 0.0, 0.0])
    rotation = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
    scale = node.get("scale", [1.0, 1.0, 1.0])
    result = quaternion_matrix(rotation)
    for row in range(3):
        for column in range(3):
            result[row][column] *= scale[column]
    for row in range(3):
        result[row][3] = translation[row]
    return result


def flatten_column_major(value: list[list[float]]) -> list[float]:
    return [value[row][column] for column in range(4) for row in range(4)]


def clean_number(value: float) -> float:
    if abs(value) < 1.0e-7:
        return 0.0
    rounded = round(value, 7)
    return float(rounded)


def clean_matrix(value: list[list[float]]) -> list[list[float]]:
    return [[clean_number(entry) for entry in row] for row in value]


class BufferBuilder:
    def __init__(self) -> None:
        self.bytes = bytearray()
        self.views: list[dict] = []
        self.accessors: list[dict] = []
        self.accessor_offsets: dict[str, int] = {}

    def accessor(self, name: str, values: list, component_type: int, kind: str,
                 fmt: str, normalized: bool = False, include_bounds: bool = False) -> int:
        component_size = struct.calcsize("<" + fmt)
        while len(self.bytes) % min(component_size, 4):
            self.bytes.append(0)
        offset = len(self.bytes)
        flattened = [component for value in values
                     for component in (value if isinstance(value, (list, tuple)) else [value])]
        self.bytes.extend(struct.pack("<" + fmt * len(flattened), *flattened))
        view_index = len(self.views)
        self.views.append({"buffer": 0, "byteOffset": offset,
                           "byteLength": len(self.bytes) - offset})
        accessor = {"bufferView": view_index, "componentType": component_type,
                    "count": len(values), "type": kind}
        if normalized:
            accessor["normalized"] = True
        if include_bounds:
            dimensions = len(values[0]) if isinstance(values[0], (list, tuple)) else 1
            accessor["min"] = [min(value[index] if dimensions > 1 else value for value in values)
                               for index in range(dimensions)]
            accessor["max"] = [max(value[index] if dimensions > 1 else value for value in values)
                               for index in range(dimensions)]
        accessor_index = len(self.accessors)
        self.accessors.append(accessor)
        self.accessor_offsets[name] = offset
        return accessor_index


def build_contract() -> tuple[dict, bytes, dict[str, int], dict]:
    builder = BufferBuilder()
    positions_a = [[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]]
    positions_b = [[1.0, 0.0, 0.0], [1.0, 1.0, 0.0], [0.0, 1.0, 0.0]]
    normals = [[0.0, 0.0, 1.0]] * 3
    tangents = [[1.0, 0.0, 0.0, 1.0]] * 3
    uv_float = [[0.0, 0.0], [1.0, 0.0], [0.0, 1.0]]
    uv_u16 = [[65535, 0], [65535, 65535], [0, 65535]]
    joints_u8 = [[0, 1, 2, 3], [0, 1, 2, 1], [0, 0, 1, 2]]
    joints_u16 = [[3, 2, 1, 0], [0, 1, 2, 3], [2, 3, 0, 1]]
    weights_float = [[0.4, 0.3, 0.2, 0.1], [0.4, 0.3, 0.2, 0.1],
                     [0.1, 0.4, 0.2, 0.3]]
    weights_u8 = [[102, 76, 51, 26], [255, 0, 0, 0], [64, 64, 64, 63]]

    position_a = builder.accessor("positions_a", positions_a, 5126, "VEC3", "f", include_bounds=True)
    normal_a = builder.accessor("normals_a", normals, 5126, "VEC3", "f")
    tangent_a = builder.accessor("tangents_a", tangents, 5126, "VEC4", "f")
    uv_a = builder.accessor("uv_a", uv_float, 5126, "VEC2", "f")
    joint_a = builder.accessor("joints_a", joints_u8, 5121, "VEC4", "B")
    weight_a = builder.accessor("weights_a", weights_float, 5126, "VEC4", "f")
    index_a = builder.accessor("indices_a", [0, 1, 2], 5123, "SCALAR", "H")
    position_b = builder.accessor("positions_b", positions_b, 5126, "VEC3", "f", include_bounds=True)
    normal_b = builder.accessor("normals_b", normals, 5126, "VEC3", "f")
    tangent_b = builder.accessor("tangents_b", tangents, 5126, "VEC4", "f")
    uv_b = builder.accessor("uv_b", uv_u16, 5123, "VEC2", "H", normalized=True)
    joint_b = builder.accessor("joints_b", joints_u16, 5123, "VEC4", "H")
    weight_b = builder.accessor("weights_b", weights_u8, 5121, "VEC4", "B", normalized=True)
    index_b = builder.accessor("indices_b", [0, 1, 2], 5123, "SCALAR", "H")

    root_rotation = [0.0, math.sin(math.pi / 8.0), 0.0, math.cos(math.pi / 8.0)]
    knee_rotation = [0.0, 0.0, math.sin(math.pi / 4.0), math.cos(math.pi / 4.0)]
    nodes = [
        {"name": "SceneRoot", "rotation": root_rotation, "children": [1, 2, 4, 6]},
        {"name": "MeshA", "translation": [1.0, 2.0, 3.0], "mesh": 0, "skin": 0},
        {"name": "Hip", "translation": [0.0, 1.0, 0.0], "children": [3]},
        {"name": "Knee", "translation": [0.0, 1.0, 0.0], "rotation": knee_rotation},
        {"name": "Shoulder", "translation": [0.0, 0.0, 1.0], "scale": [1.0, 2.0, 1.0], "children": [5]},
        {"name": "Hand", "translation": [1.0, 0.0, 0.0]},
        {"name": "MeshB", "translation": [-1.0, 0.0, 0.0],
         "rotation": [0.0, math.sin(math.pi / 4.0), 0.0, math.cos(math.pi / 4.0)],
         "scale": [1.0, 2.0, 1.0], "mesh": 1, "skin": 1},
    ]
    parents = {child: parent for parent, node in enumerate(nodes) for child in node.get("children", [])}
    globals_by_node: dict[int, list[list[float]]] = {}

    def global_matrix(index: int) -> list[list[float]]:
        if index not in globals_by_node:
            local = trs_matrix(nodes[index])
            globals_by_node[index] = (matrix_multiply(global_matrix(parents[index]), local)
                                      if index in parents else local)
        return globals_by_node[index]

    inverse_binds = [flatten_column_major(matrix_inverse(global_matrix(index))) for index in [2, 3, 4, 5]]
    inverse_bind = builder.accessor("inverse_binds", inverse_binds, 5126, "MAT4", "f")
    walk_times = builder.accessor("walk_times", [0.0, 1.0, 2.0], 5126, "SCALAR", "f", include_bounds=True)
    walk_translation_values = [[0.0, 1.0, 0.0], [0.0, 2.0, 0.0], [0.0, 3.0, 0.0]]
    walk_translation = builder.accessor("walk_translation", walk_translation_values, 5126, "VEC3", "f")
    walk_rotation_values = [[0.0, 0.0, 0.0, 1.0], knee_rotation,
                            [0.0, 0.0, 1.0, 0.0]]
    walk_rotation = builder.accessor("walk_rotation", walk_rotation_values, 5126, "VEC4", "f")
    walk_scale_values = [[1.0, 1.0, 1.0], [1.0, 2.0, 1.0], [1.0, 1.0, 1.0]]
    walk_scale = builder.accessor("walk_scale", walk_scale_values, 5126, "VEC3", "f")
    pose_times = builder.accessor("pose_times", [0.0, 0.5], 5126, "SCALAR", "f", include_bounds=True)
    pose_translation_values = [[0.0, 0.0, 0.0], [0.25, 0.0, 0.0]]
    pose_translation = builder.accessor("pose_translation", pose_translation_values, 5126, "VEC3", "f")

    document = {
        "asset": {"version": "2.0", "generator": "Durin skeletal contract fixture generator"},
        "scene": 0,
        "scenes": [{"name": "ContractScene", "nodes": [0]}],
        "nodes": nodes,
        "buffers": [{"byteLength": len(builder.bytes)}],
        "bufferViews": builder.views,
        "accessors": builder.accessors,
        "materials": [
            {"name": "Red", "pbrMetallicRoughness": {"baseColorFactor": [1.0, 0.0, 0.0, 1.0]}},
            {"name": "Blue", "pbrMetallicRoughness": {"baseColorFactor": [0.0, 0.0, 1.0, 1.0]}},
        ],
        "meshes": [{"name": "ContractMeshA", "primitives": [
            {"attributes": {"POSITION": position_a, "NORMAL": normal_a, "TANGENT": tangent_a,
                            "TEXCOORD_0": uv_a, "JOINTS_0": joint_a, "WEIGHTS_0": weight_a},
             "indices": index_a, "material": 0, "mode": 4},
            {"attributes": {"POSITION": position_b, "NORMAL": normal_b, "TANGENT": tangent_b,
                            "TEXCOORD_0": uv_b, "JOINTS_0": joint_b, "WEIGHTS_0": weight_b},
             "indices": index_b, "material": 1, "mode": 4},
        ]}, {"name": "ContractMeshB", "primitives": [
            {"attributes": {"POSITION": position_a, "NORMAL": normal_a, "TANGENT": tangent_a,
                            "TEXCOORD_0": uv_a, "JOINTS_0": joint_a, "WEIGHTS_0": weight_a},
             "indices": index_a, "material": 0, "mode": 4},
            {"attributes": {"POSITION": position_b, "NORMAL": normal_b, "TANGENT": tangent_b,
                            "TEXCOORD_0": uv_b, "JOINTS_0": joint_b, "WEIGHTS_0": weight_b},
             "indices": index_b, "material": 1, "mode": 4},
        ]}],
        "skins": [
            {"name": "BoundSkin", "skeleton": 0, "joints": [2, 3, 4, 5],
             "inverseBindMatrices": inverse_bind},
            {"name": "DefaultBindSkin", "skeleton": 0, "joints": [2, 3, 4, 5]},
        ],
        "animations": [
            {"name": "Walk", "samplers": [
                {"input": walk_times, "output": walk_translation, "interpolation": "LINEAR"},
                {"input": walk_times, "output": walk_rotation, "interpolation": "STEP"},
                {"input": walk_times, "output": walk_scale, "interpolation": "LINEAR"},
             ], "channels": [
                {"sampler": 0, "target": {"node": 2, "path": "translation"}},
                {"sampler": 1, "target": {"node": 3, "path": "rotation"}},
                {"sampler": 2, "target": {"node": 4, "path": "scale"}},
             ]},
            {"name": "Pose", "samplers": [
                {"input": pose_times, "output": pose_translation, "interpolation": "STEP"},
             ], "channels": [
                {"sampler": 0, "target": {"node": 5, "path": "translation"}},
             ]},
        ],
    }

    conversion = [[0.0, 0.0, -1.0, 0.0], [1.0, 0.0, 0.0, 0.0],
                  [0.0, 1.0, 0.0, 0.0], [0.0, 0.0, 0.0, 1.0]]
    conversion_inverse = matrix_inverse(conversion)

    def converted_matrix(value: list[list[float]]) -> list[list[float]]:
        return clean_matrix(matrix_multiply(matrix_multiply(conversion, value), conversion_inverse))

    converted_rotation_values: list[list[float]] = []
    for source_rotation in walk_rotation_values:
        converted = matrix_quaternion(matrix_multiply(
            matrix_multiply(conversion, quaternion_matrix(source_rotation)), conversion_inverse))
        lexical = (converted[3], converted[2], converted[1], converted[0])
        if (converted_rotation_values
                and sum(a * b for a, b in zip(converted_rotation_values[-1], converted)) < 0.0):
            converted = [-component for component in converted]
        elif (not converted_rotation_values and lexical < (0.0, 0.0, 0.0, 0.0)):
            converted = [-component for component in converted]
        converted_rotation_values.append([clean_number(component) for component in converted])

    converted_joint_globals = {index: converted_matrix(global_matrix(index)) for index in [2, 3, 4, 5]}
    mesh_bind_matrices = [converted_matrix(global_matrix(1)), converted_matrix(global_matrix(6))]
    converted_inverse_binds = [converted_matrix(matrix_inverse(global_matrix(index)))
                               for index in [2, 3, 4, 5]]
    identity = [[1.0 if row == column else 0.0 for column in range(4)] for row in range(4)]
    bind_pose_by_mesh = []
    for mesh_index, mesh_bind in enumerate(mesh_bind_matrices):
        mesh_inverse = matrix_inverse(mesh_bind)
        inverse_bind_set = converted_inverse_binds if mesh_index == 0 else [identity] * 4
        bind_pose_by_mesh.append([
            clean_matrix(matrix_multiply(matrix_multiply(mesh_inverse, converted_joint_globals[joint]), inverse_bind))
            for joint, inverse_bind in zip([2, 3, 4, 5], inverse_bind_set)
        ])

    bones = [
        {"name": "$DurinRoot", "parent": -1, "sourceNode": None,
         "localMatrix": clean_matrix([[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0],
                                      [0.0, 0.0, 1.0, 0.0], [0.0, 0.0, 0.0, 1.0]])},
        {"name": "Hip", "parent": 0, "sourceNode": 2,
         "localMatrix": converted_matrix(global_matrix(2))},
        {"name": "Knee", "parent": 1, "sourceNode": 3,
         "localMatrix": converted_matrix(trs_matrix(nodes[3]))},
        {"name": "Shoulder", "parent": 0, "sourceNode": 4,
         "localMatrix": converted_matrix(global_matrix(4))},
        {"name": "Hand", "parent": 3, "sourceNode": 5,
         "localMatrix": converted_matrix(trs_matrix(nodes[5]))},
    ]
    expected = {
        "contractVersion": 1,
        "sourceToDurin": {
            "positionFormula": "(-source.z, source.x, source.y)",
            "matrixFormula": "C * source * inverse(C)",
            "matrixC": conversion,
            "tolerance": 1.0e-5,
        },
        "skeleton": {
            "canonicalBones": bones,
            "compatibilityEncodingFile": "SkeletonCompatibilityV1.bin",
            "compatibilityHashAlgorithm": "XXH3-128",
            "compatibilityIdentity": EXPECTED_COMPATIBILITY_HASH,
        },
        "mesh": {
            "sourcePositionsByPrimitive": [positions_a, positions_b],
            "durinPositionsByPrimitive": [
                [[-value[2], value[0], value[1]] for value in positions_a],
                [[-value[2], value[0], value[1]] for value in positions_b],
            ],
            "meshNodeBindMatrices": mesh_bind_matrices,
            "paletteBoneIndices": [1, 2, 3, 4],
            "inverseBindMatrices": converted_inverse_binds,
            "defaultInverseBindMatrices": [clean_matrix([[1.0 if row == column else 0.0
                                                           for column in range(4)] for row in range(4)])
                                           for _ in range(4)],
            "bindPoseJointMatricesByMesh": bind_pose_by_mesh,
            "normalizedInfluencesByPrimitive": [
                [[[1, 0.4], [2, 0.3], [3, 0.2], [4, 0.1]],
                 [[1, 0.4], [2, 0.4], [3, 0.2]],
                 [[1, 0.5], [4, 0.3], [2, 0.2]]],
                [[[4, 102.0 / 255.0], [3, 76.0 / 255.0], [2, 51.0 / 255.0], [1, 26.0 / 255.0]],
                 [[1, 1.0]],
                 [[3, 64.0 / 255.0], [4, 64.0 / 255.0], [1, 64.0 / 255.0], [2, 63.0 / 255.0]]],
            ],
        },
        "animations": [
            {"name": "Walk", "duration": 2.0, "tracks": [
                {"bone": 1, "path": "translation", "interpolation": "LINEAR",
                 "times": [0.0, 1.0, 2.0],
                 "values": [[-value[2], value[0], value[1]] for value in walk_translation_values]},
                {"bone": 2, "path": "rotation", "interpolation": "STEP",
                 "times": [0.0, 1.0, 2.0], "sourceValues": walk_rotation_values,
                 "values": converted_rotation_values},
                {"bone": 3, "path": "scale", "interpolation": "LINEAR",
                 "times": [0.0, 1.0, 2.0],
                 "values": [[value[2], value[0], value[1]] for value in walk_scale_values]},
             ]},
            {"name": "Pose", "duration": 0.5, "tracks": [
                {"bone": 4, "path": "translation", "interpolation": "STEP",
                 "times": [0.0, 0.5],
                 "values": [[-value[2], value[0], value[1]] for value in pose_translation_values]},
             ]},
        ],
        "outputGraph": {
            "skeletons": ["skeleton:skin/0", "skeleton:skin/1"],
            "skeletalMeshes": ["skeletal-mesh:node/1/mesh/0", "skeletal-mesh:node/6/mesh/1"],
            "animationClips": [
                "animation-clip:animation/0/skin/0", "animation-clip:animation/0/skin/1",
                "animation-clip:animation/1/skin/0", "animation-clip:animation/1/skin/1",
            ],
        },
    }
    return document, bytes(builder.bytes), builder.accessor_offsets, expected


def encode_compatibility_stream(expected: dict) -> bytes:
    output = bytearray(b"DSKC")
    output.extend(struct.pack("<II", 1, len(expected["skeleton"]["canonicalBones"])))
    for bone in expected["skeleton"]["canonicalBones"]:
        name = bone["name"].encode("utf-8")
        output.extend(struct.pack("<iI", bone["parent"], len(name)))
        output.extend(name)
        for row in bone["localMatrix"]:
            output.extend(struct.pack("<4f", *row))
    return bytes(output)


def with_uri(document: dict, uri: str | None) -> dict:
    result = copy.deepcopy(document)
    if uri is not None:
        result["buffers"][0]["uri"] = uri
    return result


def make_static_projection(document: dict) -> dict:
    result = copy.deepcopy(document)
    result.pop("skins", None)
    result.pop("animations", None)
    for node in result["nodes"]:
        node.pop("skin", None)
    for mesh in result["meshes"]:
        for primitive_index, primitive in enumerate(mesh["primitives"]):
            primitive["attributes"].pop("JOINTS_0", None)
            primitive["attributes"].pop("WEIGHTS_0", None)
            if primitive_index == 1:
                # The full contract keeps normalized-u16 UV coverage for the
                # format-owned decoder. The current Assimp path produces NaN
                # values for that accessor, so the static projection uses the
                # first primitive's equivalent float32 UV accessor.
                primitive["attributes"]["TEXCOORD_0"] = 3
    return result


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=False, allow_nan=False) + "\n",
                    encoding="utf-8", newline="\n")


def write_glb(path: Path, document: dict, payload: bytes) -> None:
    glb_document = with_uri(document, None)
    json_bytes = json.dumps(glb_document, separators=(",", ":"), allow_nan=False).encode("utf-8")
    json_bytes += b" " * ((-len(json_bytes)) % 4)
    binary = payload + b"\0" * ((-len(payload)) % 4)
    total_length = 12 + 8 + len(json_bytes) + 8 + len(binary)
    output = bytearray(struct.pack("<III", 0x46546C67, 2, total_length))
    output.extend(struct.pack("<II", len(json_bytes), 0x4E4F534A))
    output.extend(json_bytes)
    output.extend(struct.pack("<II", len(binary), 0x004E4942))
    output.extend(binary)
    path.write_bytes(output)


def generate_malformed(document: dict, payload: bytes, offsets: dict[str, int]) -> dict[str, str]:
    cases: list[tuple[str, str, callable]] = []

    def json_case(name: str, category: str, mutate) -> None:
        cases.append((name, category, mutate))

    json_case("CyclicHierarchy", "MalformedSource",
              lambda doc, data: doc["nodes"][3].update({"children": [2]}))
    json_case("DisconnectedHierarchy", "MalformedSource",
              lambda doc, data: (doc["nodes"].append({"name": "OrphanJoint"}),
                                 doc["skins"][0]["joints"].append(7)))
    json_case("CountMismatch", "MalformedSource",
              lambda doc, data: doc["accessors"][5].update({"count": 2}))
    json_case("InvalidAnimationTarget", "MalformedSource",
              lambda doc, data: doc["animations"][0]["channels"][0]["target"].update({"node": 99}))
    json_case("UnsupportedCubicSpline", "UnsupportedFeature",
              lambda doc, data: doc["animations"][0]["samplers"][0].update({"interpolation": "CUBICSPLINE"}))
    json_case("UnsupportedSecondaryInfluences", "UnsupportedFeature",
              lambda doc, data: doc["meshes"][0]["primitives"][0]["attributes"].update({"JOINTS_1": 4, "WEIGHTS_1": 5}))
    json_case("UnsupportedRequiredExtension", "UnsupportedFeature",
              lambda doc, data: doc.update({"extensionsUsed": ["EXT_fixture_required"],
                                            "extensionsRequired": ["EXT_fixture_required"]}))
    json_case("ResourceLimit", "ResourceLimitExceeded",
              lambda doc, data: doc["accessors"][0].update({"count": 100000001}))
    json_case("SparseAccessor", "UnsupportedFeature",
              lambda doc, data: doc["accessors"][0].update({"sparse": {
                  "count": 1,
                  "indices": {"bufferView": 6, "componentType": 5123},
                  "values": {"bufferView": 0},
              }}))
    json_case("TruncatedAccessor", "MalformedSource",
              lambda doc, data: doc["bufferViews"][0].update({"byteLength": 35}))
    json_case("AnimatedNonJoint", "UnsupportedFeature",
              lambda doc, data: doc["animations"][1]["channels"][0]["target"].update({"node": 1}))
    json_case("MorphTargets", "UnsupportedFeature",
              lambda doc, data: doc["meshes"][0]["primitives"][0].update({"targets": [{"POSITION": 0}]}))

    def payload_case(name: str, category: str, offset_name: str, replacement: bytes) -> None:
        def mutate(doc, data) -> None:
            offset = offsets[offset_name]
            data[offset:offset + len(replacement)] = replacement
        cases.append((name, category, mutate))

    payload_case("InvalidJointIndex", "MalformedSource", "joints_a", bytes([7]))
    payload_case("ZeroWeights", "MalformedSource", "weights_a", struct.pack("<4f", 0.0, 0.0, 0.0, 0.0))
    payload_case("NaNWeights", "MalformedSource", "weights_a", struct.pack("<f", math.nan))
    payload_case("NonFiniteInverseBind", "MalformedSource", "inverse_binds", struct.pack("<f", math.inf))
    payload_case("UnorderedKeyTimes", "MalformedSource", "walk_times", struct.pack("<3f", 0.0, 2.0, 1.0))
    payload_case("DuplicateKeyTimes", "MalformedSource", "walk_times", struct.pack("<3f", 0.0, 1.0, 1.0))

    manifest: dict[str, str] = {}
    malformed_root = ROOT / "Malformed"
    malformed_root.mkdir(exist_ok=True)
    for name, category, mutate in cases:
        case_document = copy.deepcopy(document)
        case_payload = bytearray(payload)
        mutate(case_document, case_payload)
        case_document = with_uri(
            case_document,
            "data:application/octet-stream;base64," + base64.b64encode(case_payload).decode("ascii"))
        write_json(malformed_root / f"{name}.gltf", case_document)
        manifest[f"Malformed/{name}.gltf"] = category
    return manifest


def main() -> None:
    document, payload, offsets, expected = build_contract()
    data_uri = "data:application/octet-stream;base64," + base64.b64encode(payload).decode("ascii")
    write_json(ROOT / "Contract.gltf", with_uri(document, data_uri))
    write_json(ROOT / "ContractExternal.gltf", with_uri(document, "Contract.bin"))
    (ROOT / "Contract.bin").write_bytes(payload)
    write_glb(ROOT / "Contract.glb", document, payload)
    static_projection = make_static_projection(document)
    write_json(ROOT / "StaticProjection.gltf", with_uri(static_projection, data_uri))
    write_json(ROOT / "StaticProjectionExternal.gltf", with_uri(static_projection, "Contract.bin"))
    write_glb(ROOT / "StaticProjection.glb", static_projection, payload)
    compatibility_stream = encode_compatibility_stream(expected)
    (ROOT / "SkeletonCompatibilityV1.bin").write_bytes(compatibility_stream)
    write_json(ROOT / "ExpectedContract.json", expected)
    manifest = generate_malformed(document, payload, offsets)
    write_json(ROOT / "MalformedManifest.json", {
        "contractVersion": 1,
        "cases": [{"path": path, "expectedCategory": category}
                  for path, category in manifest.items()],
    })


if __name__ == "__main__":
    main()
