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
	struct FPreparedSkeletalMeshView;
	struct FResolvedSkeletalMeshView;
	struct FPreparedSkeletalMeshDraw;
	struct FPreparedSkeletalMeshPrimitive;
	struct FPreparedSkeletalPaletteTable;
	struct FResolvedSkeletalPaletteTable;
	struct FMaterialRenderBinding;
	enum class ERenderMode : uint8;
	struct FRHIUniformBufferRange;
	struct FSceneView;
	enum class EMeshBasePass : uint8;

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
			const FPreparedSkeletalPaletteTable& PreparedPalettes,
			FResolvedSkeletalPaletteTable& ResolvedPalettes,
			const FPreparedSkeletalMeshView& PreparedView,
			FResolvedSkeletalMeshView& ResolvedView,
			bool bPrepareLitOpaqueForward
		) -> FGeometryResolutionResult;
		auto PrepareHybridRetainedResources_RenderThread(
			const FPreparedSkeletalMeshView& PreparedView,
			const FResolvedSkeletalMeshView& ResolvedView
		) -> bool;
		auto PrepareShadowResources_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FPreparedSkeletalPaletteTable& PreparedPalettes,
			FResolvedSkeletalPaletteTable& ResolvedPalettes,
			const FPreparedSkeletalMeshView& PreparedView,
			FResolvedSkeletalMeshView& ResolvedView
		) -> FGeometryResolutionResult;
		auto ExecuteShadow_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& ShadowView,
			const FRHIUniformBufferRange& FallbackLighting,
			const FPreparedSkeletalMeshView& PreparedView,
			FResolvedSkeletalMeshView& ResolvedView
		) -> bool;
		auto Execute_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FRHIUniformBufferRange& Lighting,
			ERenderMode RenderMode,
			const FPreparedSkeletalMeshView& PreparedView,
			FResolvedSkeletalMeshView& ResolvedView
		) -> void;
		auto ExecutePass_RenderThread(
			FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EMeshBasePass Pass, const FPreparedSkeletalMeshView& PreparedView, FResolvedSkeletalMeshView& ResolvedView
		) -> void;
		auto ExecutePreparedDraw_RenderThread(
			FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EMeshBasePass Pass, const FPreparedSkeletalMeshDraw& Draw, const FPreparedSkeletalMeshView& PreparedView, FResolvedSkeletalMeshView& ResolvedView, bool bHybridRetained = false
		) -> void;
		auto FinalizeExecution_RenderThread(FResolvedSkeletalMeshView& ResolvedView)
			-> void;
		auto ExecuteGBuffer_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			FGBufferRenderer& GBuffer,
			const FPreparedSkeletalMeshView& PreparedView,
			FResolvedSkeletalMeshView& ResolvedView
		) -> FGeometryExecutionResult;
		auto ReleaseResources_RenderThread() -> void;

	private:
		auto EnsureMaterialSamplers_RenderThread(
			const FMaterialRenderBinding& MaterialBinding
		) -> bool;
		auto EnsureSectionResources_RenderThread(
			const FPreparedSkeletalMeshPrimitive& Primitive,
			const FPreparedSkeletalMeshDraw& Item,
			const FMaterialRenderBinding& MaterialBinding,
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
			const FResolvedSkeletalMeshView& ResolvedView,
			bool bShadowDepth = false,
			bool bHybridRetained = false
		) -> bool;
		auto DrawGBufferSection_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			FGBufferRenderer& GBuffer,
			const FPreparedSkeletalMeshPrimitive& Primitive,
			const FPreparedSkeletalMeshDraw& Item,
			const FResolvedSkeletalMeshView& ResolvedView
		) -> bool;
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		RendererPrivate::FSurfaceMaterialResources& SurfaceMaterials;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
