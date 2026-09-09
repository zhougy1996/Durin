#pragma once

#include "EngineAPI.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureRenderResource.h"

namespace Durin
{
	struct FTexturePlatformData;

	// Persistent Texture2D render representation; immutable upload input is consumed by initialization.
	class FTexture2DResource final : public FTextureResource
	{
	public:
		ENGINE_API FTexture2DResource(
			FTextureReference* InTextureReference,
			std::shared_ptr<const FTexturePlatformData> InPlatformData);
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
