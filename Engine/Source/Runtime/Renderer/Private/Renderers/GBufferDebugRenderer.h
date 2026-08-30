#pragma once

#include "RHIResources.h"

#include <memory>
#include <optional>

namespace Durin
{
	class FRendererResourceCoordinator;
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
			FFullscreenGeometryResources& InFullscreenGeometry);
		~FGBufferDebugRenderer();
		static auto DescribeTarget(uint32 Width, uint32 Height)
			-> FRHITextureCreateDesc;

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
		std::unique_ptr<FState> State;
	};
} // namespace Durin
