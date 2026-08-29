#include "AssetRegistry/ObjectStream.h"
#include "AssetRegistry/Scan.h"
#include "AssetRegistry/State.h"

#include "Misc/FileHelper.h"

namespace Durin::Asset
{
	namespace
	{
		constexpr size_t MaximumReferencesPerSnapshot = 1'000'000;

		auto Error(EAssetError Code, std::string Message) -> FAssetResult
		{
			return {Code, std::move(Message)};
		}

		auto ReferenceLess(const FAssetReferenceEdge& Left,
			const FAssetReferenceEdge& Right) -> bool
		{
			return std::tuple(Left.TargetPath.GetView(), Left.SourcePackage.GetView(),
				Left.SourceObjectId, std::string_view(Left.DeclaringType),
				std::string_view(Left.FieldName), Left.Kind, Left.DisplayRoute)
				< std::tuple(Right.TargetPath.GetView(), Right.SourcePackage.GetView(),
					Right.SourceObjectId, std::string_view(Right.DeclaringType),
					std::string_view(Right.FieldName), Right.Kind, Right.DisplayRoute);
		}

		struct FCacheOperationalState
		{
			std::mutex Mutex;
			bool bCatalogDirty = false;
			bool bReferencesDirty = false;
			std::string CatalogWarning;
			std::string ReferenceWarning;
		};

		auto GetCacheOperationalState() -> FCacheOperationalState&
		{
			static FCacheOperationalState State;
			return State;
		}
	}

	auto RefreshAssetRegistry(EAssetRegistryScanMode Mode)
		-> FAssetCatalogRefreshResult
	{
		const auto Started = std::chrono::steady_clock::now();
		const uint64 PriorRevision = GetAssetRegistryState().GetRevision();
		FAssetRegistryScanCandidate Candidate;
		ScanMountedAssetMetadata(Mode, Candidate);
		const std::vector<std::string> MountManifest = Private::GetMountManifest();

		std::unordered_map<FAssetPath, Private::FReferenceCacheSource> CachedSources;
		std::string ReferenceCacheWarning;
		const bool bReferenceCacheLoaded =
			Private::LoadReferenceCache(CachedSources, ReferenceCacheWarning);
		FAssetReferenceIndexStats ReferenceStats;
		std::vector<FAssetResult> ReferenceErrors;
		std::vector<FAssetReferenceEdge> ReferenceEdges;
		std::unordered_map<FAssetPath, FAssetPackageFingerprint> Fingerprints;

		std::vector<const FAssetData*> SortedAssets;
		SortedAssets.reserve(Candidate.Assets.size());
		for (const auto& [Path, Data] : Candidate.Assets)
			SortedAssets.push_back(&Data);
		std::ranges::sort(SortedAssets,
			[](const FAssetData* Left, const FAssetData* Right) {
				return Left->PackagePath.GetView() < Right->PackagePath.GetView();
			});

		for (const FAssetData* Data : SortedAssets)
		{
			const auto Cached = CachedSources.find(Data->PackagePath);
			if (Mode == EAssetRegistryScanMode::Incremental && bReferenceCacheLoaded
				&& Cached != CachedSources.end()
				&& Cached->second.Fingerprint.FileSize == Data->FileSize
				&& Cached->second.Fingerprint.LastWriteTimeTicks
					== Data->LastWriteTimeTicks
				&& Cached->second.Fingerprint.ReaderVersion == Data->FormatVersion)
			{
				if (ReferenceEdges.size() > MaximumReferencesPerSnapshot
					- Cached->second.References.size())
				{
					ReferenceErrors.push_back(Error(EAssetError::CorruptFile,
						"AssetReferenceIndexSnapshotExceeded: scan exceeds 1,000,000 occurrences."));
					++ReferenceStats.FailedSources;
					continue;
				}
				ReferenceEdges.insert(ReferenceEdges.end(),
					Cached->second.References.begin(), Cached->second.References.end());
				Fingerprints.emplace(Data->PackagePath, Cached->second.Fingerprint);
				++ReferenceStats.ReusedSources;
				continue;
			}

			std::vector<std::byte> Bytes;
			std::vector<FAssetReferenceEdge> SourceReferences;
			FAssetPackageFingerprint Fingerprint;
			FAssetResult Result;
			++ReferenceStats.PayloadReadAttempts;
			if (!FFileHelper::LoadFileToArray(Bytes, Data->PhysicalPath))
				Result = Error(EAssetError::IoError, std::format(
					"AssetReferenceIndexReadFailed: could not read {}.",
					Data->PhysicalPath));
			else
			{
				ReferenceStats.PayloadBytesRead += Bytes.size();
				Result = PackageObjectStream::ExtractAssetPackageReferences(
					Bytes, Data->PackagePath, SourceReferences, &Fingerprint);
				Fingerprint.LastWriteTimeTicks = Data->LastWriteTimeTicks;
			}
			++ReferenceStats.ExtractedSources;
			if (!Result)
			{
				Result.Message = std::format("{} ({})", Result.Message, Data->PhysicalPath);
				ReferenceErrors.push_back(std::move(Result));
				++ReferenceStats.FailedSources;
				continue;
			}
			if (ReferenceEdges.size() > MaximumReferencesPerSnapshot
				- SourceReferences.size())
			{
				ReferenceErrors.push_back(Error(EAssetError::CorruptFile,
					"AssetReferenceIndexSnapshotExceeded: scan exceeds 1,000,000 occurrences."));
				++ReferenceStats.FailedSources;
				continue;
			}
			ReferenceEdges.insert(ReferenceEdges.end(),
				std::make_move_iterator(SourceReferences.begin()),
				std::make_move_iterator(SourceReferences.end()));
			Fingerprints.emplace(Data->PackagePath, Fingerprint);
		}
		std::ranges::sort(ReferenceEdges, ReferenceLess);
		const bool bReferenceComplete = ReferenceErrors.empty()
			&& Fingerprints.size() == Candidate.Assets.size();

		Candidate.Stats.DurationMilliseconds =
			std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - Started).count();
		FAssetCatalogRefreshResult Refresh{
			.Mode = Mode,
			.bCatalogComplete = Candidate.Errors.empty(),
			.bReferenceIndexComplete = bReferenceComplete,
			.bRetainedPriorRevision = !Candidate.Errors.empty() || !bReferenceComplete,
			.PriorRevision = PriorRevision,
			.ResultingRevision = PriorRevision,
			.CatalogStats = Candidate.Stats,
			.ReferenceStats = ReferenceStats,
			.Errors = Candidate.Errors,
			.CatalogCacheWarning = Candidate.CacheWarning,
			.ReferenceCacheWarning = ReferenceCacheWarning};
		Refresh.Errors.insert(Refresh.Errors.end(),
			ReferenceErrors.begin(), ReferenceErrors.end());
		if (!Refresh.Succeeded()) return Refresh;

