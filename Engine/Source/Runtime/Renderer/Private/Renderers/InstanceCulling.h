#pragma once

#include "CoreMinimal.h"
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
		std::optional<FRDGBufferHandle> InstanceIdOffsetBuffer;
		std::optional<FRDGBufferHandle> DrawIndirectArgsBuffer;
	};

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
		static constexpr uint32 GroupedMember = UINT32_MAX - 1;
		static constexpr size_t MinimumGroupCandidates = 64;
		static constexpr size_t MaximumCandidates = 65'536;
		static constexpr size_t MaximumGeneratedBytes = 4 * 1024 * 1024;
		RENDERER_API FInstanceCullingContext(std::shared_ptr<const FInstanceCullingView> InView, RendererPrivate::FMeshPassKey InPass, FInstanceCullingManager* InManager = nullptr);
		RENDERER_API auto SetupDrawCommands(std::span<const RendererPrivate::FVisibleMeshDrawCommand> VisibleMeshDrawCommands, bool bRequested, bool bIndirectSupported, size_t* Overflow = nullptr, size_t Budget = MaximumGeneratedBytes, FInstanceCullingAdmissionDiagnostics* Diagnostics = nullptr) -> bool;
		RENDERER_API auto BuildRenderingCommands(FRDGBuilder& Graph, FInstanceCullingTelemetry& Telemetry, bool bAsyncCompute, bool bObserveVisibility, EGPUCullingFailure Failure) -> FInstanceCullingDrawParams;
		RENDERER_API auto PrepareForSubmission(FResolvedStaticMeshView& Resolved) const -> void;
		RENDERER_API auto SubmitDrawCommands(std::span<const RendererPrivate::FVisibleMeshDrawCommand> VisibleMeshDrawCommands, const RendererPrivate::FMeshDrawCommandOverrideArgs& OverrideArgs, uint32 StartIndex, uint32 NumMeshDrawCommands, FRHICommandList& Commands, FMeshDrawCommandStateCache* Cache = nullptr) const -> bool;
		auto GetArgumentIndex(uint32 Draw) const -> uint32
		{
			return bDispatched && Draw < ArgumentByResolvedDraw.size() ? ArgumentByResolvedDraw[Draw] : InvalidArgument;
		}

		std::vector<FInstanceCullingCandidate> Candidates;
		std::vector<FRHIDrawIndexedIndirectArguments> Arguments;
		std::vector<uint32> ArgumentByResolvedDraw;
		bool bDispatched = false;

	private:
		FInstanceCullingManager* Manager = nullptr;
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
		RENDERER_API FInstanceCullingManager();
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
		FRendererResourceCoordinator* Coordinator = nullptr;
		std::unique_ptr<FState> State;
		std::vector<std::shared_ptr<const FInstanceCullingView>> Views;
	};
} // namespace Durin
