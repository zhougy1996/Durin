#pragma once

#include "EngineAPI.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureRenderResource.h"

namespace Durin
{
	struct FTextureCubePlatformData;

	// One asset-owned cube allocation. Only render commands access its RHI state.
	class FTextureCubeResource final : public FTextureAssetResource
	{
	public:
		ENGINE_API FTextureCubeResource(
			FTextureReference* InTextureReference,
			std::shared_ptr<const FTextureCubePlatformData> InPlatformData,
			uint64 InRevision,
			std::shared_ptr<FTextureResourceCompletion> InCompletion);
		ENGINE_API ~FTextureCubeResource() override;

		ENGINE_API auto InitRHI(FRHICommandListBase& RHICmdList) -> void override;
		auto GetFriendlyName() const -> std::string override
		{
			return "FTextureCubeResource";
		}

	private:
		std::shared_ptr<const FTextureCubePlatformData> PlatformData;
	};
}
