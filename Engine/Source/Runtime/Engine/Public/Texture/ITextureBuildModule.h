#pragma once

#include "Modules/ModuleManager.h"
#include "Texture/Texture2DBuildTypes.h"
#include "Texture/TextureCubeBuildTypes.h"
#include "Texture/VolumeTextureBuildTypes.h"

namespace Durin
{
	// Fixed Developer module contract. Builds return detached CPU values only.
	class ITextureBuildModule : public IModuleInterface
	{
	public:
		// Borrow the active implementation. Consumers drain work before editor shutdown.
		ENGINE_API static auto Get() -> ITextureBuildModule*;
		virtual auto GetTexture2DBuilderVersion() const -> uint32 = 0;
		virtual auto GetTextureCubeBuilderVersion() const -> uint32 = 0;
		virtual auto GetTextureCubeProjectionVersion() const -> uint32 = 0;
		virtual auto GetVolumeTextureBuilderVersion() const -> uint32 = 0;
		virtual auto BuildTexture2D(const FTexture2DBuildInput& Request,
			const FTexture2DBuildControl* Control = nullptr)
			-> std::expected<FTexture2DBuildOutput, FTexture2DBuildError> = 0;
		virtual auto NormalizeTextureCube(const FTextureCubeNormalizeRequest& Request)
			-> std::expected<FTextureCubeCanonicalBuildInput, FTextureBuildError> = 0;
		virtual auto BuildTextureCube(const FTextureCubeBuildInput& Request)
			-> std::expected<std::unique_ptr<FTextureCubePlatformData>, FTextureBuildError> = 0;
		virtual auto BuildVolumeTexture(const FVolumeTextureBuildInput& Request)
			-> std::expected<std::unique_ptr<FVolumeTexturePlatformData>, FTextureBuildError> = 0;
	};
}
