#pragma once

#include "RHIPipelineCreation.h"

namespace Durin
{
	template<typename T> struct TRHIPipelineBatchItem
	{
		T Initializer;
		std::string DebugName;
	};
	using FRHIGraphicsPipelineBatchItem = TRHIPipelineBatchItem<FGraphicsPipelineStateInitializer>;
	using FRHIComputePipelineBatchItem = TRHIPipelineBatchItem<FComputePipelineStateInitializer>;
	struct FRHIPipelineCreationBatch
	{
		ERHIPipelineRequestRejection Rejection = ERHIPipelineRequestRejection::None;
		std::vector<FRHIPipelineCreationRequest> Items;
	};

	// Owns a bounded device queue and one counted Core drain task. Native callbacks
	// must never schedule replay or wait for a consuming command batch.
	class RHI_API FPipelineCompileQueue
	{
	public:
		using FBackend = FRHIPipelineCompileBackend;

		FPipelineCompileQueue(const FRHICapabilities& Capabilities, FBackend Backend);
		~FPipelineCompileQueue();
		auto RequestGraphics(const FGraphicsPipelineStateInitializer& Initializer,
			std::string_view DebugName) -> FRHIPipelineCreationRequest;
		auto RequestCompute(const FComputePipelineStateInitializer& Initializer,
			std::string_view DebugName) -> FRHIPipelineCreationRequest;
		auto RequestGraphicsBatch(std::span<const FRHIGraphicsPipelineBatchItem> Items) -> FRHIPipelineCreationBatch;
		auto RequestComputeBatch(std::span<const FRHIComputePipelineBatchItem> Items) -> FRHIPipelineCreationBatch;
		auto CloseAndJoin(bool RetireResults = true) -> void;
		auto IsClosed() const -> bool;
		auto GetStatistics() const -> FRHIPipelineCreationStatistics;
	private:
		// Cache keys were validated against the cache's capability snapshot.
		auto RequestValidated(const FGraphicsPipelineStateInitializer& Initializer,
			std::string_view DebugName, const FGraphicsPipelineStateKey& Key) -> FRHIPipelineCreationRequest;
		auto RequestValidated(const FComputePipelineStateInitializer& Initializer,
			std::string_view DebugName, const FComputePipelineStateKey& Key) -> FRHIPipelineCreationRequest;
		auto ReserveCacheMetadata(uint64 Bytes) -> std::shared_ptr<void>;
		friend class FRHIPipelineStateCache;
		struct FState;
		std::unique_ptr<FState> State;
	};
}
