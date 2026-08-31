#include "TerrainHeightmapThumbnailRenderer.h"

#include "Asset/Asset.h"
#include "DObject/Class.h"
#include "Terrain/TerrainHeightmap.h"

namespace Durin::Editor::Level
{
	namespace
	{
		constexpr uint32 GeneratorSchemaVersion = 1;

		auto MakeFingerprint(const Asset::FAssetData& Data,
			FTopLevelAssetPath AssetPath = {})
			-> ::Durin::Editor::FAssetThumbnailPackageFingerprint
		{
			return {.AssetPath = std::move(AssetPath),
				.PackagePath = Data.PackagePath,
				.AssetClassName = Data.AssetClassName,
				.PackageFormatVersion = Data.FormatVersion,
				.FileSize = static_cast<uint64>(Data.FileSize),
				.LastWriteTimeTicks = Data.LastWriteTimeTicks};
		}
	}

	auto GenerateTerrainHeightmapThumbnailPixels(
		const FTerrainHeightmapPayload& Payload,
		FByteArray& OutPixels,
		std::string& OutError) -> bool
	{
		OutPixels.clear();
		OutError.clear();
		if (!Payload.IsValid() || Payload.Width < 2 || Payload.Height < 2)
		{
			OutError = "Terrain thumbnail generation requires a valid canonical payload.";
			return false;
		}
		constexpr uint32 Size = TerrainHeightmapThumbnailDimension;
		OutPixels.assign(static_cast<size_t>(Size) * Size * 4, std::byte{255});
		const double Scale = std::min(static_cast<double>(Size) / Payload.Width,
			static_cast<double>(Size) / Payload.Height);
		const uint32 DrawWidth = std::max(1u, static_cast<uint32>(std::floor(Payload.Width * Scale)));
		const uint32 DrawHeight = std::max(1u, static_cast<uint32>(std::floor(Payload.Height * Scale)));
		const uint32 OffsetX = (Size - DrawWidth) / 2;
		const uint32 OffsetY = (Size - DrawHeight) / 2;
		std::ranges::fill(OutPixels, std::byte{24});
		for (size_t Index = 3; Index < OutPixels.size(); Index += 4) OutPixels[Index] = std::byte{255};
		const uint32 Range = static_cast<uint32>(Payload.Maximum) - Payload.Minimum;
		for (uint32 Y = 0; Y < DrawHeight; ++Y)
			for (uint32 X = 0; X < DrawWidth; ++X)
			{
				const uint32 SourceX = DrawWidth == 1 ? 0 : static_cast<uint32>(
					(static_cast<uint64>(X) * (Payload.Width - 1)) / (DrawWidth - 1));
				const uint32 SourceY = DrawHeight == 1 ? 0 : static_cast<uint32>(
					(static_cast<uint64>(Y) * (Payload.Height - 1)) / (DrawHeight - 1));
				const uint16 Sample = Payload.Samples[static_cast<size_t>(SourceY) * Payload.Width + SourceX];
				const uint8 Gray = Range == 0 ? 127 : static_cast<uint8>(
					(static_cast<uint32>(Sample - Payload.Minimum) * 255u) / Range);
				const size_t Pixel = (static_cast<size_t>(OffsetY + Y) * Size + OffsetX + X) * 4;
				OutPixels[Pixel] = static_cast<std::byte>(Gray);
				OutPixels[Pixel + 1] = static_cast<std::byte>(Gray);
				OutPixels[Pixel + 2] = static_cast<std::byte>(Gray);
				OutPixels[Pixel + 3] = std::byte{255};
			}
		// A fixed white L marks the canonical top-left without changing sample orientation.
		for (uint32 I = 0; I < std::min(16u, std::min(DrawWidth, DrawHeight)); ++I)
		{
			for (const auto [X, Y] : std::array<std::pair<uint32, uint32>, 2>{
				std::pair{OffsetX + I, OffsetY}, std::pair{OffsetX, OffsetY + I}})
			{
				const size_t Pixel = (static_cast<size_t>(Y) * Size + X) * 4;
				OutPixels[Pixel] = OutPixels[Pixel + 1] = OutPixels[Pixel + 2] = std::byte{255};
			}
		}
		return true;
	}

	auto DTerrainHeightmapThumbnailRenderer::GetRegistration() const
		-> ::Durin::Editor::FThumbnailRenderingInfo
	{
		return {.AssetClassName = DTerrainHeightmap::StaticClass()->GetQualifiedName().ToString(),
			.RendererName = "TerrainHeightmapCanonicalThumbnail",
			.GeneratorSchemaVersion = GeneratorSchemaVersion};
	}

	auto DTerrainHeightmapThumbnailRenderer::CaptureGenerationRequest(
		const ::Durin::Editor::FAssetThumbnailRequest& Request,
		uint64 RendererGeneration,
		::Durin::Editor::FAssetThumbnailGenerationRequest& OutRequest,
		std::string& OutError) -> bool
	{
		OutRequest = {};
		OutError.clear();
		if (Request.Asset.AssetClassName != GetRegistration().AssetClassName)
		{
			OutError = "The Terrain thumbnail renderer received the wrong asset class.";
			return false;
		}
		const Asset::FAssetCatalogEntry Entry =
			Asset::FindAssetExact(Request.Asset.PackagePath);
		const Asset::FAssetData* Data = Entry.Data ? &*Entry.Data : nullptr;
		if (!Data || MakeFingerprint(*Data, Request.Asset.AssetPath) != Request.Asset)
		{
			OutError = "Terrain thumbnail registry data is missing or changed.";
			return false;
		}
		DObject* Loaded = nullptr;
		const Asset::FAssetResult LoadResult = Asset::LoadObject(Request.Asset.AssetPath, Loaded);
		auto* Heightmap = LoadResult ? Cast<DTerrainHeightmap>(Loaded) : nullptr;
		const std::shared_ptr<const FTerrainHeightmapPayload> Payload = Heightmap ? Heightmap->GetPayload() : nullptr;
		if (!Heightmap || Heightmap->GetStatus() != ETerrainHeightmapStatus::Ready || !Payload)
		{
			OutError = LoadResult ? "Terrain heightmap payload is unavailable." : LoadResult.Message;
			return false;
		}
		auto Generated = std::make_shared<::Durin::Editor::FAssetThumbnailGeneratedPixels>();
		Generated->Width = TerrainHeightmapThumbnailDimension;
		Generated->Height = TerrainHeightmapThumbnailDimension;
		Generated->AssetRevision = Heightmap->GetRevision();
		if (!GenerateTerrainHeightmapThumbnailPixels(*Payload, Generated->Pixels, OutError)) return false;
		OutRequest.KeyInput.Output = {.Width = Generated->Width, .Height = Generated->Height};
		OutRequest.KeyInput.PreviewFixtureIdentity = std::format(
			"TerrainHeightmapPayload/{}", Heightmap->GetRevision());
		OutRequest.KeyInput.PreviewFixtureVersion = GeneratorSchemaVersion;
		OutRequest.GeneratedPixels = std::move(Generated);
		OutRequest.RendererGeneration = RendererGeneration;
		OutRequest.RequestSerial = Request.RequestSerial;
		OutRequest.bHasTransparency = false;
		OutRequest.AssetRevision = Heightmap->GetRevision();
		return true;
	}
}
