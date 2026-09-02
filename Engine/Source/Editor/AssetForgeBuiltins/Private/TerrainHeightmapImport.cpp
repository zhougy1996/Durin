#include "AssetForge/Builtins/TerrainHeightmapImport.h"
#include "AssetForge/Builtins/TerrainHeightmapFactory.h"
#include "Asset/AssetImportData.h"

#include "DObject/Package.h"
#include "EncodedSourceSnapshot.h"
#include "Image/ImageDecoder.h"
#include "Asset/PackageSerialization.h"
#include "Asset/SourceHint.h"
#include "Asset/Asset.h"
#include "DObject/DObjectGlobals.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/StringHelper.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainHeightmapDerivedData.h"
#include "Terrain/TerrainHeightmapBuildOperations.h"

namespace Durin::AssetForge::Builtins
{
	using namespace Durin;
	namespace
	{
		constexpr std::string_view Png16DecoderId = "DurinImage.Png16";
		constexpr std::string_view Raw16DecoderId = "DurinTerrainRaw16";
		constexpr uint32 TerrainSourceProfileVersion = 1;

		auto ResolveOwningPackagePhysicalPath(const DTerrainHeightmap& Heightmap,
			std::filesystem::path& OutPath, std::string& OutError) -> bool
		{
			if (!Heightmap.GetPackage())
			{
				OutError = "Terrain heightmap source capture requires an owning package.";
				return false;
			}
			const FAssetPathResult Resolved =
				FMountPaths::ResolveAssetPath(Heightmap.GetPackage()->GetPackagePath(),
					EMountPathExistence::AllowMissing);
			if (!Resolved) { OutError = Resolved.Message; return false; }
			OutPath = Resolved.PhysicalPath;
			OutPath += ".dasset";
			return true;
		}

		auto NormalizeExtension(std::string_view Extension) -> std::string
		{
			return StringUtils::FoldAscii(Extension);
		}

		auto IsSupportedHeightmapExtension(std::string_view Extension) -> bool
		{
			return Extension == ".png" || Extension == ".raw";
		}

		auto PublishImportData(DTerrainHeightmap& Heightmap,
			std::string Filename, ESourceHintBase HintBase,
			const std::filesystem::path& PhysicalPath,
			const FEncodedSourceSnapshot& Snapshot,
			std::string& OutError) -> bool
		{
			FAssetImportDataState State;
			State.SourceData.Sources.push_back({
				.Role = "source",
				.DisplayLabel = PhysicalPath.filename().generic_string(),
				.Hint = std::move(Filename),
				.HintBase = HintBase,
				.ContentHashLow = Snapshot.ContentHash.HashLow,
				.ContentHashHigh = Snapshot.ContentHash.HashHigh,
				.ByteCount = Snapshot.FileSize});
			auto* Data = Heightmap.GetAssetImportData();
			if (!Data) Data = NewObject<DAssetImportData>(
				&Heightmap, "AssetImportData");
			return Data && Data->SetState(std::move(State), OutError)
				&& Heightmap.PublishAssetImportData(*Data, OutError);
		}

