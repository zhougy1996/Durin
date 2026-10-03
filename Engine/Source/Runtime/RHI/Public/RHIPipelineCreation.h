#pragma once

#include "CoreMinimal.h"

#include "RHIResources.h"
#include "Threading/TaskComposition.h"

namespace Durin
{
	class FRHIPipelineStateCache;
	// Budget control blocks are owned by RHI, so backend unload does not invalidate
	// metadata-only request/layout handles retained after device shutdown.
	class RHI_API FRHIPipelineMetadataBudget
	{
	public:
		FRHIPipelineMetadataBudget();
		auto Reserve(uint64 Bytes) -> std::shared_ptr<void>;
		auto GetUsedBytes() const -> uint64 { return Used->load(); }
	private:
		std::shared_ptr<std::atomic<uint64>> Used;
	};
	RHI_API auto IsPipelineCreationPayloadBounded(const FGraphicsPipelineStateInitializer& Initializer,
		std::string_view DebugName) -> bool;
	RHI_API auto IsPipelineCreationPayloadBounded(const FComputePipelineStateInitializer& Initializer,
		std::string_view DebugName) -> bool;
	struct FRHIGraphicsPipelineCreationInputs
	{
		FRHIGraphicsPipelineCreationInputs(const FGraphicsPipelineStateInitializer& InInitializer,
			std::string_view InDebugName)
			: Initializer(InInitializer), DebugName(InDebugName),
			VertexShader(InInitializer.BoundShaders.VertexShader),
			FragmentShader(InInitializer.BoundShaders.FragmentShader),
			VertexDeclaration(InInitializer.VertexDeclaration) {}
		const FGraphicsPipelineStateInitializer Initializer;
		const std::string DebugName;
		const FShaderRHIRef VertexShader, FragmentShader;
		const FVertexDeclarationRHIRef VertexDeclaration;
	};

	struct FRHIComputePipelineCreationInputs
	{
		FRHIComputePipelineCreationInputs(const FComputePipelineStateInitializer& InInitializer,
			std::string_view InDebugName)
			: Initializer(InInitializer), DebugName(InDebugName), Shader(InInitializer.ComputeShader) {}
		const FComputePipelineStateInitializer Initializer;
		const std::string DebugName;
		const FShaderRHIRef Shader;
	};

	enum class ERHIPipelineRequestState : uint8 { Pending, Ready, Failed, Canceled };
	enum class ERHIPipelineRequestRejection : uint8
	{
		None, Unsupported, Closed, InvalidDescription, CapacityExceeded
	};

	struct FRHIPipelineCreationResult
	{
		ERHIPipelineRequestState State = ERHIPipelineRequestState::Pending;
		FGraphicsPipelineStateRHIRef Graphics;
		FComputePipelineStateRHIRef Compute;
		FRHICreationError Error;
	};

	// One observer of shared device work. Copying a handle copies that observer;
	// a separate request call creates an independently cancelable observer.
	class FRHIPipelineCreationRequest
	{
	public:
		FRHIPipelineCreationRequest() = default;
		auto operator==(const FRHIPipelineCreationRequest&) const -> bool = default;
		RHI_API static auto Rejected(ERHIPipelineRequestRejection Reason) -> FRHIPipelineCreationRequest;
		auto IsAccepted() const -> bool { return State != nullptr; }
		auto GetRejection() const -> ERHIPipelineRequestRejection { return Rejection; }
		RHI_API auto GetState() const -> ERHIPipelineRequestState;
		RHI_API auto GetResult() const -> FRHIPipelineCreationResult;
		RHI_API auto GetCompletion() const -> Tasks::FTaskCompletion;
		RHI_API auto IsCompute() const -> bool;
		// Immutable recording metadata is available before the native pipeline.
		RHI_API auto GetPipelineLayout() const -> std::shared_ptr<const FPipelineLayoutDesc>;
		RHI_API auto Cancel() const -> bool;
		// Pending waits reject replay and Core workers; asynchronous callers use completion edges.
		RHI_API auto Wait() const -> bool;
		RHI_API auto CanWait() const -> bool;
	private:
		struct FState;
		std::shared_ptr<FState> State;
		ERHIPipelineRequestRejection Rejection = ERHIPipelineRequestRejection::Unsupported;
		friend class FPipelineCompileQueue;
	};

	struct FRHIPipelineCreationStatistics
	{
		uint32 UnfinishedRequests = 0;
		uint32 ActiveObservers = 0;
		uint64 UnfinishedDescriptionBytes = 0;
		uint64 RejectedRequests = 0;
		uint64 SharedPendingHits = 0;
		uint64 NativeRequests = 0;
	};

	struct FRHIPipelineCompileBackend
	{
		std::function<FGraphicsPipelineStateRHIRef(const FGraphicsPipelineStateKey&)> FindGraphics;
		std::function<FComputePipelineStateRHIRef(const FComputePipelineStateKey&)> FindCompute;
		std::function<FGraphicsPipelineStateRHIRef(const FRHIGraphicsPipelineCreationInputs&,
			const FGraphicsPipelineStateKey&)> CreateGraphics;
		std::function<FComputePipelineStateRHIRef(const FRHIComputePipelineCreationInputs&,
			const FComputePipelineStateKey&)> CreateCompute;
		std::function<void(std::exception_ptr)> PublishTerminalFailure;
		std::function<std::shared_ptr<void>(uint64)> ReserveMetadata;
	};
}
