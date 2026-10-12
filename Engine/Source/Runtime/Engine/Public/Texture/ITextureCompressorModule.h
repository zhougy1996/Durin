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
	enum class ETextureBuildKind : uint8 { Texture2D, TextureCube, VolumeTexture };

	// Fixed Developer module contract. Builds return detached CPU values only.
	class ITextureCompressorModule : public IModuleInterface
	{
	public:
		ENGINE_API auto StartupModule() -> void override;
		ENGINE_API auto ShutdownModule() -> void override;
		// Borrow the active implementation. Consumers drain work before editor shutdown.
		ENGINE_API static auto Get() -> ITextureCompressorModule*;
		// Nonzero opaque identity combining implementation, algorithm, and dependency versions.
		// Each kind has an independent identity, immutable for the editor lifetime; compare only for equality.
		// Unsupported kinds return zero.
		virtual auto GetBuildVersion(ETextureBuildKind TextureKind) const -> uint64 = 0;
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
