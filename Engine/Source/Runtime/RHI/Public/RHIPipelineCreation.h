#pragma once

#include "CoreMinimal.h"

#include "RHIResources.h"

namespace Durin
{
	class FRHIPipelineStateCache;
	// Monotonic shared-work priority; an executing native compile is not preempted.
	enum class EPSOPrecachePriority : uint8 { Low, Medium, High, Highest, Num, NotSet = Num };
	// Accounting control blocks are owned by RHI, so backend unload does not invalidate
	// metadata-only request/layout handles retained after device shutdown.
	// Logical CPU metadata only; accounting never rejects resource creation.
	class RHI_API FRHIPipelineMetadataTracker
	{
	public:
		FRHIPipelineMetadataTracker();
		auto Reserve(uint64 Bytes) -> std::shared_ptr<void>;
		auto GetUsedBytes() const -> uint64 { return Used->load(); }
	private:
		std::shared_ptr<std::atomic<uint64>> Used;
	};
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

	// Failed means compilation returned no usable pipeline.
	enum class ERHIPipelineState : uint8 { Pending, Ready, Failed, Canceled };

	// Read-only shared outcome metadata. Retains no native pipeline or completion task.
	class RHI_API FRHIPipelineStateObservation
	{
	public:
		FRHIPipelineStateObservation() = default;
		explicit operator bool() const { return Outcome != nullptr; }
		auto operator==(const FRHIPipelineStateObservation&) const -> bool = default;
		auto GetState() const -> ERHIPipelineState;
	private:
		explicit FRHIPipelineStateObservation(std::shared_ptr<const std::atomic<ERHIPipelineState>> InOutcome)
			: Outcome(std::move(InOutcome)) {}
		std::shared_ptr<const std::atomic<ERHIPipelineState>> Outcome;
		friend class FPipelineCompilation;
	};


	struct FRHIPipelineCreationStatistics
	{
		uint32 UnfinishedRequests = 0;
		uint64 UnfinishedDescriptionBytes = 0;
		uint64 SharedPendingHits = 0;
		uint64 NativeRequests = 0;
	};

	struct FRHIPipelineCompileBackend
	{
		std::function<FGraphicsPipelineStateRHIRef(const FGraphicsPipelineStateKey&)> FindGraphics;
		std::function<FComputePipelineStateRHIRef(const FComputePipelineStateKey&)> FindCompute;
		std::function<FGraphicsPipelineStateRHIRef(const FRHIGraphicsPipelineCreationInputs&,
			const FGraphicsPipelineStateKey&)> CreateGraphics;
		std::function<FComputePipelineStateRHIRef(const FRHIComputePipelineCreationInputs&, const FComputePipelineStateKey&)> CreateCompute;
		std::function<std::shared_ptr<void>(uint64)> ReserveMetadata;
	};
}