		auto RebuildFromFilename(DTerrainHeightmap& Heightmap,
			std::string Filename, ESourceHintBase HintBase,
			std::string& OutError,
			const FAssetBundleSaveOptions* SaveOptions,
			std::optional<std::filesystem::path> SelectedPhysicalPath = {}) -> bool
		{
			std::filesystem::path OwningPackagePath;
			if (!ResolveOwningPackagePhysicalPath(Heightmap, OwningPackagePath, OutError))
				return false;
			std::filesystem::path PhysicalPath;
			if (SelectedPhysicalPath) PhysicalPath = std::move(*SelectedPhysicalPath);
			else
			{
				std::string PhysicalPathText;
				if (!ResolveSourceHint(HintBase, Filename,
					OwningPackagePath.generic_string(), PhysicalPathText, OutError)) return false;
				PhysicalPath = PhysicalPathText;
			}
			if (!std::filesystem::is_regular_file(PhysicalPath))
			{
				OutError = std::format("Terrain heightmap source file is missing: {}.", Filename);
				return false;
			}
			if (SelectedPhysicalPath && !MakeSourceHint(
				PhysicalPath.generic_string(), OwningPackagePath.generic_string(),
				HintBase, Filename, OutError)) return false;
			FEncodedSourceSnapshot Snapshot;
			if (!CaptureEncodedSource(Filename, PhysicalPath, Snapshot,
				OutError, MaximumTerrainHeightmapEncodedBytes)) return false;
			FTerrainHeightmapSourceData SourceData;
			if (!TranslateTerrainHeightmapSource(
				PhysicalPath.extension().generic_string(), Snapshot.GetBytes(),
				SourceData, OutError)) return false;
			const FTerrainHeightmapSourceData ImportState = SourceData;
			const std::shared_ptr<const FTerrainHeightmapPayload> Existing = Heightmap.GetPayload();
			const bool bSamplesChanged = !Existing || Existing->Samples != SourceData.Samples;
			if (!BuildTerrainHeightmapInto(Heightmap, {
					.Samples = std::move(SourceData.Samples),
					.Width = SourceData.Width, .Height = SourceData.Height,
					.SourceContentHashLow = Snapshot.ContentHash.HashLow,
					.SourceContentHashHigh = Snapshot.ContentHash.HashHigh,
					.DecoderId = SourceData.DecoderId,
					.DecoderVersion = SourceData.DecoderVersion,
					.SourceFormat = SourceData.SourceFormat,
					.SourceProfileVersion = SourceData.SourceProfileVersion}, {
						.SourceFilename = Snapshot.Filename,
						.DecoderId = ImportState.DecoderId,
						.DecoderVersion = ImportState.DecoderVersion,
						.SourceFormat = ImportState.SourceFormat,
						.SourceProfileVersion = ImportState.SourceProfileVersion,
						.bAdvanceRevision = bSamplesChanged}, OutError)
				|| !PublishImportData(Heightmap, std::move(Filename), HintBase, PhysicalPath,
					Snapshot, OutError)) return false;
			if (!SaveOptions) return true;
			DPackage* Package = Heightmap.GetPackage();
			const FAssetResult Saved = SavePackagesAtomically(
				std::span<DPackage* const>(&Package, 1), *SaveOptions);
			if (Saved) return true;
			OutError = Saved.Message;
			return false;
		}
	}

	DTerrainHeightmapFactory::DTerrainHeightmapFactory(
		const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		SupportedClass = DTerrainHeightmap::StaticClass();
		Formats = {"png", "raw"};
	}

	auto DTerrainHeightmapFactory::FactoryCreateFromFile(
		DClass* InClass,
		DObject* InParent,
		FName InName,
		EObjectFlags Flags,
		std::string_view Filename,
		DObject*,
		FFactoryDiagnostics* Diagnostics) const -> DObject*
	{
		auto Failed = [&](std::string Message) -> DObject* {
			if (Diagnostics) Diagnostics->Report(Message);
			return nullptr;
		};
		if (InClass != DTerrainHeightmap::StaticClass())
			return Failed("Terrain heightmap factory requires the exact TerrainHeightmap class.");
		auto* Package = Cast<DPackage>(InParent);
		if (!Package || !Package->IsAssetPackage())
			return Failed("Terrain heightmap factory requires an asset package parent.");
		const std::filesystem::path Input =
			std::filesystem::absolute(Filename).lexically_normal();
		if (!std::filesystem::is_regular_file(Input)
			|| !IsTerrainHeightmapSourceExtension(Input.extension().generic_string()))
			return Failed("Terrain heightmap import requires an existing .png or .raw source.");

		(void)Settings;
		auto* Heightmap = NewObject<DTerrainHeightmap>(
			InClass, Package, InName, Flags);
		if (!Heightmap)
			return Failed("Terrain heightmap object could not be created.");
		std::string Error;
		if (!RebuildFromFilename(
			*Heightmap, Input.generic_string(), ESourceHintBase::AssetRelative,
			Error, nullptr, Input)) return Failed(std::move(Error));
		return Heightmap;
	}

