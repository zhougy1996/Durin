#pragma once

#include "EngineAPI.h"
#include "RHIResources.h"

namespace Durin
{
	struct FTexturePlatformData;

	// Cross-thread lifetime proxy for DTexture2D. Only render commands may touch TextureRHI.
	class ENGINE_API FTexture2DRenderResource final : public std::enable_shared_from_this<FTexture2DRenderResource>
	{
	public:
		FTexture2DRenderResource() = default;
		~FTexture2DRenderResource();

		FTexture2DRenderResource(const FTexture2DRenderResource&) = delete;
		auto operator=(const FTexture2DRenderResource&) -> FTexture2DRenderResource& = delete;

		auto QueueBuild(std::shared_ptr<const FTexturePlatformData> PlatformDataSnapshot, uint64 Revision) -> void;
		auto QueueRelease(uint64 Revision) -> void;

		// These accessors are render-thread-only. Callers must resolve nullptr to a renderer default texture.
		auto GetTextureRHI_RenderThread() const -> FRHITexture*;
		auto IsReady_RenderThread() const -> bool;
		auto GetAppliedRevision_RenderThread() const -> uint64;

	private:
		auto Build_RenderThread(FRHICommandListImmediate& CommandList, const FTexturePlatformData& PlatformData, uint64 Revision) -> void;
		auto Release_RenderThread(uint64 Revision) -> void;

		FTextureRHIRef TextureRHI;
		uint64 AppliedRevision = 0;
		std::atomic<uint64> RequestedRevision = 0;
	};
}
