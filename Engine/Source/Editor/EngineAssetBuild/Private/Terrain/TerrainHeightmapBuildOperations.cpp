#include "Terrain/TerrainHeightmapBuildOperations.h"

#include "AssetSystem.h"
#include "Misc/DerivedDataCache.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Source/SourcePath.h"

namespace Durin
{
	struct FTerrainHeightmapBuildOperations
	{
		static auto SetSourceFingerprint(
			DTerrainHeightmap& Heightmap,
			const std::filesystem::path& PhysicalPath) -> void;
	};
}

namespace Durin::AssetBuild
{
	namespace
	{
		constexpr std::string_view DefaultHeightmapSourceRoot = "TerrainHeightmaps";

		auto FindOwningMount(std::string_view VirtualPath)
			-> const PathUtilities::FMountPoint*
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(VirtualPath);
			return Lookup ? Lookup.Mount : nullptr;
		}

		auto MakeCanonicalSourceLocation(
			const FAssetPath& AssetPath,
			std::string_view RequestedSourcePath,
			std::string& OutStoredPath,
			std::string& OutError) -> bool
		{
			const PathUtilities::FMountPoint* Mount = FindOwningMount(AssetPath.ToString());
			if (!Mount)
			{
				OutError = std::format(
					"Terrain heightmap asset {} is outside a registered mount.",
					AssetPath.ToString());
				return false;
			}
			std::filesystem::path Relative = RequestedSourcePath.empty()
				? std::filesystem::path(DefaultHeightmapSourceRoot)
					/ (std::string(AssetPath.GetAssetName()) + ".png")
				: std::filesystem::path(RequestedSourcePath);
			if (RequestedSourcePath.starts_with('/'))
			{
				const PathUtilities::FSourcePathResult Requested =
					PathUtilities::ResolveSourcePath(
						RequestedSourcePath, PathUtilities::EPathExistence::AllowMissing);
				if (!Requested || Requested.Mount != Mount)
				{
					OutError = Requested
						? "Heightmap source must remain in the asset mount."
						: Requested.Message;
					return false;
				}
				Relative = Requested.RelativePath;
			}
			Relative = Relative.lexically_normal();
			const std::string RelativeText = Relative.generic_string();
			std::string Extension = Relative.extension().generic_string();
			std::ranges::transform(Extension, Extension.begin(), [](unsigned char Character) {
				return static_cast<char>(std::tolower(Character));
			});
			if (Relative.empty() || Relative.is_absolute() || RelativeText == ".."
				|| RelativeText.starts_with("../") || Extension != ".png")
			{
				OutError =
					"Heightmap source destination must be a normalized mount-relative .png path.";
				return false;
			}
			OutStoredPath = Mount->VirtualRoot + RelativeText;
			return true;
		}
	}

	auto BuildTerrainHeightmapFromEncodedBytes(
		DTerrainHeightmap& Heightmap,
		std::span<const uint8> EncodedBytes,
		const FSourcePath& SourcePath,
		std::string& OutError) -> bool
	{
		return Heightmap.BuildFromEncodedBytes(EncodedBytes, SourcePath, OutError);
	}

	auto ImportTerrainHeightmapAsset(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FTerrainHeightmapImportSettings& Settings,
		bool bEngineAuthoringContext) -> FTerrainHeightmapImportResult
	{
		const std::filesystem::path Input =
			std::filesystem::absolute(FilePath).lexically_normal();
		std::string Extension = Input.extension().generic_string();
		std::ranges::transform(Extension, Extension.begin(), [](unsigned char Character) {
			return static_cast<char>(std::tolower(Character));
		});
		if (!std::filesystem::is_regular_file(Input) || Extension != ".png")
			return {false,
				"Terrain heightmap import requires an existing .png source.", nullptr};
		FAssetPath ParsedPath;
		std::string Error;
		if (!FAssetPath::TryCreate(AssetPath, ParsedPath, &Error))
			return {false, std::move(Error), nullptr};
		if (Asset::GetAssetRegistry().FindAssetExact(ParsedPath)
			|| Asset::FindLoadedPackage(ParsedPath))
			return {false,
				std::format("Asset {} already exists.", ParsedPath.ToString()), nullptr};
		std::string StoredSourcePath;
		if (!MakeCanonicalSourceLocation(
			ParsedPath, Settings.SourceDestination, StoredSourcePath, Error))
			return {false, std::move(Error), nullptr};
		FMountedSourceFile MountedSource;
		if (!PrepareMountedSourceFile(
			Input,
			ParsedPath.ToString(),
			StoredSourcePath,
			MountedSource,
			Error,
			bEngineAuthoringContext)) return {false, std::move(Error), nullptr};
		std::vector<uint8> EncodedBytes;
		if (!FFileHelper::LoadFileToArray(
			EncodedBytes, MountedSource.PhysicalPath.generic_string()))
		{
			RollbackMountedSourceFile(MountedSource);
			return {false, "Failed to read the mounted terrain heightmap source.", nullptr};
		}
		DTerrainHeightmap* Heightmap = nullptr;
		const Asset::FAssetResult Created = Asset::CreateAsset(ParsedPath, Heightmap);
		if (!Created)
		{
			RollbackMountedSourceFile(MountedSource);
			return {false, Created.Message, nullptr};
		}
		if (!BuildTerrainHeightmapFromEncodedBytes(
			*Heightmap, EncodedBytes, MountedSource.SourcePath, Error))
		{
			RollbackMountedSourceFile(MountedSource);
			Asset::UnloadPackage(ParsedPath);
			return {false, std::move(Error), nullptr};
		}
		FTerrainHeightmapBuildOperations::SetSourceFingerprint(
			*Heightmap, MountedSource.PhysicalPath);
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
}

namespace Durin
{
	auto FTerrainHeightmapBuildOperations::SetSourceFingerprint(
		DTerrainHeightmap& Heightmap,
		const std::filesystem::path& PhysicalPath) -> void
	{
		std::error_code Error;
		Heightmap.SourceFileSize = std::filesystem::file_size(PhysicalPath, Error);
		const auto LastWrite = std::filesystem::last_write_time(PhysicalPath, Error);
		Heightmap.SourceLastWriteTime = Error ? 0
			: DerivedDataCache::FileTimeToStableTicks(LastWrite);
	}
}
