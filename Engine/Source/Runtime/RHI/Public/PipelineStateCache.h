#pragma once

#include "CoreMinimal.h"

#include "RHIPipelineCreation.h"

namespace Durin
{
	class FRHICommandListBase;
	class FRHIPipelineStateCache;
	class FPipelineCompilation;

	// Cache identity may exist before its complete native PSO. Drawing retains
	// shared compilation without owning cancellation authority.
	class RHI_API FPipelineState
	{
	public:
		virtual ~FPipelineState();
		FPipelineState(const FPipelineState&) = delete;
		auto operator=(const FPipelineState&) -> FPipelineState& = delete;
		auto GetState() const -> ERHIPipelineState;
		auto GetStateObservation() const -> FRHIPipelineStateObservation;
		auto GetPipelineLayout() const -> std::shared_ptr<const FPipelineLayoutDesc>;
		auto Wait() const -> ERHIPipelineState;
		auto BoostPriority(EPSOPrecachePriority Priority) const -> void;
		auto GetPriority() const -> EPSOPrecachePriority;
	protected:
		explicit FPipelineState(FPipelineCompilation&& InCompilation, std::shared_ptr<void> InMetadata);
	private:
		std::unique_ptr<FPipelineCompilation> Compilation;
		std::shared_ptr<void> Metadata;
		friend class FRHICommandListBase;
		friend class FRHIPipelineStateCacheBackend;
	};

	class RHI_API FGraphicsPipelineState final : public FPipelineState
	{
	public:
		auto GetRHIPipeline() const -> FGraphicsPipelineStateRHIRef;
	private:
		using FPipelineState::FPipelineState;
		friend class FRHIPipelineStateCache;
	};
	class RHI_API FComputePipelineState final : public FPipelineState
	{
	public:
		auto GetRHIPipeline() const -> FComputePipelineStateRHIRef;
	private:
		using FPipelineState::FPipelineState;
		friend class FRHIPipelineStateCache;
	};
	using FGraphicsPipelineStateRef = std::shared_ptr<FGraphicsPipelineState>;
	using FComputePipelineStateRef = std::shared_ptr<FComputePipelineState>;

	// One device-owned weak cache. The backend owns ready native reuse.
	// Ordinary tasks admit cold requests directly; native creation runs on the
	// device-owned Worker drain. Independent CPU tasks may look up retained
	// identities, but cannot admit cold requests.
	class RHI_API FRHIPipelineStateCache
	{
	public:
		FRHIPipelineStateCache(const FRHICapabilities& Capabilities, FRHIPipelineCompileBackend Backend);
		~FRHIPipelineStateCache();
		auto GetGraphics(const FGraphicsPipelineStateInitializer& Initializer, std::string_view Name)
			-> FGraphicsPipelineStateRef;
		auto GetCompute(const FComputePipelineStateInitializer& Initializer, std::string_view Name)
			-> FComputePipelineStateRef;
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

	RHI_API auto SetGraphicsPipelineState(FRHICommandListBase& CommandList, const FGraphicsPipelineStateInitializer& Initializer, FName Name = {}) -> void;
	RHI_API auto SetComputePipelineState(FRHICommandListBase& CommandList, const FComputePipelineStateInitializer& Initializer, FName Name = {}) -> void;

	namespace PipelineStateCache
	{
		RHI_API auto GetAndOrCreateGraphicsPipelineState(FRHICommandListBase& CommandList, const FGraphicsPipelineStateInitializer& Initializer,
			FName Name = {}) -> FGraphicsPipelineStateRef;
		RHI_API auto GetAndOrCreateComputePipelineState(FRHICommandListBase& CommandList, const FComputePipelineStateInitializer& Initializer,
			FName Name = {}) -> FComputePipelineStateRef;
		// Draw acquisition must succeed; native compilation is joined before replay.
		// Precache may return empty and its shared compilation may fail.
		// Precache and draw use the same identity. Dropping a precache reference
		// does not cancel creation retained by another owner or recorded command.
		RHI_API auto PrecacheGraphicsPipelineState(const FGraphicsPipelineStateInitializer& Initializer,
			FName Name = {}) -> FGraphicsPipelineStateRef;
		RHI_API auto PrecacheComputePipelineState(const FComputePipelineStateInitializer& Initializer,
			FName Name = {}) -> FComputePipelineStateRef;
	}

}
