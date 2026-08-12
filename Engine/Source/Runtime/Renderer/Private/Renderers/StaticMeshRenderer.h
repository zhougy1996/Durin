#pragma once

#include "Misc/CoreTypes.h"

#include <memory>

namespace Durin
{
	class FDefaultTextureResources;
	class FEnvironmentLightingResources;
	class FRendererResourceCoordinator;
	class FRHICommandListImmediate;
	struct FPreparedStaticMeshView;
	struct FPreparedStaticMeshDraw;
	struct FPreparedStaticMeshPrimitive;
	enum class ERasterMode : uint8;
	enum class ERenderMode : uint8;
	struct FRHIUniformBufferRange;
	struct FSceneView;
	enum class EStaticMeshBasePass : uint8;

	// Owns StaticMesh shaders, material pipelines, and proxy draw submission.
	class FStaticMeshRenderer final
	{
	public:
		FStaticMeshRenderer(
			FRendererResourceCoordinator& InCoordinator,
			FDefaultTextureResources& InDefaultTextures,
			FEnvironmentLightingResources& InEnvironmentLighting
		);
		~FStaticMeshRenderer();

		FStaticMeshRenderer(const FStaticMeshRenderer&) = delete;
		auto operator=(const FStaticMeshRenderer&)
			-> FStaticMeshRenderer& = delete;

		auto PrepareResources_RenderThread(
			FRHICommandListImmediate& CommandList,
			FPreparedStaticMeshView& PreparedView) -> bool;
		auto Execute_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FRHIUniformBufferRange& Lighting,
			ERenderMode RenderMode,
			FPreparedStaticMeshView& PreparedView
		) -> void;
		auto ExecutePass_RenderThread(
			FRHICommandListImmediate& CommandList, const FSceneView& View,
			const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode,
			EStaticMeshBasePass Pass, FPreparedStaticMeshView& PreparedView) -> void;
		auto ExecutePreparedDraw_RenderThread(
			FRHICommandListImmediate& CommandList, const FSceneView& View,
			const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode,
			EStaticMeshBasePass Pass, const FPreparedStaticMeshDraw& Draw,
			FPreparedStaticMeshView& PreparedView) -> void;
		auto FinalizeExecution_RenderThread(FPreparedStaticMeshView& PreparedView)
			-> void;
		auto ReleaseResources_RenderThread() -> void;

	private:
		auto DrawSection_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FRHIUniformBufferRange& Lighting,
			ERenderMode RenderMode,
			const FPreparedStaticMeshPrimitive& Primitive,
			const FPreparedStaticMeshDraw& Item
		) -> bool;
		auto EnsureBaseResources_RenderThread() -> bool;
		auto EnsureSectionResources_RenderThread(
			const FPreparedStaticMeshPrimitive& Primitive,
			const FPreparedStaticMeshDraw& Item) -> bool;
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		FDefaultTextureResources& DefaultTextures;
		FEnvironmentLightingResources& EnvironmentLighting;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
