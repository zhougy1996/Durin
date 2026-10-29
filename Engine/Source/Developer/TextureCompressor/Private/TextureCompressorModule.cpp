#include "Texture/ITextureCompressorModule.h"
#include "Texture/Texture2DBuildOperations.h"
#include "Texture/TextureCubeBuildOperations.h"
#include "Texture/TextureBuildVersion.h"
#include "Serialization/BinaryFormat.h"
#include "Texture/VolumeTextureBuildOperations.h"
#include <glm/detail/setup.hpp>

namespace Durin
{
	class FTextureCompressorModule final : public ITextureCompressorModule
	{
	public:
		// Resident until normal editor shutdown; dynamic reloading is unsupported.

		auto GetBuildVersion(ETextureBuildKind TextureKind) const -> uint64 override
		{
			using namespace TextureCompressorPrivate;
			static const auto Versions = [] {
				auto Make = [](std::string_view Implementation, uint32 Revision, bool bCompressed, bool bCube) {
					FBinaryWriter Writer;
					Writer.WriteString(Implementation);
					Writer.WriteU32(Revision);
					if (bCompressed)
					{
						Writer.WriteU32(ColorRevision);
						Writer.WriteU32(MipRevision);
						Writer.WriteString(DURIN_TEXTURE_ENCODER_REVISION);
						Writer.WriteU32(GLM_VERSION_MAJOR);
						Writer.WriteU32(GLM_VERSION_MINOR);
						Writer.WriteU32(GLM_VERSION_PATCH);
					}
					if (bCube) Writer.WriteU32(CubeProjectionRevision);
					return FXxHash64::HashBuffer(Writer.TakeBytes()).HashValue;
				};
				return std::array{
					Make("Durin.TextureCompressor.Texture2D", Texture2DAlgorithmRevision, true, false),
					Make("Durin.TextureCompressor.TextureCube", TextureCubeAlgorithmRevision, true, true),
					Make("Durin.TextureCompressor.VolumeTexture", VolumeTextureAlgorithmRevision, false, false)};
			}();
			switch (TextureKind)
			{
			case ETextureBuildKind::Texture2D: return Versions[0];
			case ETextureBuildKind::TextureCube: return Versions[1];
			case ETextureBuildKind::VolumeTexture: return Versions[2];
			}
			return 0;
		}
		auto BuildTexture2D(const FTexture2DBuildInput& Request)
			-> std::optional<FTexturePlatformData> override
		{
			return Durin::BuildTexture2D(Request);
		}
		auto NormalizeTextureCube(const FTextureCubeNormalizeRequest& Request)
			-> std::expected<FTextureCubeCanonicalBuildInput, FTextureBuildOperationError> override
		{
			auto Normalized = Durin::NormalizeTextureCube(Request);
			if (!Normalized) return std::unexpected(FTextureBuildOperationError{ETextureBuildOperationFailure::InvalidInput, std::move(Normalized.error().Diagnostic)});
			return std::move(*Normalized);
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
