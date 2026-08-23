#pragma once

#include "Misc/CoreTypes.h"
#include "Renderers/MeshRendererExecution.h"

#include <array>
#include <span>
#include <vector>

namespace Durin
{
	class FGBufferRenderer;
	class FRendererResourceCoordinator;
	namespace RendererPrivate { class FSurfaceMaterialResources; }
	class FRHICommandListImmediate;
	struct FPreparedTerrainDraw;
	struct FPreparedTerrainBatch;
	struct FPreparedTerrainView;
	struct FResolvedTerrainView;
	struct FRHIUniformBufferRange;
	struct FSceneView;
	enum class ERenderMode : uint8;
	enum class EMeshBasePass : uint8;

	// Owns exact height/topology caches and Terrain shader draw submission.
	class FTerrainRenderer final
	{
	public:
		FTerrainRenderer(FRendererResourceCoordinator& InCoordinator, RendererPrivate::FSurfaceMaterialResources& InSurfaceMaterials);
		~FTerrainRenderer();

		auto PrepareResources_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FPreparedTerrainView& View,
			FResolvedTerrainView& ResolvedView,
			bool bPrepareLitOpaqueForward
		) -> FGeometryResolutionResult;
		auto PrepareHybridRetainedResources_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FPreparedTerrainView& View,
			FResolvedTerrainView& ResolvedView
		) -> bool;
		auto PrepareShadowResources_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FPreparedTerrainView& View,
			FResolvedTerrainView& ResolvedView
		) -> FGeometryResolutionResult;
		auto ExecuteShadow_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& ShadowView,
			const FRHIUniformBufferRange& FallbackLighting,
			const FPreparedTerrainView& View,
			FResolvedTerrainView& ResolvedView
		) -> bool;
		auto ExecutePass_RenderThread(FRHICommandListImmediate& CommandList, const FSceneView& SceneView, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EMeshBasePass Pass, const FPreparedTerrainView& View, FResolvedTerrainView& ResolvedView) -> void;
		auto ExecutePreparedDraw_RenderThread(FRHICommandListImmediate& CommandList, const FSceneView& SceneView, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, const FPreparedTerrainDraw& Draw, const FPreparedTerrainView& View, FResolvedTerrainView& ResolvedView, bool bHybridRetained = false) -> void;
		auto FinalizeExecution_RenderThread(FResolvedTerrainView& ResolvedView) -> void;
		auto ExecuteGBuffer_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& SceneView,
			FGBufferRenderer& GBuffer,
			const FPreparedTerrainView& View,
			FResolvedTerrainView& ResolvedView
		) -> FGeometryExecutionResult;
		auto ReleaseResources_RenderThread() -> void;

	private:
		auto EnsureDrawResources_RenderThread(FRHICommandListImmediate& CommandList, const FPreparedTerrainDraw& Draw, FResolvedTerrainView& ResolvedView, bool bShadowDepth = false, bool bHybridRetained = false, bool bPrepareForwardPipeline = true) -> bool;
		auto Draw_RenderThread(FRHICommandListImmediate& CommandList, const FSceneView& SceneView, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, const FPreparedTerrainDraw& Draw, const FResolvedTerrainView& ResolvedView, bool bShadowDepth = false, std::span<const std::array<uint32, 2>> InstanceOrigins = {}, uint64* OutDynamicAllocationNanoseconds = nullptr, FGBufferRenderer* GBuffer = nullptr, bool bHybridRetained = false) -> bool;
		auto DrawBatch_RenderThread(FRHICommandListImmediate& CommandList, const FSceneView& SceneView, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, const std::vector<FPreparedTerrainDraw>& Draws, const FPreparedTerrainBatch& Batch, const FResolvedTerrainView& ResolvedView, bool bShadowDepth = false, uint64* OutDynamicAllocationNanoseconds = nullptr, FGBufferRenderer* GBuffer = nullptr) -> bool;
		struct FState;
		FRendererResourceCoordinator& Coordinator;
		RendererPrivate::FSurfaceMaterialResources& SurfaceMaterials;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
