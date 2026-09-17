#pragma once

#include "RHIAPI.h"
#include "RHIDefinitions.h"

namespace Durin
{
	enum class ERHICreationFailureSource : uint8
	{
		None, NativeBackend, MetadataBudget, GraphicsPipelineCache,
		ComputePipelineCache, PipelineLayoutCache, DescriptorLayoutCache,
		RenderPassCache, RequestNotAdmitted, ObserverFailed, BackendReturnedNull,
	};

	struct FRHICreationError
	{
		ERHIResourceCreationFailure Failure = ERHIResourceCreationFailure::None;
		ERHICreationFailureSource Source = ERHICreationFailureSource::None;
		// Backend-neutral storage of the native status; no backend headers leak into RHI.
		std::optional<int32> NativeCode;
		// In-process diagnostic identity, not a persistent cache key.
		RHI_API auto GetSemanticFingerprint() const -> size_t;
		auto HasError() const -> bool { return Failure != ERHIResourceCreationFailure::None; }
	};

	RHI_API auto FormatRHICreationError(const FRHICreationError& Error) -> std::string;
}
