#include "AssetCatalogPersistenceInternal.h"
#include "AssetMutationReferenceInternal.h"
#include "AssetMutationRegistryInternal.h"

#include "DObject/Class.h"
#include "Misc/FileTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Profiling/Profiling.h"

namespace Durin::Asset
{
	using Private::AssetReferenceLess;
	using Private::BuildRegistryCacheEntries;
	using Private::FMutationPackageMetadata;
	using Private::FReferenceCacheSource;
	using Private::FRegistryCacheEntry;
	using Private::GetAssetReferenceStoreRegistry;
	using Private::GetMountManifest;
	using Private::InspectAssetPackageBytesForCatalog;
	using Private::LoadReferenceCache;
	using Private::LoadRegistryCache;
	using Private::MakeRegistryIdentity;
	using Private::ValidateMutationPackageMetadata;
	using Private::WriteReferenceCache;
	using Private::WriteRegistryCache;

	namespace
	{
		constexpr size_t MaximumReferencesPerSnapshot = 1000000;
		constexpr uint32 MaximumRedirectDepth = 64;
		constexpr std::string_view RedirectorClassName =
			"Durin::Asset::DAssetRedirector";

		auto Error(EAssetError Code, std::string Message) -> FAssetResult
		{
			return {Code, std::move(Message)};
		}

		auto IsMissingPathError(const std::error_code& ErrorCode) -> bool
		{
			return ErrorCode == std::errc::no_such_file_or_directory
				|| ErrorCode.value() == 2
				|| ErrorCode.value() == 3;
		}

		auto AssetPathResolutionError(
			const FAssetPathResolveResult& Resolution) -> FAssetResult
		{
			switch (Resolution.State)
			{
			case EAssetPathResolveState::Resolved:
				return {};
			case EAssetPathResolveState::NotFound:
				return Error(EAssetError::NotFound, std::format(
					"Asset {} is not present in the registry.",
					Resolution.RequestedPath.ToString()));
			case EAssetPathResolveState::MissingRedirectTarget:
				return Error(EAssetError::NotFound, std::format(
					"Asset redirect {} has a missing target {}.",
					Resolution.RequestedPath.ToString(),
					Resolution.FinalPath.ToString()));
			case EAssetPathResolveState::RedirectCycle:
				return Error(EAssetError::CircularDependency, std::format(
					"Asset redirect {} contains a cycle at {}.",
					Resolution.RequestedPath.ToString(),
					Resolution.FinalPath.ToString()));
			case EAssetPathResolveState::RedirectDepthExceeded:
				return Error(EAssetError::CircularDependency, std::format(
					"Asset redirect {} exceeds the maximum redirect depth at {}.",
					Resolution.RequestedPath.ToString(),
					Resolution.FinalPath.ToString()));
			case EAssetPathResolveState::UnknownTargetClass:
				return Error(EAssetError::UnknownClass, std::format(
					"Asset {} resolves to a target with an unavailable reflected class.",
					Resolution.RequestedPath.ToString()));
			case EAssetPathResolveState::RedirectTypeMismatch:
				return Error(EAssetError::TypeMismatch, std::format(
					"Asset {} resolves to a target with an incompatible class.",
					Resolution.RequestedPath.ToString()));
			case EAssetPathResolveState::CorruptRedirector:
				return Error(EAssetError::CorruptFile, std::format(
					"CorruptRedirector: asset {} traverses invalid redirect metadata at {}.",
					Resolution.RequestedPath.ToString(),
					Resolution.FinalPath.ToString()));
			}
			return Error(
				EAssetError::CorruptFile,
				"Asset resolution returned an unknown state.");
		}
	}


	auto FAssetCatalogStore::ScanMountedContent(EAssetRegistryScanMode Mode) -> FAssetResult
	{
		RefreshMountedContent(Mode);
		return {};
	}

