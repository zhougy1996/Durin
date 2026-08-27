#include "AssetForge/Builtins/TerrainHeightmapImport.h"
#include "AssetForge/Builtins/TerrainHeightmapImportData.h"

#include "DObject/Package.h"
#include "EncodedSourceSnapshot.h"
#include "Image/ImageDecoder.h"
#include "Asset/AssetOperations.h"
#include "Asset/SourceFilename.h"
#include "Asset/PackageSerialization.h"
#include "Asset.h"
#include "DObject/DObjectGlobals.h"
#include "Misc/Paths.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainHeightmapDerivedData.h"
#include "Terrain/TerrainHeightmapBuildOperations.h"

namespace Durin::AssetForge::Builtins
{
	using namespace Durin::Asset;
	namespace
	{
		constexpr std::string_view Png16DecoderId = "DurinImage.Png16";
		constexpr std::string_view Raw16DecoderId = "DurinTerrainRaw16";
		constexpr uint32 TerrainSourceProfileVersion = 1;

		auto NormalizeExtension(std::string_view Extension) -> std::string
		{
			std::string Result(Extension);
			std::ranges::transform(Result, Result.begin(), [](unsigned char Character) {
				return static_cast<char>(std::tolower(Character));
			});
			return Result;
		}

		auto IsSupportedHeightmapExtension(std::string_view Extension) -> bool
		{
			return Extension == ".png" || Extension == ".raw";
		}

		auto PublishImportData(DTerrainHeightmap& Heightmap,
			std::string Filename, const std::filesystem::path& PhysicalPath,
			const FEncodedSourceSnapshot& Snapshot,
			const FTerrainHeightmapSourceData& SourceData,
			std::string& OutError) -> bool
		{
			FTerrainHeightmapImportDataState State;
			State.SourceData.Sources.push_back({
				.StableIdentity = "root", .Role = "source",
				.DisplayLabel = PhysicalPath.filename().generic_string(),
				.Filename = std::move(Filename),
				.ContentHashLow = Snapshot.ContentHash.HashLow,
				.ContentHashHigh = Snapshot.ContentHash.HashHigh,
				.ByteCount = Snapshot.FileSize,
				.LastWriteTime = Snapshot.LastWriteTime});
			State.DecoderId = SourceData.DecoderId;
			State.DecoderVersion = SourceData.DecoderVersion;
			State.SourceFormat = SourceData.SourceFormat;
			State.SourceProfileVersion = SourceData.SourceProfileVersion;
			auto* Data = dynamic_cast<DTerrainHeightmapImportData*>(
				Heightmap.GetAssetImportData());
			if (!Data) Data = NewObject<DTerrainHeightmapImportData>(
				&Heightmap, "AssetImportData");
			return Data && Data->SetState(std::move(State), OutError)
				&& Heightmap.PublishAssetImportData(*Data, OutError);
		}

