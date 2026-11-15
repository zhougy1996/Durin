#pragma once

#include "CoreMinimal.h"
#include "Renderers/MeshDrawCommandSubmission.h"
#include "RendererAPI.h"
#include "Renderers/GPUSceneRendering.h"
#include "Renderers/MeshPassProcessor.h"
#include "Renderers/MeshPipelineDescription.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/ViewPreparationMath.h"

namespace Durin
{
	class FInstanceCullingManager;
	class FRendererResourceCoordinator;
	struct FInstanceCullingTelemetry;
	struct FPreparedStaticMeshDraw;
	struct FMeshDrawCommandStateCache;
	struct FSceneView;

	struct alignas(16) FInstanceCullingUniform final
	{
		std::array<FVector4f, 6> Planes{};
		FVector4f WorldOriginAndRelativeTolerance{};
		FVector4f ViewOriginLow{};
		uint32 CandidateCount = 0;
		uint32 PlaneCount = 0;
		uint32 Padding0 = 0;
		uint32 Padding = 0;
	};
	static_assert(sizeof(FInstanceCullingUniform) == 144);


	// Pass-local references into counted scene data; no copied attributes.
	struct FInstanceCullingCandidate
	{
		uint32 InstanceId = 0;
		uint32 ArgumentIndex = 0;
	};
	static_assert(sizeof(FInstanceCullingCandidate) == 8);

	struct FInstanceCullingAdmissionDiagnostics
	{
		size_t BelowThresholdCandidates = 0;
		size_t IncompatibleCandidates = 0;
		size_t PreparationUnavailableCandidates = 0;
	};

	struct FInstanceCullingDrawParams
	{
		std::optional<FRDGBufferParameter> InstanceIdOffsetBuffer;
		std::optional<FRDGBufferParameter> DrawIndirectArgsBuffer;
		uint32 InstanceDataByteOffset = 0;
		uint64 IndirectArgsByteOffset = 0;
		RENDERER_API static auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*;
	};

	RENDERER_API auto GetMeshDrawCommandOverrideArgs(const FInstanceCullingDrawParams& Parameters, const FRDGParameterResolver& Resolver) -> RendererPrivate::FMeshDrawCommandOverrideArgs;

	struct FInstanceCullingPassParameters
	{
		FRDGBufferParameter Candidates;
		FRDGBufferParameter VisibleInstances;
		FRDGBufferParameter Arguments;
		FRDGBufferParameter InstanceSceneData;
		static RENDERER_API auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*;
	};

	struct FInstanceCullingView
	{
		FVector3 Location{};
		std::optional<FViewFrustum> Frustum;
	};

	// One pass's selection, graph-owned output and submission overrides. Shared
	// ownership keeps recording state alive across deferred graph callbacks.
	class FInstanceCullingContext final : public std::enable_shared_from_this<FInstanceCullingContext>
	{
	public:
		static constexpr uint32 InvalidArgument = UINT32_MAX;
		static constexpr size_t MinimumGroupCandidates = 64;
		static constexpr size_t MaximumCandidates = 65'536;
		static constexpr size_t MaximumGeneratedBytes = 4 * 1024 * 1024;
		RENDERER_API FInstanceCullingContext(std::shared_ptr<const FInstanceCullingView> InView, RendererPrivate::FMeshPassKey InPass, FInstanceCullingManager& InManager);
		RENDERER_API auto SetupDrawCommands(RendererPrivate::FMeshCommandOneFrameArray& VisibleMeshDrawCommands, std::span<const RendererPrivate::FMeshDrawCommandCullingPayload> Sources, size_t* Overflow = nullptr, size_t Budget = MaximumGeneratedBytes, FInstanceCullingAdmissionDiagnostics* Diagnostics = nullptr) -> bool;
		RENDERER_API auto BuildRenderingCommands(FRDGBuilder& Graph, FInstanceCullingTelemetry& Telemetry, bool bAsyncCompute, bool bObserveVisibility, EGPUCullingFailure Failure) -> FInstanceCullingDrawParams;
		RENDERER_API auto SubmitDrawCommands(std::span<const RendererPrivate::FVisibleMeshDrawCommand> VisibleMeshDrawCommands, const RendererPrivate::FMeshDrawCommandOverrideArgs& OverrideArgs, uint32 StartIndex, uint32 NumMeshDrawCommands, FRHICommandList& Commands, FMeshDrawCommandStateCache* Cache = nullptr, RendererPrivate::FMeshDrawCommandPassStats* Statistics = nullptr) const -> RendererPrivate::FMeshDrawCommandPassResult;
		auto GetArgumentIndex(uint32 Draw) const -> uint32
		{
			return bDispatched && Draw < MeshDrawCommandInfos.size() ? MeshDrawCommandInfos[Draw].ArgumentIndex : InvalidArgument;
		}

		auto GetCandidates() const -> std::span<const FInstanceCullingCandidate> { return Candidates; }
		auto GetArguments() const -> std::span<const FRHIDrawIndexedIndirectArguments> { return Arguments; }
		auto IsCullingDispatched() const -> bool { return bDispatched; }

	private:
		friend struct FInstanceCullingContextTestAccess;
		std::vector<FInstanceCullingCandidate> Candidates;
		std::vector<FRHIDrawIndexedIndirectArguments> Arguments;
		struct FMeshDrawCommandInfo
		{
			uint32 ArgumentIndex = InvalidArgument;
			RendererPrivate::FMeshDrawCommandRun Fallback;
		};
		// Exactly one entry per final visible command. Only merged GPU groups retain fallback sources.
		std::vector<FMeshDrawCommandInfo> MeshDrawCommandInfos;
		RendererPrivate::FMeshCommandOneFrameArray FallbackCommands;
		bool bDispatched = false;
		FInstanceCullingManager& Manager;
		std::shared_ptr<const FInstanceCullingView> View;
		RendererPrivate::FMeshPassKey Pass;
		FBufferRHIRef InstanceSceneData;
		std::vector<std::shared_ptr<FGPUSceneDynamicContext>> DynamicContexts;
		size_t SceneCapacityBytes = 0;
		size_t StandaloneSceneCapacityBytes = 0;
		bool bBuilt = false;
	};

	// Shared compute resources and view registration. Callers/graphs own contexts. Scene
	// attributes remain owned by GPUScene or the scoped dynamic upload owner.
	class FInstanceCullingManager
	{
	public:
		RENDERER_API explicit FInstanceCullingManager(FRendererResourceCoordinator& Coordinator);
		RENDERER_API ~FInstanceCullingManager();
		RENDERER_API auto ResetViews() -> void;
		RENDERER_API auto EnsureResources_RenderThread(EGPUCullingFailure Failure = EGPUCullingFailure::None) -> bool;
		RENDERER_API auto Dispatch_RenderThread(FRHICommandListImmediate& Commands, const FVector3& ViewLocation, const std::optional<FViewFrustum>& Frustum, uint32 CandidateCount, const FRDGShaderParameterScope& Parameters, EGPUCullingFailure Failure) -> bool;
		RENDERER_API auto ReleaseResources_RenderThread() -> void;
		RENDERER_API auto RegisterView(const FSceneView& View, std::optional<FViewFrustum> Frustum = {}) -> uint32;
		RENDERER_API auto CreateContext(uint32 ViewId, RendererPrivate::FMeshPassKey Pass) -> std::shared_ptr<FInstanceCullingContext>;

	private:
		struct FState;
		FRendererResourceCoordinator& Coordinator;
		std::unique_ptr<FState> State;
		std::vector<std::shared_ptr<const FInstanceCullingView>> Views;
	};
} // namespace Durin