	auto FAssetCatalogStore::RefreshMountedContent(
		EAssetRegistryScanMode Mode) -> FAssetCatalogRefreshResult
	{
		const uint64 PriorRevision = Revision;
		const FAssetReferenceIndex PriorReferenceIndex = ReferenceIndex;
		const auto ScanStartTime = std::chrono::steady_clock::now();
		std::unordered_map<FAssetPath, FAssetData> NewAssets;
		std::vector<FRegistryCacheEntry> NewCacheEntries;
		std::unordered_map<std::string, FRegistryCacheEntry> CachedEntries;
		std::unordered_map<FAssetPath, FReferenceCacheSource> CachedReferenceSources;
		std::unordered_map<FAssetPath, FAssetPackageInspection> FullValidationInspections;
		std::unordered_set<std::string> SeenCachedIdentities;
		ScanErrors.clear();
		LastScanStats = {};
		CacheWarning.clear();
		ReferenceIndex.Errors.clear();
		ReferenceIndex.Stats = {};
		ReferenceIndex.CacheWarning.clear();
		const std::vector<std::string> MountManifest = GetMountManifest();
		const bool bCacheLoaded = LoadRegistryCache(MountManifest, CachedEntries, CacheWarning);
		const bool bReferenceCacheLoaded = LoadReferenceCache(
			CachedReferenceSources, ReferenceIndex.CacheWarning);
		for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
		{
			if (!Mount.bAutoScan) continue;
			const std::filesystem::path AssetRoot = Mount.GetContentDir();
			std::error_code Ec;
			if (!std::filesystem::exists(AssetRoot, Ec)) continue;
			for (std::filesystem::recursive_directory_iterator It(AssetRoot, Ec), End; !Ec && It != End; It.increment(Ec))
			{
				std::error_code FileEc;
				if (!It->is_regular_file(FileEc) || It->path().extension() != ".dasset") continue;
				++LastScanStats.Enumerated;
				FAssetPackageHeader PackageHeader;
				FAssetPath DiskPath;
				std::filesystem::path Relative = std::filesystem::relative(It->path(), AssetRoot, FileEc).lexically_normal();
				const std::string RelativeString = Relative.generic_string();
				std::filesystem::path PackageRelative = Relative;
				PackageRelative.replace_extension();
				if (FileEc || Relative.is_absolute() || RelativeString.starts_with("../")
					|| !FAssetPath::TryCreate(Mount.VirtualRoot + PackageRelative.generic_string(), DiskPath))
				{
					ScanErrors.push_back(Error(EAssetError::InvalidPath, std::format("Failed to map asset path {}.", It->path().generic_string())));
					++LastScanStats.Failed;
					continue;
				}
				const std::string Identity = MakeRegistryIdentity(Mount.VirtualRoot, RelativeString);
				if (CachedEntries.contains(Identity)) SeenCachedIdentities.insert(Identity);
				const auto LastWriteTime = It->last_write_time(FileEc);
				const auto FileSize = It->file_size(FileEc);
				if (FileEc)
				{
					ScanErrors.push_back(Error(EAssetError::IoError, std::format("Failed to fingerprint asset {}.", It->path().generic_string())));
					++LastScanStats.Failed;
					continue;
				}
				const int64 LastWriteTimeTicks = FileTime::ToStableTicks(LastWriteTime);
				std::string AssetClassName;
				EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
				FAssetPath RedirectDestination;
				uint32 FormatVersion = 0;
				std::vector<FAssetPath> Dependencies;
				const auto CachedIt = CachedEntries.find(Identity);
				if (Mode == EAssetRegistryScanMode::Incremental && CachedIt != CachedEntries.end()
					&& CachedIt->second.FileSize == FileSize && CachedIt->second.LastWriteTimeTicks == LastWriteTimeTicks)
				{
					AssetClassName = CachedIt->second.AssetClassName;
					EntryKind = CachedIt->second.EntryKind;
					RedirectDestination = CachedIt->second.RedirectDestination;
					FormatVersion = CachedIt->second.FormatVersion;
					Dependencies = CachedIt->second.Dependencies;
					++LastScanStats.Reused;
				}
				else
				{
					++LastScanStats.HeaderReadAttempts;
					FAssetResult Result = ReadAssetPackageHeader(It->path().generic_string(), PackageHeader);
					LastScanStats.HeaderBytesRead += PackageHeader.BytesRead;
					LastScanStats.HeaderFileBytesRead += PackageHeader.FileBytesRead;
					if (!Result)
					{
						Result.Message = std::format("{} ({})", Result.Message, It->path().generic_string());
						ScanErrors.push_back(std::move(Result));
						++LastScanStats.Failed;
						continue;
					}
					AssetClassName = std::move(PackageHeader.AssetClassName);
					EntryKind = PackageHeader.EntryKind;
					RedirectDestination = std::move(PackageHeader.RedirectDestination);
					FormatVersion = PackageHeader.FormatVersion;
					Dependencies = std::move(PackageHeader.Dependencies);
					++LastScanStats.Reparsed;
				}
				if (EntryKind == EAssetRegistryEntryKind::Redirector)
				{
					FMutationPackageMetadata RedirectHeader{
						.FormatVersion = FormatVersion,
						.AssetClassName = AssetClassName,
						.EntryKind = EntryKind,
						.RedirectDestination = RedirectDestination,
						.Dependencies = Dependencies};
					FAssetResult RedirectResult = ValidateMutationPackageMetadata(
						RedirectHeader, 1, &DiskPath);
					if (!RedirectResult)
					{
						RedirectResult.Message = std::format(
							"{} ({})", RedirectResult.Message, It->path().generic_string());
						ScanErrors.push_back(std::move(RedirectResult));
						++LastScanStats.Failed;
						continue;
					}
					if (Mode == EAssetRegistryScanMode::FullValidation)
					{
						FAssetPackageInspection Inspection;
						std::vector<uint8> Bytes;
						++ReferenceIndex.Stats.PayloadReadAttempts;
						if (!FFileHelper::LoadFileToArray(Bytes, It->path().generic_string()))
							RedirectResult = Error(EAssetError::IoError, std::format(
								"Failed to open asset package {}.", It->path().generic_string()));
						else
						{
							ReferenceIndex.Stats.PayloadBytesRead += Bytes.size();
							RedirectResult = InspectAssetPackageBytesForCatalog(
								It->path().generic_string(), Bytes, Inspection);
						}
						if (!RedirectResult)
						{
							RedirectResult.Message = std::format(
								"{} ({})", RedirectResult.Message,
								It->path().generic_string());
							ScanErrors.push_back(std::move(RedirectResult));
							++LastScanStats.Failed;
							continue;
						}
						FullValidationInspections.emplace(DiskPath, std::move(Inspection));
					}
					++LastScanStats.Redirectors;
				}
				if (NewAssets.contains(DiskPath))
				{
					ScanErrors.push_back(Error(EAssetError::AlreadyExists, std::format("Duplicate asset path {}.", DiskPath.ToString())));
					++LastScanStats.Failed;
					continue;
				}
				NewCacheEntries.push_back(FRegistryCacheEntry{
					.MountRoot = Mount.VirtualRoot,
					.RelativePath = RelativeString,
					.AssetClassName = AssetClassName,
					.EntryKind = EntryKind,
					.RedirectDestination = RedirectDestination,
					.FormatVersion = FormatVersion,
					.Dependencies = Dependencies,
					.FileSize = FileSize,
					.LastWriteTimeTicks = LastWriteTimeTicks});
				NewAssets.emplace(DiskPath, FAssetData{
					.PackagePath = DiskPath,
					.PhysicalPath = It->path().generic_string(),
					.AssetClassName = std::move(AssetClassName),
					.EntryKind = EntryKind,
					.RedirectDestination = std::move(RedirectDestination),
					.FormatVersion = FormatVersion,
					.Dependencies = std::move(Dependencies),
					.FileSize = FileSize,
					.LastWriteTime = LastWriteTime,
					.LastWriteTimeTicks = LastWriteTimeTicks});
			}
			if (Ec)
			{
				ScanErrors.push_back(Error(EAssetError::IoError, std::format("Failed to enumerate mount {}.", Mount.VirtualRoot)));
				++LastScanStats.Failed;
			}
		}
		if (bCacheLoaded) LastScanStats.Removed = CachedEntries.size() - SeenCachedIdentities.size();

		std::vector<const FAssetData*> SortedAssets;
		SortedAssets.reserve(NewAssets.size());
		for (const auto& [Path, Data] : NewAssets) SortedAssets.push_back(&Data);
		std::ranges::sort(SortedAssets, [](const FAssetData* Left, const FAssetData* Right) {
			return Left->PackagePath.GetView() < Right->PackagePath.GetView();
		});
		std::vector<FAssetReferenceEdge> NewReferenceEdges;
		std::unordered_map<FAssetPath, FAssetPackageFingerprint> NewReferenceFingerprints;
		for (const FAssetData* Data : SortedAssets)
		{
			const auto CachedIt = CachedReferenceSources.find(Data->PackagePath);
			if (Mode == EAssetRegistryScanMode::Incremental && bReferenceCacheLoaded
				&& CachedIt != CachedReferenceSources.end()
				&& CachedIt->second.Fingerprint.FileSize == Data->FileSize
				&& CachedIt->second.Fingerprint.LastWriteTimeTicks == Data->LastWriteTimeTicks
				&& CachedIt->second.Fingerprint.ReaderVersion == Data->FormatVersion)
			{
				if (NewReferenceEdges.size() > MaximumReferencesPerSnapshot
					- CachedIt->second.References.size())
				{
					ReferenceIndex.Errors.push_back(Error(EAssetError::CorruptFile,
						"AssetReferenceIndexSnapshotExceeded: scan exceeds 1,000,000 occurrences."));
					++ReferenceIndex.Stats.FailedSources;
					continue;
				}
				NewReferenceEdges.insert(
					NewReferenceEdges.end(),
					CachedIt->second.References.begin(), CachedIt->second.References.end());
				NewReferenceFingerprints.emplace(Data->PackagePath, CachedIt->second.Fingerprint);
				++ReferenceIndex.Stats.ReusedSources;
				continue;
			}

			FAssetPackageInspection Inspection;
			FAssetResult InspectionResult;
			const auto PreparedIt = FullValidationInspections.find(Data->PackagePath);
			if (PreparedIt != FullValidationInspections.end())
				Inspection = std::move(PreparedIt->second);
			else
			{
				std::vector<uint8> Bytes;
				++ReferenceIndex.Stats.PayloadReadAttempts;
				if (!FFileHelper::LoadFileToArray(Bytes, Data->PhysicalPath))
					InspectionResult = Error(EAssetError::IoError, std::format(
						"AssetReferenceIndexReadFailed: could not read {}.", Data->PhysicalPath));
				else
				{
					ReferenceIndex.Stats.PayloadBytesRead += Bytes.size();
					InspectionResult = InspectAssetPackageBytesForCatalog(
						Data->PhysicalPath, Bytes, Inspection);
				}
			}
			std::vector<FAssetReferenceEdge> SourceReferences;
			if (InspectionResult)
				InspectionResult = ExtractAssetReferences(
					Data->PackagePath, Inspection, SourceReferences);
			++ReferenceIndex.Stats.ExtractedSources;
			if (!InspectionResult)
			{
				InspectionResult.Message = std::format(
					"{} ({})", InspectionResult.Message, Data->PhysicalPath);
				ReferenceIndex.Errors.push_back(std::move(InspectionResult));
				++ReferenceIndex.Stats.FailedSources;
				continue;
			}
			if (NewReferenceEdges.size() > MaximumReferencesPerSnapshot
				- SourceReferences.size())
			{
				ReferenceIndex.Errors.push_back(Error(EAssetError::CorruptFile,
					"AssetReferenceIndexSnapshotExceeded: scan exceeds 1,000,000 occurrences."));
				++ReferenceIndex.Stats.FailedSources;
				continue;
			}
			NewReferenceEdges.insert(
				NewReferenceEdges.end(),
				std::make_move_iterator(SourceReferences.begin()),
				std::make_move_iterator(SourceReferences.end()));
			NewReferenceFingerprints.emplace(Data->PackagePath, Inspection.Fingerprint);
		}
		std::ranges::sort(NewReferenceEdges, &AssetReferenceLess);
		ReferenceIndex.bComplete = ReferenceIndex.Errors.empty()
			&& NewReferenceFingerprints.size() == NewAssets.size();
		if (!ScanErrors.empty() || !ReferenceIndex.bComplete)
		{
			LastScanStats.DurationMilliseconds =
				std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - ScanStartTime).count();
			FAssetCatalogRefreshResult Result{
				.Mode = Mode,
				.bCatalogComplete = ScanErrors.empty(),
				.bReferenceIndexComplete = ReferenceIndex.bComplete,
				.bPublished = false,
				.bRetainedPriorRevision = true,
				.PriorRevision = PriorRevision,
				.ResultingRevision = PriorRevision,
				.CatalogStats = LastScanStats,
				.ReferenceStats = ReferenceIndex.Stats,
				.Errors = ScanErrors,
				.CatalogCacheWarning = CacheWarning,
				.ReferenceCacheWarning = ReferenceIndex.CacheWarning};
			Result.Errors.insert(
				Result.Errors.end(), ReferenceIndex.Errors.begin(),
				ReferenceIndex.Errors.end());
			ReferenceIndex = PriorReferenceIndex;
			DURIN_WARN_CATEGORY(
				"AssetRegistry",
				"Retained catalog revision {} after incomplete refresh with {} catalog error(s) and {} reference error(s).",
				PriorRevision, ScanErrors.size(),
				Result.Errors.size() - ScanErrors.size());
			return Result;
		}

