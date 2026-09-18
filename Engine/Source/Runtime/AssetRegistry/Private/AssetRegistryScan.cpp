#include "AssetRegistryScanInternal.h"

#include "Misc/FileTime.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"

namespace Durin::AssetPrivate
{
	namespace
	{

		auto Error(EAssetRegistryError Code, FAssetRegistryErrorContext Context) -> FAssetRegistryResult
		{
			return {Code, std::move(Context)};
		}

		auto ValidateRedirectorHeader(
			const FAssetPackageHeader& Header,
			const FPackagePath& Source) -> FAssetRegistryResult
		{
			if (!ArePackageAssetsValid(Header.TopLevelAssets, Source,
				Header.ObjectCount, Header.Dependencies))
				return Error(EAssetRegistryError::CorruptFile,
					{.Reason = EAssetRegistryFailure::InvalidRedirector});
			return {};
		}
	}

	auto ScanMountedAssetMetadata(EAssetRegistryScanMode Mode,
		FAssetRegistryScanCandidate& OutCandidate) -> void
	{
		FAssetRegistryScanCandidate Result;
		std::unordered_map<std::string, FRegistryCacheEntry> CachedEntries;
		std::unordered_set<std::string> SeenCachedIdentities;
		const std::vector<std::string> MountManifest = GetMountManifest();
		const bool bCacheLoaded = LoadRegistryCache(
			MountManifest, CachedEntries, Result.CacheWarning);

		for (const FMountPoint& Mount : FMountPaths::GetRegisteredMountPoints())
		{
			if (!Mount.bAutoScan) continue;
			const std::filesystem::path AssetRoot = Mount.GetContentDir();
			std::error_code Ec;
			if (!std::filesystem::exists(AssetRoot, Ec)) continue;
			for (std::filesystem::recursive_directory_iterator It(AssetRoot, Ec), End;
				!Ec && It != End; It.increment(Ec))
			{
				std::error_code FileEc;
				if (!It->is_regular_file(FileEc) || It->path().extension() != ".dasset")
					continue;
				++Result.Stats.Enumerated;
				FPackagePath DiskPath;
				std::filesystem::path Relative = std::filesystem::relative(
					It->path(), AssetRoot, FileEc).lexically_normal();
				const std::string RelativeString = Relative.generic_string();
				std::filesystem::path PackageRelative = Relative;
				PackageRelative.replace_extension();
				if (FileEc || Relative.is_absolute() || RelativeString.starts_with("../")
					|| !FPackagePath::TryCreate(
						Mount.VirtualRoot + PackageRelative.generic_string(), DiskPath))
				{
					Result.Errors.push_back(Error(EAssetRegistryError::InvalidPath,
						{.Reason = EAssetRegistryFailure::MapPathFailed, .Path = It->path().generic_string(), .SystemError = FileEc}));
					++Result.Stats.Failed;
					continue;
				}
				const std::string Identity = MakeRegistryIdentity(
					Mount.VirtualRoot, RelativeString);
				if (CachedEntries.contains(Identity)) SeenCachedIdentities.insert(Identity);
				const auto LastWriteTime = It->last_write_time(FileEc);
				const auto FileSize = It->file_size(FileEc);
				if (FileEc)
				{
					Result.Errors.push_back(Error(EAssetRegistryError::IoError,
						{.Reason = EAssetRegistryFailure::FingerprintFailed, .Path = It->path().generic_string(), .SystemError = FileEc}));
					++Result.Stats.Failed;
					continue;
				}
				const int64 LastWriteTimeTicks = FileTime::ToStableTicks(LastWriteTime);
				FAssetPackageHeader Header;
				const auto CachedIt = CachedEntries.find(Identity);
				if (Mode == EAssetRegistryScanMode::Incremental
					&& CachedIt != CachedEntries.end()
					&& CachedIt->second.FileSize == FileSize
					&& CachedIt->second.LastWriteTimeTicks == LastWriteTimeTicks)
				{
					Header.TopLevelAssets.clear();
					for (const FTopLevelAssetData& Asset : CachedIt->second.TopLevelAssets)
						Header.TopLevelAssets.push_back({Asset.AssetPath,
							Asset.AssetClassName, Asset.RedirectDestination});
					if (Header.TopLevelAssets.size() == 1)
					{
						const auto& Asset = Header.TopLevelAssets.front();
						Header.AssetClassName = Asset.AssetClassName;
						Header.EntryKind = Asset.IsRedirector()
							? EAssetRegistryEntryKind::Redirector : EAssetRegistryEntryKind::Asset;
						Header.RedirectDestination = Asset.RedirectDestination.GetPackagePath();
					}
					Header.FormatVersion = CachedIt->second.FormatVersion;
					Header.Dependencies = CachedIt->second.Dependencies;
					Header.SoftDependencies = CachedIt->second.SoftDependencies;
					Header.SearchableNames = CachedIt->second.SearchableNames;
					Header.ObjectCount = CachedIt->second.ObjectCount;
					Header.BulkSegmentExtent = CachedIt->second.BulkSegmentExtent;
					Header.BulkSegmentDigest = CachedIt->second.BulkSegmentDigest;
					++Result.Stats.Reused;
				}
				else
				{
					++Result.Stats.HeaderReadAttempts;
					FAssetRegistryResult ReadResult = ReadAssetPackageHeader(
						It->path().generic_string(), DiskPath, Header);
					Result.Stats.HeaderBytesRead += Header.BytesRead;
					Result.Stats.HeaderFileBytesRead += Header.FileBytesRead;
					if (!ReadResult)
					{
						ReadResult.Context.Path = It->path().generic_string();
						Result.Errors.push_back(std::move(ReadResult));
						++Result.Stats.Failed;
						continue;
					}
					++Result.Stats.Reparsed;
				}
				if (FAssetRegistryResult Validation = ValidateRedirectorHeader(Header, DiskPath);
					!Validation)
				{
					Validation.Context.Path = It->path().generic_string();
					Result.Errors.push_back(std::move(Validation));
					++Result.Stats.Failed;
					continue;
				}
				for (const auto& Asset : Header.TopLevelAssets)
					if (Asset.RedirectDestination.IsValid()) ++Result.Stats.Redirectors;
				if (Result.Assets.contains(DiskPath))
				{
					Result.Errors.push_back(Error(EAssetRegistryError::AlreadyExists,
						{.Reason = EAssetRegistryFailure::DuplicatePath, .Path = DiskPath.ToString()}));
					++Result.Stats.Failed;
					continue;
				}
				Result.CacheEntries.push_back(FRegistryCacheEntry{
					.MountRoot = Mount.VirtualRoot,
					.RelativePath = RelativeString,
					.TopLevelAssets = [&] {
						std::vector<FTopLevelAssetData> Assets;
						for (const auto& Asset : Header.TopLevelAssets)
							Assets.push_back({Asset.AssetPath, Asset.AssetClassName,
								Asset.RedirectDestination});
						return Assets;
					}(),
					.FormatVersion = Header.FormatVersion,
					.Dependencies = Header.Dependencies,
					.SoftDependencies = Header.SoftDependencies,
					.SearchableNames = Header.SearchableNames,
					.ObjectCount = Header.ObjectCount,
					.BulkSegmentExtent = Header.BulkSegmentExtent,
					.BulkSegmentDigest = Header.BulkSegmentDigest,
					.FileSize = FileSize,
					.LastWriteTimeTicks = LastWriteTimeTicks});
				Result.Assets.emplace(DiskPath, FAssetData{
					.PackagePath = DiskPath,
					.PhysicalPath = It->path().generic_string(),
					.TopLevelAssets = [&] {
						std::vector<FTopLevelAssetData> Assets;
						for (auto& Asset : Header.TopLevelAssets)
							Assets.push_back({std::move(Asset.AssetPath),
								std::move(Asset.AssetClassName),
								std::move(Asset.RedirectDestination)});
						return Assets;
					}(),
					.AssetClassName = std::move(Header.AssetClassName),
					.EntryKind = Header.EntryKind,
					.RedirectDestination = std::move(Header.RedirectDestination),
					.FormatVersion = Header.FormatVersion,
					.Dependencies = std::move(Header.Dependencies),
					.SoftDependencies = std::move(Header.SoftDependencies),
					.SearchableNames = std::move(Header.SearchableNames),
					.ObjectCount = Header.ObjectCount,
					.BulkSegmentExtent = Header.BulkSegmentExtent,
					.BulkSegmentDigest = Header.BulkSegmentDigest,
					.FileSize = FileSize,
					.LastWriteTime = LastWriteTime,
					.LastWriteTimeTicks = LastWriteTimeTicks});
			}
			if (Ec)
			{
				Result.Errors.push_back(Error(EAssetRegistryError::IoError,
					{.Reason = EAssetRegistryFailure::EnumerationFailed, .Path = Mount.VirtualRoot, .SystemError = Ec}));
				++Result.Stats.Failed;
			}
		}
		if (bCacheLoaded)
			Result.Stats.Removed = CachedEntries.size() - SeenCachedIdentities.size();
		OutCandidate = std::move(Result);
	}
}
