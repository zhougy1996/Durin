#pragma once

#include "CoreMinimal.h"
#include "RHIResources.h"

namespace Durin
{
	// Borrowed pass bindings. The scene or submission context owns the buffers.
	struct FGPUSceneShaderParameters
	{
		FRHIStorageBufferRange GPUScenePrimitiveSceneData;
		FRHIStorageBufferRange GPUSceneInstanceSceneData;
		FRHIStorageBufferRange GPUSceneInstancePayloadData;
		FRHIStorageBufferRange GPUSceneInstanceIds;
	};
}
