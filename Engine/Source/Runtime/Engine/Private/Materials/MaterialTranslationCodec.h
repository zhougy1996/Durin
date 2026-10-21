#pragma once
#include "CoreMinimal.h"
#include "Materials/MaterialPreparedProgram.h"

namespace Durin::MaterialBuildPrivate
{
	inline constexpr uint32 TranslationSchema = 4;
	inline constexpr uint64 TranslationInputBytes = 2ull * 1024 * 1024;
	inline constexpr uint64 TranslationOutputBytes = 8ull * 1024 * 1024;
	ENGINE_API auto EstimateTranslationCaptureBytes(const MIR::FCompilerInput& Input) -> uint64;
	ENGINE_API auto EncodeTranslationInput(const MIR::FCompilerInput& Input) -> std::optional<FSharedByteBuffer>;
	ENGINE_API auto DecodeTranslationInput(FByteView Bytes) -> std::optional<MIR::FCompilerInput>;
	ENGINE_API auto EncodeTranslationProduct(const FMaterialTranslatedProgram& Product, FXxHash128 InputIdentity) -> std::optional<FSharedByteBuffer>;
	ENGINE_API auto DecodeTranslationProduct(FByteView Bytes, FXxHash128 InputIdentity,
		const MIR::FCompilerInput& Consumer) -> std::optional<FMaterialTranslatedProgram>;
}
