#pragma once

#include "CoreMinimal.h"

#include "Asset/Cook.h"
#include "EngineAPI.h"
#include "Materials/MaterialCompilationConfiguration.h"
#include "Materials/MaterialProgramCompiler.h"

namespace Durin
{
	inline constexpr uint32 MaterialCookedProgramPayloadSchemaVersion = 10;
	inline constexpr uint64 MaterialCookedProgramMaxPayloadBytes =
		8ull * 1024ull * 1024ull;
	inline constexpr uint32 MaterialCookedProgramPayloadAlignment = 16;
	inline constexpr uint32 MaterialCookedProgramMaxConfigurations =
		MaterialSupportedConfigurationCount;
	inline const FGuid MaterialCookedProgramPayloadId{
		0x4d415450, 0x7c4d4a68, 0xa141390e, 0x71b2c418};

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
