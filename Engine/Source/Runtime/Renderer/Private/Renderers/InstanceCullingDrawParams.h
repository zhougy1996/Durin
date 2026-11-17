#pragma once

#include "CoreMinimal.h"
#include "GPUScene.h"
#include "RendererAPI.h"
#include "RDG/RDGParameters.h"

namespace Durin
{
	struct FInstanceCullingDrawParams
	{
		FGPUSceneResourceParameters Scene;
		std::optional<FRDGBufferParameter> InstanceIdsBuffer;
		std::optional<FRDGBufferParameter> DynamicInstanceIdsBuffer;
		std::optional<FRDGBufferParameter> InstanceIdOffsetBuffer;
		std::optional<FRDGBufferParameter> DrawIndirectArgsBuffer;
		uint32 InstanceDataByteOffset = 0;
		uint64 IndirectArgsByteOffset = 0;
		RENDERER_API static auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*;
	};

}
