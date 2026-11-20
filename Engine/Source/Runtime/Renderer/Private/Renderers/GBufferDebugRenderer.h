#pragma once

#include "CoreMinimal.h"

#include "RHIResources.h"

namespace Durin
{
	class FRHICommandListImmediate;
	class FFullscreenGeometryResources;
	struct FSceneView;
	enum class EGBufferDebugMode : uint8;

	class FGBufferDebugRenderer final
	{
	public:
		struct FTargets { FTextureRHIRef Color; };
		FGBufferDebugRenderer(
			FFullscreenGeometryResources& InFullscreenGeometry);
		~FGBufferDebugRenderer();
		static auto DescribeTarget(uint32 Width, uint32 Height)
			-> FRHITextureCreateDesc;

		auto Render_RenderThread(
			FRHICommandListImmediate& CommandList,
			FRHITexture* Material,
			FRHITexture* Normals,
			FRHITexture* Surface,
			FRHITexture* BasePassColor,
			FRHITexture* Depth,
			FRHITexture* Output,
			const FSceneView& View,
			EGBufferDebugMode Mode,
			uint32 Width,
			uint32 Height) -> bool;
		auto ReleaseResources_RenderThread() -> void;
		auto EnsureResources_RenderThread(FRHICommandListImmediate& CommandList, bool bPrecachePipeline = false) -> bool;

	private:
		struct FState;
		FFullscreenGeometryResources& FullscreenGeometry;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
