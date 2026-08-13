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

		auto MakeCanonicalSourceLocation(
			const FAssetPath& AssetPath,
			std::string_view RequestedSourcePath,
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
					/ (std::string(AssetPath.GetAssetName()) + ".png")
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
			std::ranges::transform(Extension, Extension.begin(), [](unsigned char Character) {
				return static_cast<char>(std::tolower(Character));
			});
			const std::string RelativeText = Relative.generic_string();
			if (Relative.empty() || Relative.is_absolute() || RelativeText == ".."
				|| RelativeText.starts_with("../") || Extension != ".png")
			{
				OutError = "Heightmap source destination must be a normalized mount-relative .png path.";
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
			std::vector<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, Source.PhysicalPath.generic_string()))
			{
				OutError = "Failed to read the terrain heightmap source.";
				return false;
			}
			Asset::FDecodedGrayscale16Image Decoded;
			if (!Asset::DecodeGrayscale16PngFromMemory(Bytes, Decoded, OutError, {
				.MaximumEncodedBytes = MaximumTerrainHeightmapEncodedBytes,
				.MaximumDecodedPixels = MaximumTerrainHeightmapSamples})) return false;
			const FXxHash128 Hash = FXxHash128::HashBuffer(Bytes);
			Asset::Build::FTerrainHeightmapBuildProduct Product;
			return Asset::Build::BuildTerrainHeightmap({
				.Samples = std::move(Decoded.Samples), .Width = Decoded.Width,
				.Height = Decoded.Height, .SourceContentHashLow = Hash.HashLow,
				.SourceContentHashHigh = Hash.HashHigh}, Product, OutError)
				&& Asset::Build::PublishTerrainHeightmapProduct(Heightmap, std::move(Product), {
					.SourcePath = Source.SourcePath, .SourceFileSize = Bytes.size()}, OutError);
		}
	}

	auto ImportTerrainHeightmapAsset(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FTerrainHeightmapImportSettings& Settings,
		bool bEngineAuthoringContext) -> FTerrainHeightmapImportResult
	{
		const std::filesystem::path Input = std::filesystem::absolute(FilePath).lexically_normal();
		std::string Extension = Input.extension().generic_string();
		std::ranges::transform(Extension, Extension.begin(), [](unsigned char Character) {
			return static_cast<char>(std::tolower(Character));
		});
		if (!std::filesystem::is_regular_file(Input) || Extension != ".png")
			return {false, "Terrain heightmap import requires an existing .png source.", nullptr};
		FAssetPath ParsedPath;
		std::string Error;
		if (!FAssetPath::TryCreate(AssetPath, ParsedPath, &Error))
			return {false, std::move(Error), nullptr};
		if (Asset::GetAssetRegistry().FindAssetExact(ParsedPath)
			|| Asset::FindLoadedPackage(ParsedPath))
			return {false, std::format("Asset {} already exists.", ParsedPath.ToString()), nullptr};
		std::string StoredSourcePath;
		if (!MakeCanonicalSourceLocation(
			ParsedPath, Settings.SourceDestination, StoredSourcePath, Error))
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
