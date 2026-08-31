#include "Thumbnail/TextureThumbnailRenderer.h"

#include "Asset.h"
#include "Texture/Texture2D.h"

namespace Durin::Editor::Texture
{
	auto DTextureThumbnailRenderer::GetRegistration() const
		-> ::Durin::Editor::FThumbnailRenderingInfo
	{
		return {
			.AssetClassName = DTexture2D::StaticClass()->GetQualifiedName().ToString(),
			.RendererName = "Texture2DSourceThumbnail",
			.GeneratorSchemaVersion = 1};
	}

	auto DTextureThumbnailRenderer::CaptureGenerationRequest(
		const ::Durin::Editor::FAssetThumbnailRequest& Request,
		uint64 RendererGeneration,
		::Durin::Editor::FAssetThumbnailGenerationRequest& OutRequest,
		std::string& OutError) -> bool
	{
		OutRequest = {};
		if (Request.Asset.AssetClassName != GetRegistration().AssetClassName)
		{
			OutError = "The Texture2D thumbnail renderer received the wrong asset class.";
			return false;
		}
		const Asset::FTopLevelAssetCatalogEntry Entry =
			Asset::FindTopLevelAssetExact(Request.Asset.AssetPath);
		if (!Entry || Entry->AssetClassName != Request.Asset.AssetClassName
			|| Entry.Package->FormatVersion != Request.Asset.PackageFormatVersion
			|| static_cast<uint64>(Entry.Package->FileSize) != Request.Asset.FileSize
			|| Entry.Package->LastWriteTimeTicks != Request.Asset.LastWriteTimeTicks)
		{
			OutError = "Texture2D thumbnail registry data is missing or changed.";
			return false;
		}
		DObject* Loaded = nullptr;
		const Asset::FAssetResult Load = Asset::LoadObject(Request.Asset.AssetPath, Loaded);
		auto* Texture = Load ? Cast<DTexture2D>(Loaded) : nullptr;
		const FTextureSourceData* Source = Texture ? Texture->GetSourceData() : nullptr;
		if (!Texture || !Source || !Source->IsValid())
		{
			OutError = Load ? "Texture2D canonical source pixels are unavailable." : Load.Message;
			return false;
		}

		constexpr uint32 OutputSize = 256;
		auto Generated = std::make_shared<::Durin::Editor::FAssetThumbnailGeneratedPixels>();
		Generated->Width = OutputSize;
		Generated->Height = OutputSize;
		Generated->Pixels.assign(static_cast<size_t>(OutputSize) * OutputSize * 4, std::byte{0});
		const double Scale = std::min(
			static_cast<double>(OutputSize) / Source->Width,
			static_cast<double>(OutputSize) / Source->Height);
		const uint32 DrawWidth = std::max(1u,
			static_cast<uint32>(std::floor(Source->Width * Scale)));
		const uint32 DrawHeight = std::max(1u,
			static_cast<uint32>(std::floor(Source->Height * Scale)));
		const uint32 OffsetX = (OutputSize - DrawWidth) / 2;
		const uint32 OffsetY = (OutputSize - DrawHeight) / 2;
		for (uint32 Y = 0; Y < DrawHeight; ++Y)
			for (uint32 X = 0; X < DrawWidth; ++X)
			{
				const uint32 SourceX = std::min(Source->Width - 1,
					static_cast<uint32>((static_cast<uint64>(X) * Source->Width) / DrawWidth));
				const uint32 SourceY = std::min(Source->Height - 1,
					static_cast<uint32>((static_cast<uint64>(Y) * Source->Height) / DrawHeight));
				const size_t SourcePixel =
					(static_cast<size_t>(SourceY) * Source->Width + SourceX) * 4;
				const size_t OutputPixel =
					(static_cast<size_t>(OffsetY + Y) * OutputSize + OffsetX + X) * 4;
				std::copy_n(Source->Pixels.begin() + static_cast<ptrdiff_t>(SourcePixel),
					4, Generated->Pixels.begin() + static_cast<ptrdiff_t>(OutputPixel));
			}
		const FXxHash128 Identity = Texture->GetImportedDataIdentity();
		Generated->AssetRevision = Identity.HashLow ^ Identity.HashHigh;
		if (Generated->AssetRevision == 0) Generated->AssetRevision = 1;
		OutRequest.KeyInput.Asset = Request.Asset;
		OutRequest.KeyInput.RendererName = GetRegistration().RendererName;
		OutRequest.KeyInput.GeneratorSchemaVersion = GetRegistration().GeneratorSchemaVersion;
		OutRequest.KeyInput.Output = {.Width = OutputSize, .Height = OutputSize};
		OutRequest.KeyInput.PreviewFixtureIdentity = Identity.ToString();
		OutRequest.KeyInput.PreviewFixtureVersion = 1;
		OutRequest.GeneratedPixels = std::move(Generated);
		OutRequest.RendererGeneration = RendererGeneration;
		OutRequest.RequestSerial = Request.RequestSerial;
		OutRequest.bHasTransparency = Source->bHasTransparency;
		OutRequest.AssetRevision = OutRequest.GeneratedPixels->AssetRevision;
		OutError.clear();
		return true;
	}

} // namespace Durin::Editor::Texture
