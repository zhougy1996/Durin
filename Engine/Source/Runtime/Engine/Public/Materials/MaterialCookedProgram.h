#pragma once

#include "Asset/Cook.h"
#include "EngineAPI.h"
#include "Materials/MaterialProgramCompiler.h"

namespace Durin
{
	inline constexpr uint32 MaterialCookedProgramPayloadSchemaVersion = 9;
	inline constexpr uint64 MaterialCookedProgramMaxPayloadBytes =
		8ull * 1024ull * 1024ull;
	inline constexpr uint32 MaterialCookedProgramPayloadAlignment = 16;
	inline constexpr uint32 MaterialCookedProgramMaxConfigurations = 6;
	inline const FGuid MaterialCookedProgramPayloadId{
		0x4d415450, 0x7c4d4a68, 0xa141390e, 0x71b2c418};

	// Encodes only target runtime program data; authored IR and generated source stay editor-owned.
	[[nodiscard]] ENGINE_API auto EncodeMaterialCookedProgram(
		const FMaterialCompilerResult& Program,
		const FMaterialStaticProperties& StaticProperties,
		ECookTargetPlatform TargetPlatform,
		ECookTargetProfile TargetProfile,
		FByteBuffer& OutBytes) -> FMaterialOperationResult;

	// Validates a complete bounded target payload before publishing an immutable program.
	[[nodiscard]] ENGINE_API auto DecodeMaterialCookedProgram(
		FByteView Bytes,
		ECookTargetPlatform ExpectedPlatform,
		ECookTargetProfile ExpectedProfile,
		FMaterialStaticProperties& OutStaticProperties,
		std::shared_ptr<const FMaterialCompilerResult>& OutProgram) -> FMaterialOperationResult;

	[[nodiscard]] ENGINE_API auto EncodeMaterialCookedProgramFamily(
		std::span<const FMaterialCompilerResult* const> Programs,
		const FMaterialStaticProperties& StaticProperties,
		ECookTargetPlatform TargetPlatform,
		ECookTargetProfile TargetProfile,
		FByteBuffer& OutBytes) -> FMaterialOperationResult;

	[[nodiscard]] ENGINE_API auto DecodeMaterialCookedProgramFamily(
		FByteView Bytes,
		ECookTargetPlatform ExpectedPlatform,
		ECookTargetProfile ExpectedProfile,
		EMaterialQualityLevel Quality,
		ERHIFeatureLevel FeatureLevel,
		std::span<const FMaterialCompilerEnvironment::FStaticBoolValue> StaticBools,
		FMaterialStaticProperties& OutStaticProperties,
		std::shared_ptr<const FMaterialCompilerResult>& OutProgram) -> FMaterialOperationResult;
}
