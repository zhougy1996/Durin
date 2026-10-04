#!/usr/bin/env python3
"""Probe direct Metal and SPIR-V-to-MSL routes for representative shaders.

This checks code generation only. A passing result does not qualify Metal
compilation, binding layouts, GPU execution, or image/readback output.
"""

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SHADERS = ROOT / "Engine/Shaders/Slang"
SLANGC = ROOT / "Engine/External/Packages/slang/bin/slangc"

# Name, entry point, and preprocessor definitions used by production variants.
CASES = (
    ("ImGui", "VertexMain", ()),
    ("ImGui", "FragmentMain", ()),
    ("TexturePreview", "FragmentMain", ()),
    ("PostProcess", "FXAAFragmentMain", ()),
    ("StaticMeshBasePass", "VertexMain", ("DURIN_SPLINE_MESH=0", "DURIN_GPU_CULLING=0")),
    ("StaticMeshBasePass", "GeometryFragmentMain", ("DURIN_SPLINE_MESH=0", "DURIN_GPU_CULLING=0")),
    ("StaticMeshBasePass", "SplineVertexMain", ("DURIN_SPLINE_MESH=1", "DURIN_GPU_CULLING=0")),
    ("StaticMeshBasePass", "GPUCullingVertexMain", ("DURIN_SPLINE_MESH=0", "DURIN_GPU_CULLING=1")),
    ("GBufferGPUCulling", "CullMain", ()),
    ("VolumetricCloud", "CloudComputeMain", ()),
    ("DeferredDirectionalLighting", "ProductionFragmentMain", ()),
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--route", choices=("direct", "spirv-cross"), default="direct")
    parser.add_argument("--require-metal-compiler", action="store_true",
                        help="also require Apple's offline Metal compiler to accept each result")
    args = parser.parse_args()

    if not SLANGC.is_file():
        print(f"Missing pinned Slang compiler: {SLANGC}", file=sys.stderr)
        return 2

    version = subprocess.run([str(SLANGC), "-version"], capture_output=True, text=True)
    if version.returncode:
        print(version.stderr, file=sys.stderr)
        return 2
    print(f"Slang {(version.stdout or version.stderr).strip()}")

    spirv_cross = None
    if args.route == "spirv-cross":
        sdk = os.environ.get("VULKAN_SDK", "")
        sdk_tool = pathlib.Path(sdk) / "bin/spirv-cross" if sdk else None
        spirv_cross = str(sdk_tool) if sdk_tool and sdk_tool.is_file() else shutil.which("spirv-cross")
        if not spirv_cross:
            print("Missing Vulkan SDK spirv-cross compiler", file=sys.stderr)
            return 2
        tool_version = subprocess.run([spirv_cross, "--version"], capture_output=True, text=True)
        print(f"SPIRV-Cross {(tool_version.stdout or tool_version.stderr).splitlines()[0]}")

    failed = 0
    with tempfile.TemporaryDirectory(prefix="durin-metal-routes-") as directory:
        output_dir = pathlib.Path(directory)
        for index, (shader, entry, definitions) in enumerate(CASES):
            output = output_dir / f"{index}.metal"
            intermediate = output if args.route == "direct" else output_dir / f"{index}.spv"
            command = [str(SLANGC), str(SHADERS / f"{shader}.slang"),
                       "-target", "metal" if args.route == "direct" else "spirv",
                       "-entry", entry, "-I", str(SHADERS), "-o", str(intermediate)]
            if args.route == "spirv-cross":
                command.extend(("-profile", "spirv_1_5"))
            command.extend(f"-D{definition}" for definition in definitions)
            result = subprocess.run(command, capture_output=True, text=True)
            label = f"{shader}:{entry} [{', '.join(definitions) or 'default'}]"
            if result.returncode:
                print(f"FAIL Slang {label}\n{result.stderr.strip()}")
                failed += 1
                continue

            if args.route == "spirv-cross":
                crossed = subprocess.run([spirv_cross, str(intermediate), "--msl",
                                          "--msl-version", "20000",
                                          "--output", str(output)], capture_output=True, text=True)
                if crossed.returncode:
                    print(f"FAIL SPIRV-Cross {label}\n{crossed.stderr.strip()}")
                    failed += 1
                    continue
            source = output.read_text()
            expected_entry = entry if args.route == "direct" else "main0"
            if not re.search(rf"\b{re.escape(expected_entry)}\s*\(", source):
                print(f"FAIL missing entry point {label}")
                failed += 1
                continue
            print(f"PASS {args.route} {label}")

            if args.require_metal_compiler:
                compiled = subprocess.run(["xcrun", "metal", "-c", str(output),
                                           "-o", str(output_dir / f"{index}.air")],
                                          capture_output=True, text=True)
                if compiled.returncode:
                    print(f"FAIL Metal {label}\n{compiled.stderr.strip()}")
                    failed += 1
                else:
                    print(f"PASS Metal {label}")

    print(f"{failed} case failures")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
