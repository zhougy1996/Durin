#pragma once

#include "RHIResources.h"

#include <memory>

namespace Durin
{
	class FRendererResourceCoordinator;
	class FRendererTransientTargetPool;
	class FRHICommandListImmediate;
	class FFullscreenGeometryResources;
	struct FSceneView;
	enum class EGBufferDebugMode : uint8;

	class FGBufferDebugRenderer final
	{
	public:
		struct FTargets { FTextureRHIRef Color; };
		FGBufferDebugRenderer(
			FRendererResourceCoordinator& InCoordinator,
			FFullscreenGeometryResources& InFullscreenGeometry,
			FRendererTransientTargetPool& InTransientTargets);
		~FGBufferDebugRenderer();
		auto EnsureTargets_RenderThread(uint32 Width, uint32 Height) -> FTargets*;

		auto Render_RenderThread(
			FRHICommandListImmediate& CommandList,
			FRHITexture* Material,
			FRHITexture* Normals,
			FRHITexture* Surface,
			FRHITexture* Emissive,
			FRHITexture* Depth,
			FRHITexture* Output,
			const FSceneView& View,
			EGBufferDebugMode Mode,
			uint32 Width,
			uint32 Height) -> bool;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;
		FRendererResourceCoordinator& Coordinator;
		FFullscreenGeometryResources& FullscreenGeometry;
		FRendererTransientTargetPool& TransientTargets;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
