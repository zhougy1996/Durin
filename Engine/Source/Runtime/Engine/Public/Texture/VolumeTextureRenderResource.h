#pragma once

#include "EngineAPI.h"
#include "Texture/TextureRenderResource.h"

namespace Durin
{
	struct FVolumeTexturePlatformData;

	// Persistent volume render representation; immutable upload input is consumed by initialization.
	class FVolumeTextureResource final : public FTextureResource
	{
	public:
		ENGINE_API FVolumeTextureResource(FTextureReference* InTextureReference,
			std::shared_ptr<const FVolumeTexturePlatformData> InPlatformData);
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
