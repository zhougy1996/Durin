#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneShaderData.h"

namespace Durin
{
	struct FPrimitiveInstanceId
	{
		uint32 Id = 0;
		auto IsValid() const -> bool { return Id != 0; }
		auto operator==(const FPrimitiveInstanceId&) const -> bool = default;
	};

	struct FInstanceSceneDataBuffers
	{
		struct FInstance
		{
			FPrimitiveInstanceId Id;
			FMatrix LocalToPrimitive{1.0};
			FBox LocalBounds;
			FPrimitiveCustomData CustomData;
		};
		// Published via shared_ptr<const ...>; null means one default instance.
		std::vector<FInstance> Instances;
	};

	struct FInstanceSceneShaderData
	{
		static constexpr uint32 DataStrideInFloat4s = 18;
		static constexpr uint32 HeaderOffset = 16;
		static constexpr uint32 PayloadOffset = 17;
		std::array<FVector4f, DataStrideInFloat4s> Data{};
		RENDERCORE_API auto Build(uint32 PrimitiveId, uint32 InstanceRelativeId,
			uint32 InstancePayloadDataOffset, FPrimitiveInstanceId StableId,
			const FMatrix& LocalToWorld, const FBox& WorldBounds) -> bool;
	};
	static_assert(sizeof(FInstanceSceneShaderData) == 18 * 16);
}