		FAssetResult PublishResult = GetAssetRegistryState().Publish({
			.ExpectedRevision = PriorRevision,
			.Assets = Candidate.Assets,
			.ReferenceEdges = ReferenceEdges,
			.ReferenceFingerprints = Fingerprints,
			.ReferenceErrors = ReferenceErrors,
			.ReferenceStats = ReferenceStats,
			.ReferenceCacheWarning = ReferenceCacheWarning,
			.bReferenceIndexComplete = true});
		if (!PublishResult)
		{
			Refresh.bCatalogComplete = false;
			Refresh.bReferenceIndexComplete = false;
			Refresh.bRetainedPriorRevision = true;
			Refresh.Errors.push_back(std::move(PublishResult));
			return Refresh;
		}
		Refresh.bPublished = true;
		Refresh.bRetainedPriorRevision = false;
		Refresh.ResultingRevision = GetAssetCatalogRevision();
		Refresh.bCatalogCacheDirty = !Private::WriteRegistryCache(
			MountManifest, std::move(Candidate.CacheEntries),
			Refresh.CatalogCacheWarning);
		std::string ReferenceWriteWarning;
		Refresh.bReferenceCacheDirty = !Private::WriteReferenceCache(
			Fingerprints, ReferenceEdges, ReferenceWriteWarning);
		if (!ReferenceWriteWarning.empty())
		{
			if (!Refresh.ReferenceCacheWarning.empty())
				Refresh.ReferenceCacheWarning.append(" ");
			Refresh.ReferenceCacheWarning.append(ReferenceWriteWarning);
		}
		{
			FCacheOperationalState& Operational = GetCacheOperationalState();
			std::lock_guard Lock(Operational.Mutex);
			Operational.bCatalogDirty = Refresh.bCatalogCacheDirty;
			Operational.bReferencesDirty = Refresh.bReferenceCacheDirty;
			Operational.CatalogWarning = Refresh.CatalogCacheWarning;
			Operational.ReferenceWarning = Refresh.ReferenceCacheWarning;
		}
		return Refresh;
	}

	auto MarkAssetRegistryCachesDirty() -> void
	{
		FCacheOperationalState& Operational = GetCacheOperationalState();
		std::lock_guard Lock(Operational.Mutex);
		Operational.bCatalogDirty = true;
		Operational.bReferencesDirty = true;
	}

	auto FlushAssetRegistryCaches() -> void
	{
		FCacheOperationalState& Operational = GetCacheOperationalState();
		std::lock_guard Lock(Operational.Mutex);
		const FAssetRegistryPublication Publication =
			GetAssetRegistryState().CapturePublication();
		if (Operational.bCatalogDirty)
		{
			std::vector<Private::FRegistryCacheEntry> Entries;
			std::string Warning;
			if (Private::BuildRegistryCacheEntries(Publication.Assets, Entries, Warning)
				&& Private::WriteRegistryCache(
					Private::GetMountManifest(), std::move(Entries), Warning))
			{
				Operational.bCatalogDirty = false;
				Operational.CatalogWarning.clear();
			}
			else Operational.CatalogWarning = std::move(Warning);
		}
		if (Operational.bReferencesDirty)
		{
			std::string Warning;
			if (Private::WriteReferenceCache(Publication.ReferenceFingerprints,
				Publication.ReferenceEdges, Warning))
			{
				Operational.bReferencesDirty = false;
				Operational.ReferenceWarning.clear();
			}
			else Operational.ReferenceWarning = std::move(Warning);
		}
	}

	auto IsAssetRegistryCacheDirty() -> bool
	{
		FCacheOperationalState& Operational = GetCacheOperationalState();
		std::lock_guard Lock(Operational.Mutex);
		return Operational.bCatalogDirty || Operational.bReferencesDirty;
	}

	auto GetAssetRegistryCacheWarning() -> std::string
	{
		FCacheOperationalState& Operational = GetCacheOperationalState();
		std::lock_guard Lock(Operational.Mutex);
		if (Operational.ReferenceWarning.empty()) return Operational.CatalogWarning;
		if (Operational.CatalogWarning.empty()) return Operational.ReferenceWarning;
		return std::format("{} {}", Operational.CatalogWarning,
			Operational.ReferenceWarning);
	}
}
