#include "Texture/ITextureCompressorModule.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture/TextureCubeBuildOperations.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/VolumeTextureBuildOperations.h"

namespace Durin
{
	class FTextureCompressorModule final : public ITextureCompressorModule
	{
	public:
		// Resident until normal editor shutdown; dynamic reloading is unsupported.

		auto GetTexture2DBuilderVersion() const -> uint32 override
		{
			return Texture2DBuilderVersion;
		}
		auto GetTextureCubeBuilderVersion() const -> uint32 override
		{
			return TextureCubeBuilderVersion;
		}
		auto GetTextureCubeProjectionVersion() const -> uint32 override
		{
			return TextureCubeProjectionVersion;
		}
		auto GetVolumeTextureBuilderVersion() const -> uint32 override
		{
			return VolumeTextureBuilderVersion;
		}
		auto BuildTexture2D(const FTexture2DBuildInput& Request)
			-> std::optional<FTexture2DBuildOutput> override
		{
			return Durin::BuildTexture2D(Request);
		}
		auto NormalizeTextureCube(const FTextureCubeNormalizeRequest& Request)
			-> std::expected<FTextureCubeCanonicalBuildInput, FTextureBuildError> override
		{
			return Durin::NormalizeTextureCube(Request);
		}
		auto BuildTextureCube(const FTextureCubeBuildInput& Request)
			-> std::optional<FTextureCubePlatformData> override
		{
			return Durin::BuildTextureCube(Request);
		}
		auto BuildVolumeTexture(const FVolumeTextureBuildInput& Request)
			-> std::optional<FVolumeTexturePlatformData> override
		{
			return Durin::BuildVolumeTexture(Request);
		}
	};

	IMPLEMENT_MODULE(FTextureCompressorModule, TextureCompressor)
}
