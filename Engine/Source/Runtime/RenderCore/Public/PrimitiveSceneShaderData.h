#pragma once

#include "CoreMinimal.h"
#include "RenderCoreAPI.h"
#include "Rendering/PrimitiveCustomData.h"

namespace Durin
{
	struct FPersistentPrimitiveIndex
	{
		uint32 Index = INDEX_NONE_U32;
		auto IsValid() const -> bool { return Index != INDEX_NONE_U32; }
		auto operator==(const FPersistentPrimitiveIndex&) const -> bool = default;
	};

	struct FPrimitiveUniformShaderParameters
	{
		FMatrix LocalToWorld{1.0};
		FBox WorldBounds;
		FPrimitiveCustomData CustomPrimitiveData;
		uint32 InstanceSceneDataOffset = INDEX_NONE_U32;
		uint32 NumInstanceSceneDataEntries = 0;
		uint32 Flags = 0;
	};

	class FPrimitiveUniformShaderParametersBuilder
	{
	public:
		auto LocalToWorld(const FMatrix& Value) -> FPrimitiveUniformShaderParametersBuilder& { Parameters.LocalToWorld = Value; return *this; }
		auto WorldBounds(const FBox& Value) -> FPrimitiveUniformShaderParametersBuilder& { Parameters.WorldBounds = Value; return *this; }
		auto CustomPrimitiveData(const FPrimitiveCustomData& Value) -> FPrimitiveUniformShaderParametersBuilder& { Parameters.CustomPrimitiveData = Value; return *this; }
		auto InstanceSceneData(uint32 Offset, uint32 Count) -> FPrimitiveUniformShaderParametersBuilder&
		{ Parameters.InstanceSceneDataOffset = Offset; Parameters.NumInstanceSceneDataEntries = Count; return *this; }
		RENDERCORE_API auto Build() const -> std::optional<FPrimitiveUniformShaderParameters>;
	private:
		FPrimitiveUniformShaderParameters Parameters;
	};

	// Explicit float4 ABI, not a memcpy of the semantic parameter struct.
	struct FPrimitiveSceneShaderData
	{
		static constexpr uint32 DataStrideInFloat4s = 25;
		static constexpr uint32 HeaderOffset = 16;
		static constexpr uint32 CustomDataOffset = 17;
		std::array<FVector4f, DataStrideInFloat4s> Data{};
		RENDERCORE_API explicit FPrimitiveSceneShaderData(const FPrimitiveUniformShaderParameters& Parameters);
		FPrimitiveSceneShaderData() = default;
	};
	static_assert(sizeof(FPrimitiveSceneShaderData) == 25 * 16);
}
