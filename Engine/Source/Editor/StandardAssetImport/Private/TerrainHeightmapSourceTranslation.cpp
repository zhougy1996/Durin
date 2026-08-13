#include "TerrainHeightmapSourceTranslation.h"

#include "Hash/XxHash.h"
#include "ImageDecoder.h"
#include "AssetSystem.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Asset/MountedSource.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainHeightmapBuildOperations.h"
#include "Terrain/TerrainHeightmapDerivedData.h"

namespace Durin::Asset::Import
{
	namespace
	{
		constexpr std::string_view DefaultHeightmapSourceRoot = "TerrainHeightmaps";
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

		auto MakeCanonicalSourceLocation(
			const FAssetPath& AssetPath,
			std::string_view RequestedSourcePath,
			std::string_view SourceExtension,
			std::string& OutStoredPath,
			std::string& OutError) -> bool
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(AssetPath.ToString());
			if (!Lookup)
			{
				OutError = std::format("Terrain heightmap asset {} is outside a registered mount.",
					AssetPath.ToString());
				return false;
			}
			std::filesystem::path Relative = RequestedSourcePath.empty()
				? std::filesystem::path(DefaultHeightmapSourceRoot)
					/ (std::string(AssetPath.GetAssetName()) + std::string(SourceExtension))
				: std::filesystem::path(RequestedSourcePath);
			if (RequestedSourcePath.starts_with('/'))
			{
				const PathUtilities::FSourcePathResult Requested = PathUtilities::ResolveSourcePath(
					RequestedSourcePath, PathUtilities::EPathExistence::AllowMissing);
				if (!Requested || Requested.Mount != Lookup.Mount)
				{
					OutError = Requested ? "Heightmap source must remain in the asset mount."
						: Requested.Message;
					return false;
				}
				Relative = Requested.RelativePath;
			}
			Relative = Relative.lexically_normal();
			std::string Extension = Relative.extension().generic_string();
			Extension = NormalizeExtension(Extension);
			const std::string RelativeText = Relative.generic_string();
			if (Relative.empty() || Relative.is_absolute() || RelativeText == ".."
				|| RelativeText.starts_with("../") || Extension != SourceExtension)
			{
				OutError = std::format(
					"Heightmap source destination must be a normalized mount-relative {} path matching the source format.",
					SourceExtension);
				return false;
			}
			OutStoredPath = Lookup.Mount->VirtualRoot + RelativeText;
			return true;
		}

