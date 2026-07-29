#include "Thumbnail/TextureCubeAssetThumbnail.h"

#include "AssetSystem.h"
#include "Texture/TextureCube.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 TextureCubeThumbnailGeneratorSchema = 2;
		constexpr uint32 TextureCubeThumbnailShaderContract = 2;

		auto MakeFingerprint(const Asset::FAssetData& Data)
			-> FAssetThumbnailPackageFingerprint
		{
			return {
				.VirtualPath = Data.PackagePath,
				.AssetClassName = Data.AssetClassName,
				.PackageFormatVersion = Data.FormatVersion,
				.FileSize = static_cast<uint64>(Data.FileSize),
				.LastWriteTimeTicks = Data.LastWriteTimeTicks};
		}
	} // namespace

	auto FTextureCubeAssetThumbnailProvider::GetRegistration() const
		-> FAssetThumbnailProviderRegistration
	{
		return {
			.AssetClassName = DTextureCube::StaticClass()->GetQualifiedName().ToString(),
			.ProviderName = "TextureCubeRenderedThumbnail",
			.GeneratorSchemaVersion = TextureCubeThumbnailGeneratorSchema};
	}

	auto FTextureCubeAssetThumbnailProvider::CaptureGenerationRequest(
		const FAssetThumbnailRequest& Request,
		uint64 ProviderGeneration,
		FAssetThumbnailGenerationRequest& OutRequest,
		std::string& OutError) -> bool
	{
		OutRequest = {};
		OutError.clear();
		const FAssetThumbnailProviderRegistration Registration = GetRegistration();
		if (Request.Asset.AssetClassName != Registration.AssetClassName)
		{
			OutError = "The TextureCube thumbnail provider received the wrong asset class.";
			return false;
		}
		const Asset::FAssetData* Data =
			Asset::GetAssetRegistry().FindAsset(Request.Asset.VirtualPath);
		if (Data == nullptr)
		{
			OutError = std::format(
				"TextureCube thumbnail registry data is missing for {}.",
				Request.Asset.VirtualPath.ToString());
			return false;
		}
		if (MakeFingerprint(*Data) != Request.Asset)
		{
			OutError = std::format(
				"TextureCube thumbnail registry data changed for {}; refresh the request snapshot.",
				Request.Asset.VirtualPath.ToString());
			return false;
		}

		const FRenderedAssetThumbnailVisualContract Visual;
		OutRequest.KeyInput = {
			.Output = Visual.Output,
			.PreviewFixtureIdentity =
				std::string(
					FRenderedAssetThumbnailVisualContract::
						TextureCubeEnvironmentViewIdentity),
			.PreviewFixtureVersion =
				FRenderedAssetThumbnailVisualContract::
					TextureCubeEnvironmentViewVersion,
			.ShaderContractVersion = TextureCubeThumbnailShaderContract};
		OutRequest.Input =
			std::make_shared<FTextureCubeThumbnailGenerationInput>(
				Request.Asset.VirtualPath);
		OutRequest.ProviderGeneration = ProviderGeneration;
		OutRequest.RequestSerial = Request.RequestSerial;
		return true;
	}
} // namespace Durin
