#include "Texture/ITextureBuildModule.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture/TextureCubeBuildOperations.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/VolumeTextureBuildOperations.h"

namespace Durin
{
	class FTextureBuildModule final : public ITextureBuildModule
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
		auto BuildTexture2D(const FTexture2DBuildInput& Request,
			const FTexture2DBuildControl* Control)
			-> std::expected<FTexture2DBuildOutput, FTexture2DBuildError> override
		{
			return Durin::BuildTexture2D(Request, Control);
		}
		auto NormalizeTextureCube(const FTextureCubeNormalizeRequest& Request)
			-> std::expected<FTextureCubeCanonicalBuildInput, FTextureBuildError> override
		{
			return Durin::NormalizeTextureCube(Request);
		}
		auto BuildTextureCube(const FTextureCubeBuildInput& Request)
			-> std::expected<std::unique_ptr<FTextureCubePlatformData>, FTextureBuildError> override
		{
			return Durin::BuildTextureCube(Request);
		}
		auto BuildVolumeTexture(const FVolumeTextureBuildInput& Request)
			-> std::expected<std::unique_ptr<FVolumeTexturePlatformData>, FTextureBuildError> override
		{
			return Durin::BuildVolumeTexture(Request);
		}
	};

	IMPLEMENT_MODULE(FTextureBuildModule, TextureBuild)
}
