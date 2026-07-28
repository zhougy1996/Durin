#pragma once

#include "EngineAPI.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureRenderResource.h"

namespace Durin
{
	struct FTexturePlatformData;

	// One asset-owned Texture2D allocation. Only render commands access its RHI state.
	class FTexture2DResource final : public FTextureAssetResource
	{
	public:
		ENGINE_API FTexture2DResource(
			FTextureReference* InTextureReference,
			std::shared_ptr<const FTexturePlatformData> InPlatformData,
			uint64 InRevision,
			std::shared_ptr<FTextureResourceCompletion> InCompletion);
		ENGINE_API ~FTexture2DResource() override;

		ENGINE_API auto InitRHI(FRHICommandListBase& RHICmdList) -> void override;
		auto GetFriendlyName() const -> std::string override
		{
			return "FTexture2DResource";
		}

	private:
		std::shared_ptr<const FTexturePlatformData> PlatformData;
	};
}
