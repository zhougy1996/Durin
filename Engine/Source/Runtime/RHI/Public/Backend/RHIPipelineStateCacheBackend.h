#pragma once

#include "CoreMinimal.h"

#include "PipelineStateCache.h"

// Backend implementation seam. Do not include from renderer or feature code.
namespace Durin
{
	class FRHIPipelineStateCacheBackend final
	{
	public:
		// Key must describe Initializer and be validated against this device's
		// capabilities. Cache lifecycle, payload, and task admission still apply.
		RHI_API static auto GetGraphicsValidated(FRHIPipelineStateCache& Cache,
			const FGraphicsPipelineStateInitializer& Initializer, std::string_view Name,
			FGraphicsPipelineStateKey Key)
			-> std::expected<FGraphicsPipelineStateRef, ERHIPipelineRequestRejection>;
		RHI_API static auto GetComputeValidated(FRHIPipelineStateCache& Cache,
			const FComputePipelineStateInitializer& Initializer, std::string_view Name,
			FComputePipelineStateKey Key)
			-> std::expected<FComputePipelineStateRef, ERHIPipelineRequestRejection>;
	};
}
