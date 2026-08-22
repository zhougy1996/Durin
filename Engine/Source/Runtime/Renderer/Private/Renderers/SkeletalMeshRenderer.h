#pragma once

#include "Misc/CoreTypes.h"

#include <memory>

namespace Durin
{
	class FGBufferRenderer;
	class FRendererResourceCoordinator;
	namespace RendererPrivate { class FSurfaceMaterialResources; }
	class FRHICommandListImmediate;
	struct FPreparedSkeletalMeshView;
	struct FPreparedSkeletalMeshDraw;
	struct FPreparedSkeletalMeshPrimitive;
	struct FPreparedSkeletalPaletteTable;
	enum class ERenderMode : uint8;
	struct FRHIUniformBufferRange;
	struct FSceneView;
	enum class EStaticMeshBasePass : uint8;

	class FSkeletalMeshRenderer final
	{
	public:
		FSkeletalMeshRenderer(
			FRendererResourceCoordinator& InCoordinator,
			RendererPrivate::FSurfaceMaterialResources& InSurfaceMaterials
		);
		~FSkeletalMeshRenderer();
		FSkeletalMeshRenderer(const FSkeletalMeshRenderer&) = delete;
		auto operator=(const FSkeletalMeshRenderer&)
			-> FSkeletalMeshRenderer& = delete;

		auto PrepareResources_RenderThread(
			FRHICommandListImmediate& CommandList,
			FPreparedSkeletalPaletteTable& PaletteTable,
			FPreparedSkeletalMeshView& PreparedView,
			bool bPrepareLitOpaqueForward
		) -> bool;
		auto PrepareHybridRetainedResources_RenderThread(
			FPreparedSkeletalMeshView& PreparedView
		) -> bool;
		auto PrepareShadowResources_RenderThread(
			FRHICommandListImmediate& CommandList,
			FPreparedSkeletalPaletteTable& PaletteTable,
			FPreparedSkeletalMeshView& PreparedView
		) -> bool;
		auto ExecuteShadow_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& ShadowView,
			const FRHIUniformBufferRange& FallbackLighting,
			FPreparedSkeletalMeshView& PreparedView
		) -> void;
		auto Execute_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FRHIUniformBufferRange& Lighting,
			ERenderMode RenderMode,
			FPreparedSkeletalMeshView& PreparedView
		) -> void;
		auto ExecutePass_RenderThread(
			FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EStaticMeshBasePass Pass, FPreparedSkeletalMeshView& PreparedView
		) -> void;
		auto ExecutePreparedDraw_RenderThread(
			FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EStaticMeshBasePass Pass, const FPreparedSkeletalMeshDraw& Draw, FPreparedSkeletalMeshView& PreparedView, bool bHybridRetained = false
		) -> void;
		auto FinalizeExecution_RenderThread(FPreparedSkeletalMeshView& PreparedView)
			-> void;
		auto ExecuteGBuffer_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			FGBufferRenderer& GBuffer,
			FPreparedSkeletalMeshView& PreparedView
		) -> void;
		auto ReleaseResources_RenderThread() -> void;

	private:
		auto EnsureMaterialSamplers_RenderThread(
			const FPreparedSkeletalMeshDraw& Item
		) -> bool;
		auto EnsureSectionResources_RenderThread(
			const FPreparedSkeletalMeshPrimitive& Primitive,
			const FPreparedSkeletalMeshDraw& Item,
			bool bShadowDepth = false,
			bool bHybridRetained = false
		) -> bool;
		auto DrawSection_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FRHIUniformBufferRange& Lighting,
			ERenderMode RenderMode,
			const FPreparedSkeletalMeshPrimitive& Primitive,
			const FPreparedSkeletalMeshDraw& Item,
			bool bShadowDepth = false,
			bool bHybridRetained = false
		) -> bool;
		auto DrawGBufferSection_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			FGBufferRenderer& GBuffer,
			const FPreparedSkeletalMeshPrimitive& Primitive,
			const FPreparedSkeletalMeshDraw& Item
		) -> bool;
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		RendererPrivate::FSurfaceMaterialResources& SurfaceMaterials;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
