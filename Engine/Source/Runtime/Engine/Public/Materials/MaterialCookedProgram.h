#pragma once

#include "Asset/Cook.h"
#include "EngineAPI.h"
#include "Materials/MaterialProgramCompiler.h"

namespace Durin
{
	inline constexpr uint32 MaterialCookedProgramPayloadSchemaVersion = 1;
	inline constexpr uint64 MaterialCookedProgramMaxPayloadBytes =
		8ull * 1024ull * 1024ull;
	inline constexpr uint32 MaterialCookedProgramPayloadAlignment = 16;
	inline const FGuid MaterialCookedProgramPayloadId{
		0x4d415450, 0x7c4d4a68, 0xa141390e, 0x71b2c418};

	// Encodes only target runtime program data; authored IR and generated source stay editor-owned.
	ENGINE_API auto EncodeMaterialCookedProgram(
		const FMaterialCompilerResult& Program,
		const FMaterialStaticProperties& StaticProperties,
		Asset::ECookTargetPlatform TargetPlatform,
		Asset::ECookTargetProfile TargetProfile,
		std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool;

	// Validates a complete bounded target payload before publishing an immutable program.
	ENGINE_API auto DecodeMaterialCookedProgram(
		std::span<const std::byte> Bytes,
		Asset::ECookTargetPlatform ExpectedPlatform,
		Asset::ECookTargetProfile ExpectedProfile,
		FMaterialStaticProperties& OutStaticProperties,
		std::shared_ptr<const FMaterialCompilerResult>& OutProgram,
		std::string& OutError) -> bool;
}
