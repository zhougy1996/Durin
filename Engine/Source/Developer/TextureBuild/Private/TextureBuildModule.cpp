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
		auto GetTexture2DDescriptor() const -> FTexture2DBuildDescriptor override
		{
			return {.ProducerIdentity = "Durin.TextureBuild.Texture2D", .BuilderVersion = Texture2DBuilderVersion};
		}
		auto GetTextureCubeDescriptor() const -> FTextureCubeBuildDescriptor override
		{
			return {.ProducerIdentity = "Durin.TextureBuild.TextureCube", .BuilderVersion = TextureCubeBuilderVersion,
				.ProjectionVersion = TextureCubeProjectionVersion};
		}
		auto GetVolumeTextureDescriptor() const -> FVolumeTextureBuildDescriptor override
		{
			return {.ProducerIdentity = "Durin.TextureBuild.VolumeTexture", .BuilderVersion = VolumeTextureBuilderVersion};
		}
		auto BuildTexture2D(const FTexture2DRecipeBuildRequest& Request,
			const FTexture2DRecipeExecutionControl* Control)
			-> std::expected<FTexture2DRecipeBuildProduct, FTexture2DBuildError> override
		{
			return Durin::BuildTexture2D(Request, Control);
		}
		auto NormalizeTextureCube(const FTextureCubeBuildRequest& Request)
			-> std::expected<FTextureCubeCanonicalBuildInput, FTextureBuildError> override
		{
			return Durin::NormalizeTextureCube(Request);
		}
		auto BuildTextureCube(const FTextureCubeRecipeBuildRequest& Request)
			-> std::expected<FTextureCubeRecipeBuildProduct, FTextureBuildError> override
		{
			return Durin::BuildTextureCube(Request);
		}
		auto BuildVolumeTexture(const FVolumeTextureRecipeBuildRequest& Request)
			-> std::expected<FVolumeTextureRecipeBuildProduct, FTextureBuildError> override
		{
			return Durin::BuildVolumeTexture(Request);
		}
	};

	IMPLEMENT_MODULE(FTextureBuildModule, TextureBuild)
}
