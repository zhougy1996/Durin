#pragma once

#include "Misc/CoreTypes.h"

#include <memory>

namespace Durin
{
	class FDefaultTextureResources;
	class FEnvironmentLightingResources;
	class FRendererResourceCoordinator;
	class FRHICommandListImmediate;
	struct FPreparedSkeletalMeshView;
	struct FPreparedSkeletalMeshDraw;
	struct FPreparedSkeletalMeshPrimitive;
	enum class ERenderMode : uint8;
	struct FRHIUniformBufferRange;
	struct FSceneView;
	enum class EStaticMeshBasePass : uint8;

	class FSkeletalMeshRenderer final
	{
	public:
		FSkeletalMeshRenderer(
			FRendererResourceCoordinator& InCoordinator,
			FDefaultTextureResources& InDefaultTextures,
			FEnvironmentLightingResources& InEnvironmentLighting);
		~FSkeletalMeshRenderer();
		FSkeletalMeshRenderer(const FSkeletalMeshRenderer&) = delete;
		auto operator=(const FSkeletalMeshRenderer&)
			-> FSkeletalMeshRenderer& = delete;

		auto PrepareResources_RenderThread(
			FRHICommandListImmediate& CommandList,
			FPreparedSkeletalMeshView& PreparedView) -> bool;
		auto PrepareShadowResources_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FPreparedSkeletalMeshView& BaseView,
			FPreparedSkeletalMeshView& PreparedView) -> bool;
		auto ExecuteShadow_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& ShadowView,
			const FRHIUniformBufferRange& FallbackLighting,
			FPreparedSkeletalMeshView& PreparedView) -> void;
		auto Execute_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FRHIUniformBufferRange& Lighting,
			ERenderMode RenderMode,
			FPreparedSkeletalMeshView& PreparedView) -> void;
		auto ExecutePass_RenderThread(
			FRHICommandListImmediate& CommandList, const FSceneView& View,
			const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode,
			EStaticMeshBasePass Pass, FPreparedSkeletalMeshView& PreparedView) -> void;
		auto ExecutePreparedDraw_RenderThread(
			FRHICommandListImmediate& CommandList, const FSceneView& View,
			const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode,
			EStaticMeshBasePass Pass, const FPreparedSkeletalMeshDraw& Draw,
			FPreparedSkeletalMeshView& PreparedView) -> void;
		auto FinalizeExecution_RenderThread(FPreparedSkeletalMeshView& PreparedView)
			-> void;
		auto ReleaseResources_RenderThread() -> void;

	private:
		auto EnsureBaseResources_RenderThread() -> bool;
		auto EnsureSectionResources_RenderThread(
			const FPreparedSkeletalMeshPrimitive& Primitive,
			const FPreparedSkeletalMeshDraw& Item,
			bool bShadowDepth = false) -> bool;
		auto DrawSection_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FRHIUniformBufferRange& Lighting,
			ERenderMode RenderMode,
			const FPreparedSkeletalMeshPrimitive& Primitive,
			const FPreparedSkeletalMeshDraw& Item,
			bool bShadowDepth = false) -> bool;
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		FDefaultTextureResources& DefaultTextures;
		FEnvironmentLightingResources& EnvironmentLighting;
		std::unique_ptr<FState> State;
	};
}
