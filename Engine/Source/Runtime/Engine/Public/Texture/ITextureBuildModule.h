#pragma once

#include "Modules/ModuleManager.h"
#include "Texture/Texture2DBuildTypes.h"
#include "Texture/TextureCubeBuildTypes.h"
#include "Texture/VolumeTextureBuildTypes.h"

namespace Durin
{
	// Fixed Developer module contract. Recipes return detached CPU values only.
	class ITextureBuildModule : public IModuleInterface
	{
	public:
		// Borrow the active implementation. Consumers drain work before editor shutdown.
		ENGINE_API static auto Get() -> ITextureBuildModule*;
		virtual auto GetTexture2DDescriptor() const -> FTexture2DBuildDescriptor = 0;
		virtual auto GetTextureCubeDescriptor() const -> FTextureCubeBuildDescriptor = 0;
		virtual auto GetVolumeTextureDescriptor() const -> FVolumeTextureBuildDescriptor = 0;
		virtual auto BuildTexture2D(const FTexture2DRecipeBuildRequest& Request,
			const FTexture2DRecipeExecutionControl* Control = nullptr)
			-> std::expected<FTexture2DRecipeBuildProduct, FTexture2DBuildError> = 0;
		virtual auto NormalizeTextureCube(const FTextureCubeNormalizeRequest& Request)
			-> std::expected<FTextureCubeCanonicalBuildInput, FTextureBuildError> = 0;
		virtual auto BuildTextureCube(const FTextureCubeRecipeBuildRequest& Request)
			-> std::expected<FTextureCubeRecipeBuildProduct, FTextureBuildError> = 0;
		virtual auto BuildVolumeTexture(const FVolumeTextureRecipeBuildRequest& Request)
			-> std::expected<FVolumeTextureRecipeBuildProduct, FTextureBuildError> = 0;
	};
}