		auto BuildFromMountedSource(
			DTerrainHeightmap& Heightmap,
			const FMountedSourceFile& Source,
			std::string& OutError) -> bool
		{
			std::error_code FileSizeError;
			const uintmax_t FileSize = std::filesystem::file_size(
				Source.PhysicalPath, FileSizeError);
			if (FileSizeError || FileSize > MaximumTerrainHeightmapEncodedBytes)
			{
				OutError = FileSizeError
					? "Failed to inspect the terrain heightmap source size."
					: "Terrain heightmap source exceeds the 512 MiB encoded-source limit.";
				return false;
			}
			std::vector<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, Source.PhysicalPath.generic_string()))
			{
				OutError = "Failed to read the terrain heightmap source.";
				return false;
			}
			FTerrainHeightmapDecodedSource Decoded;
			if (!DecodeTerrainHeightmapSource(
				Source.PhysicalPath.extension().generic_string(), Bytes, Decoded, OutError)) return false;
			const FXxHash128 Hash = FXxHash128::HashBuffer(Bytes);
			Asset::Build::FTerrainHeightmapBuildProduct Product;
			if (!Asset::Build::BuildTerrainHeightmap({
				.Samples = std::move(Decoded.Samples), .Width = Decoded.Width,
				.Height = Decoded.Height, .SourceContentHashLow = Hash.HashLow,
				.SourceContentHashHigh = Hash.HashHigh,
				.DecoderId = Decoded.DecoderId,
				.DecoderVersion = Decoded.DecoderVersion,
				.SourceFormat = Decoded.SourceFormat,
				.SourceProfileVersion = Decoded.SourceProfileVersion}, Product, OutError)) return false;
			const std::shared_ptr<const FTerrainHeightmapPayload> ExistingPayload = Heightmap.GetPayload();
			const bool bSamplesChanged = !ExistingPayload
				|| ExistingPayload->Samples != Product.Payload->Samples;
			return Asset::Build::PublishTerrainHeightmapProduct(Heightmap, std::move(Product), {
					.SourcePath = Source.SourcePath,
					.DecoderId = Decoded.DecoderId,
					.DecoderVersion = Decoded.DecoderVersion,
					.SourceFormat = Decoded.SourceFormat,
					.SourceProfileVersion = Decoded.SourceProfileVersion,
					.SourceFileSize = Bytes.size(),
					.bAdvanceRevision = bSamplesChanged}, OutError);
		}
	}

	auto DecodeTerrainHeightmapSource(
		std::string_view Extension,
		std::span<const uint8> Bytes,
		FTerrainHeightmapDecodedSource& OutSource,
		std::string& OutError) -> bool
	{
		OutSource = {};
		const std::string NormalizedExtension = NormalizeExtension(Extension);
		if (NormalizedExtension == ".png")
		{
			Asset::FDecodedGrayscale16Image Decoded;
			if (!Asset::DecodeGrayscale16PngFromMemory(Bytes, Decoded, OutError, {
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
			OutError.clear();
			return true;
		}
		if (NormalizedExtension != ".raw")
		{
			OutError = "Terrain heightmap source extension must be .png or .raw.";
			return false;
		}
		if (Bytes.size() > MaximumTerrainHeightmapEncodedBytes)
		{
			OutError = "RAW16 terrain heightmap exceeds the 512 MiB encoded-source limit.";
			return false;
		}
		if (Bytes.size() < 8)
		{
			OutError = "RAW16 terrain heightmap must contain at least four samples (8 bytes).";
			return false;
		}
		if ((Bytes.size() & 1u) != 0)
		{
			OutError = "RAW16 terrain heightmap byte count must be even.";
			return false;
		}
		const uint64 SampleCount = Bytes.size() / sizeof(uint16);
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
				static_cast<uint16>(Bytes[ByteOffset])
				| (static_cast<uint16>(Bytes[ByteOffset + 1]) << 8));
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
		bool bEngineAuthoringContext) -> FTerrainHeightmapImportResult
	{
		const std::filesystem::path Input = std::filesystem::absolute(FilePath).lexically_normal();
		std::string Extension = Input.extension().generic_string();
		Extension = NormalizeExtension(Extension);
		if (!std::filesystem::is_regular_file(Input) || !IsSupportedHeightmapExtension(Extension))
			return {false, "Terrain heightmap import requires an existing .png or .raw source.", nullptr};
		FAssetPath ParsedPath;
		std::string Error;
		if (!FAssetPath::TryCreate(AssetPath, ParsedPath, &Error))
			return {false, std::move(Error), nullptr};
		if (Asset::GetAssetRegistry().FindAssetExact(ParsedPath)
			|| Asset::FindLoadedPackage(ParsedPath))
			return {false, std::format("Asset {} already exists.", ParsedPath.ToString()), nullptr};
		std::string StoredSourcePath;
		if (!MakeCanonicalSourceLocation(
			ParsedPath, Settings.SourceDestination, Extension, StoredSourcePath, Error))
			return {false, std::move(Error), nullptr};
		FMountedSourceFile MountedSource;
		if (!PrepareMountedSourceFile(Input, ParsedPath.ToString(), StoredSourcePath,
			MountedSource, Error,
			bEngineAuthoringContext
				? EMountedSourceMutationContext::EngineAuthoring
				: EMountedSourceMutationContext::DependencySafe))
			return {false, std::move(Error), nullptr};
		DTerrainHeightmap* Heightmap = nullptr;
		const Asset::FAssetResult Created = Asset::CreateAsset(ParsedPath, Heightmap);
		if (!Created)
		{
			RollbackMountedSourceFile(MountedSource);
			return {false, Created.Message, nullptr};
		}
		if (!BuildFromMountedSource(*Heightmap, MountedSource, Error))
		{
			RollbackMountedSourceFile(MountedSource);
			Asset::UnloadPackage(ParsedPath);
			return {false, std::move(Error), nullptr};
		}
		const Asset::FAssetResult Saved = Asset::SavePackage(Heightmap->GetPackage());
		if (!Saved)
		{
			RollbackMountedSourceFile(MountedSource);
			Asset::UnloadPackage(ParsedPath);
			return {false, Saved.Message, nullptr};
		}
		CommitMountedSourceFile(MountedSource);
		return {true, {}, Heightmap};
	}

	auto ChangeTerrainHeightmapSourceReference(
		DTerrainHeightmap& Heightmap,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool
	{
		if (!Heightmap.GetPackage())
		{
			OutError = "Terrain heightmap source changes require an owning package.";
			return false;
		}
		FMountedSourceFile Source;
		return ResolveMountedSourceReference(
			Heightmap.GetPackage()->GetPackagePath(), SourceVirtualPath, Source, OutError)
			&& BuildFromMountedSource(Heightmap, Source, OutError);
	}

	auto ReimportTerrainHeightmapSource(
		DTerrainHeightmap& Heightmap,
		std::string& OutError) -> bool
	{
		return ChangeTerrainHeightmapSourceReference(
			Heightmap, Heightmap.GetSourceImportData().SourcePath.Path, OutError);
	}
}
