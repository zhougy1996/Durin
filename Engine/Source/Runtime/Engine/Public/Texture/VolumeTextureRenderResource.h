#pragma once

#include "EngineAPI.h"
#include "Texture/TextureRenderResource.h"

namespace Durin
{
	struct FVolumeTexturePlatformData;

	// Uploads one immutable volume payload and publishes it through a texture reference.
	class FVolumeTextureResource final : public FTextureAssetResource
	{
	public:
		ENGINE_API FVolumeTextureResource(FTextureReference* InTextureReference,
			std::shared_ptr<const FVolumeTexturePlatformData> InPlatformData,
			uint64 InRevision,
			std::shared_ptr<FTextureResourceCompletion> InCompletion);
		ENGINE_API ~FVolumeTextureResource() override;

		ENGINE_API auto InitRHI(FRHICommandListBase& RHICmdList) -> void override;
		auto GetFriendlyName() const -> std::string override
		{
			return "FVolumeTextureResource";
		}

	private:
		std::shared_ptr<const FVolumeTexturePlatformData> PlatformData;
	};
}
