#pragma once

#include "CoreMinimal.h"

#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

#include "Modules/ModuleManager.h"
#include "Texture/Texture2DBuildTypes.h"
#include "Texture/TextureCubeBuildTypes.h"
#include "Texture/VolumeTextureBuildTypes.h"

namespace Durin
{
	// Fixed Developer module contract. Builds return detached CPU values only.
	class ITextureCompressorModule : public IModuleInterface
	{
	public:
		ENGINE_API auto StartupModule() -> void override;
		// Borrow the active implementation. Consumers drain work before editor shutdown.
		ENGINE_API static auto Get() -> ITextureCompressorModule*;
		virtual auto GetTexture2DBuilderVersion() const -> uint32 = 0;
		virtual auto GetTextureCubeBuilderVersion() const -> uint32 = 0;
		virtual auto GetTextureCubeProjectionVersion() const -> uint32 = 0;
		virtual auto GetVolumeTextureBuilderVersion() const -> uint32 = 0;
		// Synchronous and non-cancelable. Failure is logged by TextureCompressor.
		virtual auto BuildTexture2D(const FTexture2DBuildInput& Request)
			-> std::optional<FTexture2DBuildOutput> = 0;
		virtual auto NormalizeTextureCube(const FTextureCubeNormalizeRequest& Request)
			-> std::expected<FTextureCubeCanonicalBuildInput, FTextureBuildError> = 0;
		// Synchronous and non-cancelable. Failure is logged by TextureCompressor.
		virtual auto BuildTextureCube(const FTextureCubeBuildInput& Request)
			-> std::optional<FTextureCubePlatformData> = 0;
		// Synchronous and non-cancelable. Failure is logged by TextureCompressor.
		virtual auto BuildVolumeTexture(const FVolumeTextureBuildInput& Request)
			-> std::optional<FVolumeTexturePlatformData> = 0;
	};
}

#endif