		auto RebuildFromFilename(DTerrainHeightmap& Heightmap,
			std::string Filename, std::string& OutError,
			const Asset::FAssetBundleSaveOptions* SaveOptions) -> bool
		{
			std::string PhysicalPathText;
			if (!AssetImport::ResolveSourceFilename(
				Filename, PhysicalPathText, OutError)) return false;
			const std::filesystem::path PhysicalPath(PhysicalPathText);
			if (!std::filesystem::is_regular_file(PhysicalPath))
			{
				OutError = std::format("Terrain heightmap source file is missing: {}.", Filename);
				return false;
			}
			FEncodedSourceSnapshot Snapshot;
			if (!CaptureEncodedSource({.Path = Filename}, PhysicalPath, Snapshot,
				OutError, MaximumTerrainHeightmapEncodedBytes)) return false;
			FTerrainHeightmapSourceData SourceData;
			if (!TranslateTerrainHeightmapSource(
				PhysicalPath.extension().generic_string(), Snapshot.GetBytes(),
				SourceData, OutError)) return false;
			const FTerrainHeightmapSourceData ImportState = SourceData;
			const std::shared_ptr<const FTerrainHeightmapPayload> Existing = Heightmap.GetPayload();
			const bool bSamplesChanged = !Existing || Existing->Samples != SourceData.Samples;
			Asset::FTerrainHeightmapBuildProduct Product;
			if (!Asset::BuildTerrainHeightmap({
					.Samples = std::move(SourceData.Samples),
					.Width = SourceData.Width, .Height = SourceData.Height,
					.SourceContentHashLow = Snapshot.ContentHash.HashLow,
					.SourceContentHashHigh = Snapshot.ContentHash.HashHigh,
					.DecoderId = SourceData.DecoderId,
					.DecoderVersion = SourceData.DecoderVersion,
					.SourceFormat = SourceData.SourceFormat,
					.SourceProfileVersion = SourceData.SourceProfileVersion},
					Product, OutError)
				|| !Asset::PublishTerrainHeightmapProduct(
					Heightmap, std::move(Product), {
						.SourcePath = Snapshot.SourcePath,
						.DecoderId = ImportState.DecoderId,
						.DecoderVersion = ImportState.DecoderVersion,
						.SourceFormat = ImportState.SourceFormat,
						.SourceProfileVersion = ImportState.SourceProfileVersion,
						.SourceFileSize = Snapshot.FileSize,
						.SourceLastWriteTime = Snapshot.LastWriteTime,
						.bAdvanceRevision = bSamplesChanged}, OutError)
				|| !PublishImportData(Heightmap, std::move(Filename), PhysicalPath,
					Snapshot, ImportState, OutError)) return false;
			if (!SaveOptions) return true;
			DPackage* Package = Heightmap.GetPackage();
			const Asset::FAssetResult Saved = Asset::SavePackagesAtomically(
				std::span<DPackage* const>(&Package, 1), *SaveOptions);
			if (Saved) return true;
			OutError = Saved.Message;
			return false;
		}
	}

	auto IsTerrainHeightmapSourceExtension(std::string_view Extension) -> bool
	{
		return IsSupportedHeightmapExtension(NormalizeExtension(Extension));
	}

	auto TranslateTerrainHeightmapSource(
		std::string_view Extension,
		std::span<const std::byte> EncodedBytes,
		FTerrainHeightmapSourceData& OutSource,
		std::string& OutError) -> bool
	{
		OutSource = {};
		const std::string NormalizedExtension = NormalizeExtension(Extension);
		if (NormalizedExtension == ".png")
		{
			Image::FDecodedGrayscale16Image Decoded;
			if (!Image::DecodeGrayscale16PngFromMemory(EncodedBytes, Decoded, OutError, {
				.MaximumEncodedBytes = MaximumTerrainHeightmapEncodedBytes,
				.MaximumDecodedPixels = MaximumTerrainHeightmapSamples})) return false;
			OutSource = {
				.Samples = std::move(Decoded.Samples),
				.Width = Decoded.Width,
				.Height = Decoded.Height,
				.DecoderId = std::string(Png16DecoderId),
				.DecoderVersion = 1,
				.SourceFormat = ETerrainHeightmapSourceFormat::Png16,
				.SourceProfileVersion = TerrainSourceProfileVersion};
			if (OutSource.IsValid())
			{
				OutError.clear();
				return true;
			}
			OutSource = {};
			OutError = "Decoded terrain heightmap source is invalid.";
			return false;
		}
		if (NormalizedExtension != ".raw")
		{
			OutError = "Terrain heightmap source extension must be .png or .raw.";
			return false;
		}
		if (EncodedBytes.size() > MaximumTerrainHeightmapEncodedBytes)
		{
			OutError = "RAW16 terrain heightmap exceeds the 512 MiB encoded-source limit.";
			return false;
		}
		if (EncodedBytes.size() < 8)
		{
			OutError = "RAW16 terrain heightmap must contain at least four samples (8 bytes).";
			return false;
		}
		if ((EncodedBytes.size() & 1u) != 0)
		{
			OutError = "RAW16 terrain heightmap byte count must be even.";
			return false;
		}
		const uint64 SampleCount = EncodedBytes.size() / sizeof(uint16);
		uint64 Low = 2;
		uint64 High = MaximumTerrainHeightmapDimension;
		uint64 Dimension = 0;
		while (Low <= High)
		{
			const uint64 Middle = Low + (High - Low) / 2;
			const uint64 Square = Middle * Middle;
			if (Square == SampleCount) { Dimension = Middle; break; }
			if (Square < SampleCount) Low = Middle + 1;
			else High = Middle - 1;
		}
		if (Dimension == 0)
		{
			OutError = "RAW16 terrain heightmap sample count must be an exact square within dimensions 2..16384.";
			return false;
		}
		if (Dimension > std::numeric_limits<size_t>::max() / Dimension
			|| Dimension * Dimension != SampleCount)
		{
			OutError = "RAW16 terrain heightmap dimensions overflow checked sample arithmetic.";
			return false;
		}
		OutSource.Samples.resize(static_cast<size_t>(SampleCount));
		for (size_t Index = 0; Index < OutSource.Samples.size(); ++Index)
		{
			const size_t ByteOffset = Index * 2;
			OutSource.Samples[Index] = static_cast<uint16>(
				static_cast<uint16>(EncodedBytes[ByteOffset])
					| (static_cast<uint16>(EncodedBytes[ByteOffset + 1]) << 8));
		}
		OutSource.Width = static_cast<uint32>(Dimension);
		OutSource.Height = static_cast<uint32>(Dimension);
		OutSource.DecoderId = Raw16DecoderId;
		OutSource.DecoderVersion = 1;
		OutSource.SourceFormat = ETerrainHeightmapSourceFormat::Raw16;
		OutSource.SourceProfileVersion = TerrainSourceProfileVersion;
		OutError.clear();
		return true;
	}

