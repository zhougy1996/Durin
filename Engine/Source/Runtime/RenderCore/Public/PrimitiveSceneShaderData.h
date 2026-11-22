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

	// GPUScene primitive upload layout.
	struct FPrimitiveSceneShaderData
	{
		static constexpr uint32 DataStrideInFloat4s = 25;
		static constexpr uint32 HeaderOffset = 16;
		static constexpr uint32 CustomDataOffset = 17;
		std::array<FVector4f, DataStrideInFloat4s> Data{};
	};
	static_assert(sizeof(FPrimitiveSceneShaderData) == 25 * 16);

	class FPrimitiveUniformShaderParametersBuilder
	{
	public:
		auto LocalToWorld(const FMatrix& Value) -> FPrimitiveUniformShaderParametersBuilder& { Transform = Value; return *this; }
		auto WorldBounds(const FBox& Value) -> FPrimitiveUniformShaderParametersBuilder& { Bounds = Value; return *this; }
		auto CustomPrimitiveData(const FPrimitiveCustomData& Value) -> FPrimitiveUniformShaderParametersBuilder& { CustomData = Value; return *this; }
		auto InstanceSceneData(uint32 Offset, uint32 Count) -> FPrimitiveUniformShaderParametersBuilder&
		{ InstanceOffset = Offset; InstanceCount = Count; return *this; }
		RENDERCORE_API auto Build() const -> std::optional<FPrimitiveSceneShaderData>;
	private:
		FMatrix Transform{1.0};
		FBox Bounds;
		FPrimitiveCustomData CustomData;
		uint32 InstanceOffset = INDEX_NONE_U32;
		uint32 InstanceCount = 0;
	};
}
