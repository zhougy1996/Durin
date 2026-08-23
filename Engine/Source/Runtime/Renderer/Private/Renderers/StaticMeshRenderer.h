#pragma once

#include "Misc/CoreTypes.h"

#include <memory>

namespace Durin
{
	class FGBufferRenderer;
	class FRendererResourceCoordinator;
	namespace RendererPrivate { class FSurfaceMaterialResources; }
	class FRHICommandListImmediate;
	struct FPreparedStaticMeshView;
	struct FPreparedStaticMeshDraw;
	struct FPreparedStaticMeshPrimitive;
	enum class ERasterMode : uint8;
	enum class ERenderMode : uint8;
	struct FRHIUniformBufferRange;
	struct FSceneView;
	enum class EMeshBasePass : uint8;

	// Owns StaticMesh shaders, material pipelines, and proxy draw submission.
	class FStaticMeshRenderer final
	{
	public:
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
			FPreparedStaticMeshView& PreparedView,
			bool bPrepareLitOpaqueForward
		) -> bool;
		auto PrepareHybridRetainedResources_RenderThread(
			FPreparedStaticMeshView& PreparedView
		) -> bool;
		auto PrepareShadowResources_RenderThread(
			FRHICommandListImmediate& CommandList,
			FPreparedStaticMeshView& PreparedView
		) -> bool;
		auto ExecuteShadow_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& ShadowView,
			const FRHIUniformBufferRange& FallbackLighting,
			FPreparedStaticMeshView& PreparedView
		) -> void;
		auto Execute_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FRHIUniformBufferRange& Lighting,
			ERenderMode RenderMode,
			FPreparedStaticMeshView& PreparedView
		) -> void;
		auto ExecutePass_RenderThread(
			FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EMeshBasePass Pass, FPreparedStaticMeshView& PreparedView
		) -> void;
		auto ExecutePreparedDraw_RenderThread(
			FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EMeshBasePass Pass, const FPreparedStaticMeshDraw& Draw, FPreparedStaticMeshView& PreparedView, bool bHybridRetained = false
		) -> void;
		auto FinalizeExecution_RenderThread(FPreparedStaticMeshView& PreparedView)
			-> void;
		auto ExecuteGBuffer_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			FGBufferRenderer& GBuffer,
			FPreparedStaticMeshView& PreparedView
		) -> void;
		auto ReleaseResources_RenderThread() -> void;

	private:
		auto DrawSection_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FRHIUniformBufferRange& Lighting,
			ERenderMode RenderMode,
			const FPreparedStaticMeshPrimitive& Primitive,
			const FPreparedStaticMeshDraw& Item,
			bool bShadowDepth = false,
			bool bHybridRetained = false
		) -> bool;
		auto EnsureMaterialSamplers_RenderThread(
			const FPreparedStaticMeshDraw& Item
		) -> bool;
		auto EnsureSectionResources_RenderThread(
			const FPreparedStaticMeshPrimitive& Primitive,
			const FPreparedStaticMeshDraw& Item,
			bool bShadowDepth = false,
			bool bHybridRetained = false
		) -> bool;
		auto DrawGBufferSection_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			FGBufferRenderer& GBuffer,
			const FPreparedStaticMeshPrimitive& Primitive,
			const FPreparedStaticMeshDraw& Item
		) -> bool;
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		RendererPrivate::FSurfaceMaterialResources& SurfaceMaterials;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
