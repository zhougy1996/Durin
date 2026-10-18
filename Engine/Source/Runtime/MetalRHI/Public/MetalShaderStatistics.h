#pragma once

#include "CoreMinimal.h"
#include "MetalRHIAPI.h"

namespace Durin
{
	// Process-wide observations; subtraction measures one quiescent preparation or
	// frame. Counts include direct RHI creation, not only queued PSO requests.
	struct FMetalShaderCreationStatistics
	{
		uint64 SourceCompileAttempts = 0;
		uint64 BinaryLoadAttempts = 0;
		uint64 FunctionsCreated = 0;
		uint64 GraphicsPipelinesCreated = 0;
		uint64 ComputePipelinesCreated = 0;
		uint64 SourceLibraryMicroseconds = 0;
		uint64 BinaryLibraryMicroseconds = 0;
	};
	METALRHI_API auto GetMetalShaderCreationStatistics() -> FMetalShaderCreationStatistics;
}