	auto DTerrainHeightmapFactory::GetReimportCapabilities(
		const DObject& Object) const -> FReimportCapabilities
	{
		const auto* Heightmap = Cast<DTerrainHeightmap>(&Object);
		if (!Heightmap || !Heightmap->GetPackage())
			return {.Diagnostic = "Only packaged TerrainHeightmap assets can be reimported."};
		const DAssetImportData* Data = Heightmap->GetAssetImportData();
		const FSourceFile* Source = Data
			? Data->GetSourceData().FindByRole("source") : nullptr;
		const bool bHasSource = Source && !Source->Hint.empty();
		return {.bCanReimport = bHasSource, .bCanReimportFromFile = true,
			.Diagnostic = bHasSource ? std::string{}
				: "TerrainHeightmap has no source hint to reimport."};
	}

	auto DTerrainHeightmapFactory::Reimport(
		DObject& Object, FReimportCompletion Completion) const -> void
	{
		auto* Heightmap = Cast<DTerrainHeightmap>(&Object);
		const DAssetImportData* Data = Heightmap ? Heightmap->GetAssetImportData() : nullptr;
		const FSourceFile* Source = Data
			? Data->GetSourceData().FindByRole("source") : nullptr;
		if (!Heightmap || !Source || Source->Hint.empty())
		{
			if (Completion) Completion({EReimportStatus::MissingSource,
				"TerrainHeightmap has no source hint to reimport."});
			return;
		}
		std::string Error;
		const bool bSucceeded = RebuildFromFilename(
			*Heightmap, Source->Hint, Source->HintBase, Error, nullptr);
		if (Completion) Completion(bSucceeded
			? FReimportResult{EReimportStatus::Succeeded, {}}
			: FReimportResult{EReimportStatus::SourceOrBuildFailure, std::move(Error)});
	}

	auto DTerrainHeightmapFactory::ReimportFromFiles(DObject& Object,
		std::span<const std::string> Filenames, FReimportCompletion Completion) const
		-> void
	{
		auto* Heightmap = Cast<DTerrainHeightmap>(&Object);
		if (!Heightmap || Filenames.size() != 1 || Filenames.front().empty())
		{
			if (Completion) Completion({EReimportStatus::SourceOrBuildFailure,
				"TerrainHeightmap reimport requires exactly one source file."});
			return;
		}
		const std::filesystem::path Requested =
			std::filesystem::absolute(Filenames.front()).lexically_normal();
		std::string Error;
		const bool bSucceeded = RebuildFromFilename(*Heightmap, {},
			ESourceHintBase::AssetRelative, Error, nullptr, Requested);
		if (Completion) Completion(bSucceeded
			? FReimportResult{EReimportStatus::Succeeded, {}}
			: FReimportResult{EReimportStatus::SourceOrBuildFailure, std::move(Error)});
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

	auto ReimportTerrainHeightmap(
		DTerrainHeightmap& Heightmap,
		std::string& OutError,
		const FAssetBundleSaveOptions& SaveOptions) -> bool
	{
		const DAssetImportData* ImportData = Heightmap.GetAssetImportData();
		const FSourceFile* Source = ImportData
			? ImportData->GetSourceData().FindByRole("source") : nullptr;
		if (!Source)
		{
			OutError = "Terrain heightmap has no source filename to reimport.";
			return false;
		}
		return RebuildFromFilename(
			Heightmap, Source->Hint, Source->HintBase, OutError, &SaveOptions);
	}

	auto ReimportTerrainHeightmapFromFile(DTerrainHeightmap& Heightmap,
		std::string_view FilePath, std::string& OutError,
		const FAssetBundleSaveOptions& SaveOptions) -> bool
	{
		const std::filesystem::path Requested =
			std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Requested))
		{
			OutError = "The selected Terrain heightmap source file does not exist.";
			return false;
		}
		return RebuildFromFilename(
			Heightmap, {}, ESourceHintBase::AssetRelative,
			OutError, &SaveOptions, Requested);
	}
}
