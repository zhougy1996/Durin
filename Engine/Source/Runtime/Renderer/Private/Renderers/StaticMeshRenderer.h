#pragma once

#include "Misc/CoreTypes.h"
#include "Renderers/MeshRendererExecution.h"

#include <memory>

namespace Durin
{
	class FGBufferRenderer;
	class FRendererResourceCoordinator;
	namespace RendererPrivate { class FSurfaceMaterialResources; }
	class FRHICommandListImmediate;
	struct FPreparedStaticMeshView;
	struct FResolvedStaticMeshView;
	struct FResolvedMeshPipeline;
	struct FPreparedStaticMeshDraw;
	struct FPreparedStaticMeshPrimitive;
	struct FMaterialRenderBinding;
	enum class ERasterMode : uint8;
	enum class ERenderMode : uint8;
	struct FRHIUniformBufferRange;
	struct FSceneView;
	enum class EMeshBasePass : uint8;

	// Owns StaticMesh shaders, material pipelines, and proxy draw submission.
	class FStaticMeshRenderer final
	{
	public:
		auto PrepareGBufferPipelines_RenderThread(FGBufferRenderer& GBuffer, const FPreparedStaticMeshView& PreparedView) -> bool;
		FStaticMeshRenderer(
			FRendererResourceCoordinator& InCoordinator,
			RendererPrivate::FSurfaceMaterialResources& InSurfaceMaterials
		);
		~FStaticMeshRenderer();

		FStaticMeshRenderer(const FStaticMeshRenderer&) = delete;
		auto operator=(const FStaticMeshRenderer&)
			-> FStaticMeshRenderer& = delete;

		auto PrepareResources_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FPreparedStaticMeshView& PreparedView,
			FResolvedStaticMeshView& ResolvedView,
			bool bPrepareLitOpaqueForward
		) -> FGeometryResolutionResult;
		auto PrepareHybridRetainedResources_RenderThread(
			const FPreparedStaticMeshView& PreparedView,
			FResolvedStaticMeshView& ResolvedView
		) -> bool;
		auto PrepareShadowResources_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FPreparedStaticMeshView& PreparedView,
			FResolvedStaticMeshView& ResolvedView
		) -> FGeometryResolutionResult;
		auto ExecuteShadow_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& ShadowView,
			const FRHIUniformBufferRange& FallbackLighting,
			const FPreparedStaticMeshView& PreparedView,
			FResolvedStaticMeshView& ResolvedView
		) -> bool;
		auto Execute_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FRHIUniformBufferRange& Lighting,
			ERenderMode RenderMode,
			const FPreparedStaticMeshView& PreparedView,
			FResolvedStaticMeshView& ResolvedView
		) -> void;
		auto ExecutePass_RenderThread(
			FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EMeshBasePass Pass, const FPreparedStaticMeshView& PreparedView, FResolvedStaticMeshView& ResolvedView
		) -> void;
		auto ExecutePreparedDraw_RenderThread(
			FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EMeshBasePass Pass, const FPreparedStaticMeshDraw& Draw, const FPreparedStaticMeshView& PreparedView, FResolvedStaticMeshView& ResolvedView, bool bHybridRetained = false
		) -> void;
		auto FinalizeExecution_RenderThread(FResolvedStaticMeshView& ResolvedView)
			-> void;
		auto ExecuteGBuffer_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			FGBufferRenderer& GBuffer,
			const FPreparedStaticMeshView& PreparedView,
			FResolvedStaticMeshView& ResolvedView
		) -> FGeometryExecutionResult;
		auto ReleaseResources_RenderThread() -> void;

		auto PrepareUniforms_RenderThread(FRHICommandListImmediate& CommandList,
			const FSceneView& View, const FPreparedStaticMeshView& PreparedView,
			FResolvedStaticMeshView& ResolvedView, bool bProductionDeferred,
			bool bGBuffer, bool bShadow = false) -> bool;

	private:
		auto DrawSection_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FRHIUniformBufferRange& Lighting,
			ERenderMode RenderMode,
			const FPreparedStaticMeshPrimitive& Primitive,
			const FPreparedStaticMeshDraw& Item,
			const FResolvedStaticMeshView& ResolvedView,
			bool bShadowDepth = false,
			bool bHybridRetained = false
		) -> bool;
		auto EnsureMaterialSamplers_RenderThread(
			const FMaterialRenderBinding& MaterialBinding
		) -> bool;
		auto EnsureSectionResources_RenderThread(
			const FPreparedStaticMeshPrimitive& Primitive,
			const FPreparedStaticMeshDraw& Item,
			const FMaterialRenderBinding& MaterialBinding,
			std::shared_ptr<const FResolvedMeshPipeline>& OutPipeline,
			bool bShadowDepth = false,
			bool bHybridRetained = false
		) -> bool;
		auto DrawGBufferSection_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			FGBufferRenderer& GBuffer,
			const FPreparedStaticMeshPrimitive& Primitive,
			const FPreparedStaticMeshDraw& Item,
			const FResolvedStaticMeshView& ResolvedView
		) -> bool;
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		RendererPrivate::FSurfaceMaterialResources& SurfaceMaterials;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