		const bool bAssetsChanged = Assets != NewAssets;
		const bool bReferencesChanged = ReferenceIndex.Edges != NewReferenceEdges
			|| ReferenceIndex.SourceFingerprints != NewReferenceFingerprints;
		if (bAssetsChanged)
		{
			Assets = std::move(NewAssets);
			RebuildRedirectorIndex();
		}
		if (bReferencesChanged)
		{
			ReferenceIndex.Edges = std::move(NewReferenceEdges);
			ReferenceIndex.SourceFingerprints = std::move(NewReferenceFingerprints);
		}
		if (bAssetsChanged || bReferencesChanged) ++Revision;
		bPersistentSnapshotDirty = !WriteRegistryCache(MountManifest, std::move(NewCacheEntries), CacheWarning);
		std::string SoftWriteWarning;
		ReferenceIndex.bSnapshotDirty = !WriteReferenceCache(
			ReferenceIndex.SourceFingerprints, ReferenceIndex.Edges, SoftWriteWarning);
		if (!SoftWriteWarning.empty())
		{
			if (!ReferenceIndex.CacheWarning.empty()) ReferenceIndex.CacheWarning.append(" ");
			ReferenceIndex.CacheWarning.append(SoftWriteWarning);
		}
		LastScanStats.DurationMilliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - ScanStartTime).count();
		DURIN_INFO_CATEGORY("AssetRegistry",
			"Scanned {} asset package(s) in {:.3f} ms: {} redirector(s), {} reused, {} reparsed, {} header read(s), {} logical header byte(s), {} file header byte(s), {} reference payload read(s), {} reference payload byte(s), {} removed, {} failed.",
			LastScanStats.Enumerated, LastScanStats.DurationMilliseconds, LastScanStats.Redirectors,
			LastScanStats.Reused, LastScanStats.Reparsed,
			LastScanStats.HeaderReadAttempts, LastScanStats.HeaderBytesRead,
			LastScanStats.HeaderFileBytesRead,
			ReferenceIndex.Stats.PayloadReadAttempts, ReferenceIndex.Stats.PayloadBytesRead,
			LastScanStats.Removed, LastScanStats.Failed);
		if (!CacheWarning.empty())
		{
			if (bPersistentSnapshotDirty)
				DURIN_WARN_CATEGORY("AssetRegistry", "{}", CacheWarning);
			else
				DURIN_INFO_CATEGORY("AssetRegistry",
					"Rebuilt asset registry cache after a recoverable cache read issue: {}",
					CacheWarning);
		}
		if (!ReferenceIndex.CacheWarning.empty())
		{
			if (ReferenceIndex.bSnapshotDirty)
				DURIN_WARN_CATEGORY("AssetRegistry", "{}", ReferenceIndex.CacheWarning);
			else
				DURIN_INFO_CATEGORY("AssetRegistry",
					"Rebuilt asset-reference cache after a recoverable cache read issue: {}",
					ReferenceIndex.CacheWarning);
		}
		for (const FAssetResult& ReferenceError : ReferenceIndex.Errors)
			DURIN_WARN_CATEGORY("AssetRegistry", "{}", ReferenceError.Message);
		FAssetCatalogRefreshResult Result{
			.Mode = Mode,
			.bCatalogComplete = ScanErrors.empty(),
			.bReferenceIndexComplete = ReferenceIndex.IsComplete(),
			.bPublished = true,
			.bRetainedPriorRevision = false,
			.PriorRevision = PriorRevision,
			.ResultingRevision = Revision,
			.CatalogStats = LastScanStats,
			.ReferenceStats = ReferenceIndex.GetStats(),
			.Errors = ScanErrors,
			.CatalogCacheWarning = CacheWarning,
			.ReferenceCacheWarning = ReferenceIndex.CacheWarning};
		Result.Errors.insert(
			Result.Errors.end(), ReferenceIndex.Errors.begin(),
			ReferenceIndex.Errors.end());
		return Result;
	}

	auto FAssetCatalogStore::FlushPersistentSnapshot() -> void
	{
		if (bPersistentSnapshotDirty)
		{
			std::vector<FRegistryCacheEntry> Entries;
			std::string Warning;
			if (BuildRegistryCacheEntries(Assets, Entries, Warning)
				&& WriteRegistryCache(GetMountManifest(), std::move(Entries), Warning))
			{
				bPersistentSnapshotDirty = false;
				CacheWarning.clear();
			}
			else
			{
				CacheWarning = std::move(Warning);
				if (!CacheWarning.empty()) DURIN_WARN_CATEGORY("AssetRegistry", "{}", CacheWarning);
			}
		}
		if (ReferenceIndex.bSnapshotDirty)
		{
			std::string Warning;
			if (WriteReferenceCache(
				ReferenceIndex.SourceFingerprints, ReferenceIndex.Edges, Warning))
			{
				ReferenceIndex.bSnapshotDirty = false;
				ReferenceIndex.CacheWarning.clear();
			}
			else
			{
				ReferenceIndex.CacheWarning = std::move(Warning);
				if (!ReferenceIndex.CacheWarning.empty())
					DURIN_WARN_CATEGORY("AssetRegistry", "{}", ReferenceIndex.CacheWarning);
			}
		}
	}

	auto FAssetCatalogStore::FindAssetExactPointer(
		const FAssetPath& Path) const -> const FAssetData*
	{
		auto It = Assets.find(Path);
		return It == Assets.end() ? nullptr : &It->second;
	}

	auto FAssetCatalogStore::FindAssetExact(
		const FAssetPath& Path) const -> FAssetCatalogEntry
	{
		const FAssetData* Data = FindAssetExactPointer(Path);
		return {.Revision = Revision,
			.Data = Data ? std::optional<FAssetData>(*Data) : std::nullopt};
	}

	auto FAssetCatalogStore::CaptureSnapshot() const -> FAssetCatalogSnapshot
	{
		return {.Revision = Revision, .Assets = Assets};
	}

	auto FAssetCatalogStore::ResolveAssetPath(
		const FAssetPath& Path,
		const FAssetPathResolveOptions& Options) const -> FAssetPathResolveResult
	{
		FAssetPathResolveResult Result;
		Result.CatalogRevision = Revision;
		Result.RequestedPath = Path;
		FAssetPath Current = Path;
		std::unordered_set<FAssetPath> Visited;
		while (true)
		{
			const FAssetData* Data = FindAssetExactPointer(Current);
			if (!Data)
			{
				Result.FinalPath = Current;
				Result.State = Result.RedirectChain.empty()
					? EAssetPathResolveState::NotFound
					: EAssetPathResolveState::MissingRedirectTarget;
				return Result;
			}
			if (Data->EntryKind == EAssetRegistryEntryKind::Asset)
			{
				if (Data->RedirectDestination.IsValid()
					|| Data->AssetClassName == RedirectorClassName)
				{
					Result.FinalPath = Current;
					Result.State = EAssetPathResolveState::CorruptRedirector;
					return Result;
				}
				DClass* TargetClass = FindClassByQualifiedName(FName(Data->AssetClassName));
				if (!TargetClass)
				{
					Result.FinalPath = Current;
					Result.State = EAssetPathResolveState::UnknownTargetClass;
					return Result;
				}
				if (Options.ExpectedClass && !TargetClass->IsChildOf(Options.ExpectedClass))
				{
					Result.FinalPath = Current;
					Result.State = EAssetPathResolveState::RedirectTypeMismatch;
					return Result;
				}
				Result.FinalPath = Current;
				Result.FinalAssetData = *Data;
				Result.State = EAssetPathResolveState::Resolved;
				return Result;
			}
			if (Data->EntryKind != EAssetRegistryEntryKind::Redirector
				|| Data->AssetClassName != RedirectorClassName
				|| !Data->RedirectDestination.IsValid()
				|| Data->RedirectDestination == Current
				|| Data->Dependencies.size() != 1
				|| Data->Dependencies.front() != Data->RedirectDestination)
			{
				Result.FinalPath = Current;
				Result.State = EAssetPathResolveState::CorruptRedirector;
				return Result;
			}
			if (!Visited.insert(Current).second)
			{
				Result.FinalPath = Current;
				Result.State = EAssetPathResolveState::RedirectCycle;
				return Result;
			}
			if (Result.RedirectChain.size() == MaximumRedirectDepth)
			{
				Result.FinalPath = Current;
				Result.State = EAssetPathResolveState::RedirectDepthExceeded;
				return Result;
			}
			Result.RedirectChain.push_back(Current);
			Current = Data->RedirectDestination;
		}
	}

	auto FAssetCatalogStore::FindRedirectorsTo(
		const FAssetPath& Destination) const -> std::vector<FAssetPath>
	{
		const auto It = RedirectorsByDestination.find(Destination);
		return It == RedirectorsByDestination.end()
			? std::vector<FAssetPath>{} : It->second;
	}

	auto FAssetCatalogStore::RebuildRedirectorIndex() -> void
	{
		RedirectorsByDestination.clear();
		for (const auto& [Path, Data] : Assets)
		{
			if (Data.EntryKind == EAssetRegistryEntryKind::Redirector
				&& Data.RedirectDestination.IsValid())
				RedirectorsByDestination[Data.RedirectDestination].push_back(Path);
		}
		for (auto& [Destination, Redirectors] : RedirectorsByDestination)
		{
			std::ranges::sort(Redirectors, [](const FAssetPath& Left, const FAssetPath& Right) {
				return Left.GetView() < Right.GetView();
			});
		}
	}

	auto FAssetReferenceIndex::FindReferencers(
		const FAssetPath& Target) const -> std::vector<FAssetReferenceEdge>
	{
		std::vector<FAssetReferenceEdge> Result;
		for (const FAssetReferenceEdge& Reference : Edges)
			if (Reference.TargetPath == Target) Result.push_back(Reference);
		return Result;
	}

	auto FAssetReferenceIndex::FindTargets(
		const FAssetPath& Source) const -> std::vector<FAssetPath>
	{
		std::vector<FAssetPath> Result;
		for (const FAssetReferenceEdge& Reference : Edges)
			if (Reference.SourcePackage == Source) Result.push_back(Reference.TargetPath);
		std::ranges::sort(Result, [](const FAssetPath& Left, const FAssetPath& Right) {
			return Left.GetView() < Right.GetView();
		});
		Result.erase(std::unique(Result.begin(), Result.end()), Result.end());
		return Result;
	}

	auto FAssetCatalogStore::BuildCookReachability(
		std::span<const FAssetPath> Roots,
		std::vector<FAssetPath>& OutPackages) const -> FAssetResult
	{
		OutPackages.clear();
		struct FPendingCookPath
		{
			FAssetPath Path;
			std::string ExpectedClass;
			std::string Source;
		};
		std::vector<FPendingCookPath> Pending;
		Pending.reserve(Roots.size());
		for (const FAssetPath& Root : Roots)
			Pending.push_back({Root, {}, "explicit Cook root"});
		for (const auto& [Handle, Entry] : GetAssetReferenceStoreRegistry().Stores)
		{
			(void)Handle;
			IAssetReferenceStore* Store = Entry.Store;
			if (!Store) continue;
			auto Call = Entry.OwnerGate.TryEnter();
			if (Entry.OwnerGate.IsValid() && !Call) continue;
			FAssetReferenceStoreSnapshot Snapshot;
			FAssetResult StoreResult = Store->CaptureSnapshot(Snapshot);
			if (!StoreResult)
			{
				StoreResult.Message = std::format(
					"CookReachabilityExternalRootProviderFailed: {}",
					StoreResult.Message);
				return StoreResult;
			}
			for (const FAssetReferenceStoreOccurrence& Occurrence :
				Snapshot.Occurrences)
				if (Occurrence.bCookRoot)
					Pending.push_back({
						Occurrence.TargetPath,
						Occurrence.ExpectedClass,
						Occurrence.DisplayRoute});
		}
		std::unordered_set<FAssetPath> Visited;
		while (!Pending.empty())
		{
			std::ranges::sort(Pending, [](const FPendingCookPath& Left,
				const FPendingCookPath& Right) {
				return Left.Path.GetView() > Right.Path.GetView();
			});
			FPendingCookPath Requested = std::move(Pending.back());
			Pending.pop_back();
			DClass* ExpectedClass = nullptr;
			if (!Requested.ExpectedClass.empty())
			{
				ExpectedClass = FindClassByQualifiedName(FName(Requested.ExpectedClass));
				if (!ExpectedClass)
					return Error(EAssetError::UnknownClass, std::format(
						"CookReachabilityUnknownRootClass: {} expects unavailable class {}.",
						Requested.Source, Requested.ExpectedClass));
			}
			const FAssetPathResolveResult SourceResolution = ResolveAssetPath(
				Requested.Path, {.ExpectedClass = ExpectedClass});
			if (!SourceResolution)
			{
				FAssetResult ResolutionError = AssetPathResolutionError(SourceResolution);
				if (ResolutionError.Error == EAssetError::NotFound)
					ResolutionError.Error = EAssetError::MissingDependency;
				ResolutionError.Message = std::format(
					"CookReachabilityUnresolvedRoot: {} from {}. {}",
					Requested.Path.ToString(), Requested.Source,
					ResolutionError.Message);
				return ResolutionError;
			}
			const FAssetPath Source = SourceResolution.FinalPath;
			if (!Visited.insert(Source).second) continue;
			const FAssetData* SourceData = FindAssetExactPointer(Source);
			if (!SourceData || SourceData->EntryKind != EAssetRegistryEntryKind::Asset)
				return Error(EAssetError::InvalidPackageType, std::format(
					"CookReachabilityNonAssetPackage: {} is not a real asset.", Source.ToString()));
			if (!ReferenceIndex.SourceFingerprints.contains(Source))
				return Error(EAssetError::StaleData, std::format(
					"CookReachabilityIncompleteReferenceIndex: {} has no current source entry.", Source.ToString()));
			for (const FAssetPath& Dependency : SourceData->Dependencies)
			{
				const FAssetPathResolveResult Resolution = ResolveAssetPath(Dependency);
				if (!Resolution)
				{
					FAssetResult ResolutionError = AssetPathResolutionError(Resolution);
					if (ResolutionError.Error == EAssetError::NotFound)
						ResolutionError.Error = EAssetError::MissingDependency;
					ResolutionError.Message = std::format(
						"CookReachabilityUnresolvedHardDependency: {} references {}. {}",
						Source.ToString(), Dependency.ToString(), ResolutionError.Message);
					return ResolutionError;
				}
				Pending.push_back({
					Resolution.FinalPath, {},
					std::format("hard dependency of {}", Source.ToString())});
			}
			for (const FAssetReferenceEdge& Reference : ReferenceIndex.Edges)
			{
				if (Reference.SourcePackage != Source
					|| Reference.Kind == EAssetReferenceKind::Redirect) continue;
				DClass* ExpectedClass = FindClassByQualifiedName(FName(Reference.ExpectedClass));
				if (!ExpectedClass)
					return Error(EAssetError::UnknownClass, std::format(
						"CookReachabilityUnknownReferenceClass: {} expects unavailable class {}.",
						Reference.DisplayRoute, Reference.ExpectedClass));
				const FAssetPathResolveResult Resolution = ResolveAssetPath(
					Reference.TargetPath, {.ExpectedClass = ExpectedClass});
				if (!Resolution)
				{
					FAssetResult ResolutionError = AssetPathResolutionError(Resolution);
					if (ResolutionError.Error == EAssetError::NotFound)
						ResolutionError.Error = EAssetError::MissingDependency;
					ResolutionError.Message = std::format(
						"CookReachabilityUnresolvedReference: {} references {} at {}. {}",
						Source.ToString(), Reference.TargetPath.ToString(),
						Reference.DisplayRoute, ResolutionError.Message);
					return ResolutionError;
				}
				Pending.push_back({
					Resolution.FinalPath, {}, Reference.DisplayRoute});
			}
		}
		OutPackages.assign(Visited.begin(), Visited.end());
		std::ranges::sort(OutPackages, [](const FAssetPath& Left, const FAssetPath& Right) {
			return Left.GetView() < Right.GetView();
		});
		return {};
	}

	auto FAssetCatalogStore::RemoveReferencesFromSource(const FAssetPath& Path) -> bool
	{
		const size_t PreviousCount = ReferenceIndex.Edges.size();
		std::erase_if(ReferenceIndex.Edges, [&](const FAssetReferenceEdge& Reference) {
			return Reference.SourcePackage == Path;
		});
		const bool bChanged = PreviousCount != ReferenceIndex.Edges.size()
			|| ReferenceIndex.SourceFingerprints.erase(Path) != 0;
		if (bChanged) ReferenceIndex.bSnapshotDirty = true;
		return bChanged;
	}

	auto FAssetCatalogStore::RefreshReferencesForAsset(const FAssetData& Data) -> bool
	{
		const std::vector<FAssetReferenceEdge> PreviousReferences = ReferenceIndex.Edges;
		const auto PreviousFingerprints = ReferenceIndex.SourceFingerprints;
		RemoveReferencesFromSource(Data.PackagePath);
		ReferenceIndex.Errors.clear();
		FAssetPackageInspection Inspection;
		FAssetResult Result = InspectAssetPackage(Data.PhysicalPath, Inspection);
		std::vector<FAssetReferenceEdge> SourceReferences;
		if (Result)
			Result = ExtractAssetReferences(Data.PackagePath, Inspection, SourceReferences);
		if (!Result)
		{
			Result.Message = std::format("{} ({})", Result.Message, Data.PhysicalPath);
			ReferenceIndex.Errors.push_back(std::move(Result));
			ReferenceIndex.bSnapshotDirty = true;
			ReferenceIndex.bComplete = false;
			return PreviousReferences != ReferenceIndex.Edges
				|| PreviousFingerprints != ReferenceIndex.SourceFingerprints;
		}
		if (ReferenceIndex.Edges.size() > MaximumReferencesPerSnapshot - SourceReferences.size())
		{
			ReferenceIndex.Errors.push_back(Error(EAssetError::CorruptFile,
				"AssetReferenceIndexSnapshotExceeded: mutation exceeds 1,000,000 occurrences."));
			ReferenceIndex.bSnapshotDirty = true;
			ReferenceIndex.bComplete = false;
			return PreviousReferences != ReferenceIndex.Edges
				|| PreviousFingerprints != ReferenceIndex.SourceFingerprints;
		}
		ReferenceIndex.Edges.insert(
			ReferenceIndex.Edges.end(),
			std::make_move_iterator(SourceReferences.begin()),
			std::make_move_iterator(SourceReferences.end()));
		std::ranges::sort(ReferenceIndex.Edges, &AssetReferenceLess);
		ReferenceIndex.SourceFingerprints.insert_or_assign(
			Data.PackagePath, Inspection.Fingerprint);
		const bool bChanged = PreviousReferences != ReferenceIndex.Edges
			|| PreviousFingerprints != ReferenceIndex.SourceFingerprints;
		ReferenceIndex.bComplete = ReferenceIndex.Errors.empty()
			&& ReferenceIndex.SourceFingerprints.size() == Assets.size();
		if (bChanged) ReferenceIndex.bSnapshotDirty = true;
		return bChanged;
	}

	auto FAssetCatalogStore::AddOrUpdate(FAssetData Data) -> void
	{
		const FAssetPath Path = Data.PackagePath;
		const auto Existing = Assets.find(Path);
		const bool bAssetChanged = Existing == Assets.end() || Existing->second != Data;
		Assets.insert_or_assign(Path, std::move(Data));
		if (bAssetChanged) RebuildRedirectorIndex();
		bPersistentSnapshotDirty = true;
		const FAssetData* Stored = FindAssetExactPointer(Path);
		const bool bReferencesChanged = Stored && RefreshReferencesForAsset(*Stored);
		if (bAssetChanged || bReferencesChanged) ++Revision;
	}

	auto FAssetCatalogStore::Remove(const FAssetPath& Path) -> void
	{
		const bool bAssetChanged = Assets.erase(Path) != 0;
		if (bAssetChanged) RebuildRedirectorIndex();
		const bool bReferencesChanged = RemoveReferencesFromSource(Path);
		ReferenceIndex.bComplete = ReferenceIndex.Errors.empty()
			&& ReferenceIndex.SourceFingerprints.size() == Assets.size();
		if (!bAssetChanged && !bReferencesChanged) return;
		bPersistentSnapshotDirty = true;
		++Revision;
	}
}