	auto ImportTerrainHeightmapAsset(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FTerrainHeightmapImportSettings& Settings,
		bool bAllowEngineContentWrite) -> FTerrainHeightmapImportResult
	{
		(void)bAllowEngineContentWrite;
		(void)Settings;
		const std::filesystem::path Input = std::filesystem::absolute(FilePath).lexically_normal();
		std::string Extension = Input.extension().generic_string();
		Extension = NormalizeExtension(Extension);
		if (!std::filesystem::is_regular_file(Input) || !IsTerrainHeightmapSourceExtension(Extension))
			return {false, "Terrain heightmap import requires an existing .png or .raw source.", nullptr};
		FAssetPath ParsedPath;
		std::string Error;
		if (!FAssetPath::TryCreate(AssetPath, ParsedPath, &Error))
			return {false, std::move(Error), nullptr};
		if (Asset::FindAssetExact(ParsedPath)
			|| Asset::FindResidentPackage(ParsedPath))
			return {false, std::format("Asset {} already exists.", ParsedPath.ToString()), nullptr};
		std::string Filename;
		if (!AssetImport::MakeSourceFilename(
			Input.generic_string(), Filename, Error))
			return {false, std::move(Error), nullptr};
		DTerrainHeightmap* Heightmap = nullptr;
		const Asset::FAssetResult Created = Asset::CreateAsset(ParsedPath, Heightmap);
		if (!Created || !Heightmap)
			return {false, Created.Message.empty()
				? "Terrain heightmap destination could not be created." : Created.Message, nullptr};
		auto Abandon = [&] {
			(void)Asset::UnloadPackage(
				ParsedPath, Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
		};
		if (!RebuildFromFilename(*Heightmap, std::move(Filename), Error, nullptr))
		{
			Abandon();
			return {false, std::move(Error), nullptr};
		}
		const Asset::FAssetResult Saved = Asset::SavePackage(Heightmap->GetPackage());
		if (!Saved) return {false, Saved.Message, Heightmap};
		return {true, {}, Heightmap};
	}


	auto ReimportTerrainHeightmapSource(
		DTerrainHeightmap& Heightmap,
		std::string& OutError,
		const Asset::FAssetBundleSaveOptions& SaveOptions) -> bool
	{
		const AssetImport::FSourceFile* Source = Heightmap.GetImportedSource();
		if (!Source)
		{
			OutError = "Terrain heightmap has no source filename to reimport.";
			return false;
		}
		return RebuildFromFilename(
			Heightmap, Source->Filename, OutError, &SaveOptions);
	}
}
