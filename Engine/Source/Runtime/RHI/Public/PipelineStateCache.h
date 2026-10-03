#pragma once

#include "CoreMinimal.h"

#include "RHIPipelineCreation.h"

namespace Durin
{
	class FRHICommandListBase;
	class FRHIPipelineStateCache;

	// Cache identity may exist before its complete native PSO. Drawing never owns
	// cancellation authority over the shared creation observation.
	class RHI_API FPipelineState
	{
	public:
		virtual ~FPipelineState() = default;
		FPipelineState(const FPipelineState&) = delete;
		auto operator=(const FPipelineState&) -> FPipelineState& = delete;
		auto GetState() const -> ERHIPipelineRequestState { return Request.GetState(); }
		auto IsComplete() const -> bool { return Request.GetCompletion().IsReady(); }
		auto GetPipelineLayout() const -> std::shared_ptr<const FPipelineLayoutDesc> { return Request.GetPipelineLayout(); }
		auto GetCreationError() const -> FRHICreationError { return Request.GetResult().Error; }
		auto CanWait() const -> bool { return Request.CanWait(); }
		auto Wait() const -> bool { return Request.Wait(); }
	protected:
		explicit FPipelineState(FRHIPipelineCreationRequest InRequest, std::shared_ptr<void> InMetadata)
			: Request(std::move(InRequest)), Metadata(std::move(InMetadata)) {}
		FRHIPipelineCreationRequest Request;
	private:
		std::shared_ptr<void> Metadata;
		friend class FRHICommandListBase;
	};

	class RHI_API FGraphicsPipelineState final : public FPipelineState
	{
	public:
		auto GetRHIPipeline() const -> FGraphicsPipelineStateRHIRef { return Request.GetResult().Graphics; }
	private:
		using FPipelineState::FPipelineState;
		friend class FRHIPipelineStateCache;
	};
	class RHI_API FComputePipelineState final : public FPipelineState
	{
	public:
		auto GetRHIPipeline() const -> FComputePipelineStateRHIRef { return Request.GetResult().Compute; }
	private:
		using FPipelineState::FPipelineState;
		friend class FRHIPipelineStateCache;
	};
	using FGraphicsPipelineStateRef = std::shared_ptr<FGraphicsPipelineState>;
	using FComputePipelineStateRef = std::shared_ptr<FComputePipelineState>;

	// One device-owned, bounded weak cache. The backend owns ready native reuse.
	// Cold requests from foreign task scopes use an admission thread; that thread
	// only constructs observations/queues Core work and never creates native PSOs.
	class RHI_API FRHIPipelineStateCache
	{
	public:
		FRHIPipelineStateCache(const FRHICapabilities& Capabilities, FRHIPipelineCompileBackend Backend);
		~FRHIPipelineStateCache();
		auto GetGraphics(const FGraphicsPipelineStateInitializer& Initializer, std::string_view Name)
			-> std::expected<FGraphicsPipelineStateRef, ERHIPipelineRequestRejection>;
		auto GetCompute(const FComputePipelineStateInitializer& Initializer, std::string_view Name)
			-> std::expected<FComputePipelineStateRef, ERHIPipelineRequestRejection>;
		// Reject new requests and join compilation, preserving already Ready native PSOs.
		auto StopAndWait() -> void;
		// Stop compilation first, then invalidate native results in surviving handles.
		auto ReleaseResources() -> void;
		auto IsClosed() const -> bool;
		auto GetStatistics() const -> FRHIPipelineCreationStatistics;
	private:
		friend class FRHIPipelineStateCacheBackend;
		struct FState;
		std::unique_ptr<FState> State;
	};

	namespace PipelineStateCache
	{
		RHI_API auto GetAndOrCreateGraphicsPipelineState(const FGraphicsPipelineStateInitializer& Initializer,
			FName Name = {}) -> std::expected<FGraphicsPipelineStateRef, ERHIPipelineRequestRejection>;
		RHI_API auto GetAndOrCreateComputePipelineState(const FComputePipelineStateInitializer& Initializer,
			FName Name = {}) -> std::expected<FComputePipelineStateRef, ERHIPipelineRequestRejection>;
		// Precache and draw use the same identity. Dropping a precache reference
		// does not cancel creation retained by another owner or recorded command.
		RHI_API auto PrecacheGraphicsPipelineState(const FGraphicsPipelineStateInitializer& Initializer,
			FName Name = {}) -> std::expected<FGraphicsPipelineStateRef, ERHIPipelineRequestRejection>;
		RHI_API auto PrecacheComputePipelineState(const FComputePipelineStateInitializer& Initializer,
			FName Name = {}) -> std::expected<FComputePipelineStateRef, ERHIPipelineRequestRejection>;
	}

	// Recording does not wait for native creation. Core-free startup uses the
	// synchronous native factory; request rejection is an explicit contract error.
	RHI_API auto SetGraphicsPipelineState(FRHICommandListBase& Commands,
		const FGraphicsPipelineStateInitializer& Initializer, FName Name = {}) -> void;
	RHI_API auto SetComputePipelineState(FRHICommandListBase& Commands,
		const FComputePipelineStateInitializer& Initializer, FName Name = {}) -> void;
}
