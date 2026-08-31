#include "AssetRegistry/Publication.h"
#include "AssetRegistry/Scan.h"
#include "AssetRegistryStateInternal.h"
#include "AssetRegistryScanInternal.h"

namespace Durin::Asset
{
	namespace
	{
		constexpr size_t MaximumReferencesPerSnapshot = 1'000'000;

		auto Error(EAssetError Code, std::string Message) -> FAssetResult
		{
			return {Code, std::move(Message)};
		}

		auto ReferenceLess(const FAssetPackageReferenceEdge& Left,
			const FAssetPackageReferenceEdge& Right) -> bool
		{
			return std::tuple(Left.TargetPath.GetView(), Left.SourcePackage.GetView(), Left.Kind)
				< std::tuple(Right.TargetPath.GetView(), Right.SourcePackage.GetView(), Right.Kind);
		}

		struct FCacheOperationalState
		{
			std::mutex Mutex;
			bool bCatalogDirty = false;
			std::string CatalogWarning;
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
		const uint64 PriorRevision = Private::GetAssetRegistryState().GetRevision();
		Private::FAssetRegistryScanCandidate Candidate;
		Private::ScanMountedAssetMetadata(Mode, Candidate);
		const std::vector<std::string> MountManifest = Private::GetMountManifest();

		std::vector<FAssetPackageReferenceEdge> ReferenceEdges;
		std::unordered_map<FPackagePath, FAssetPackageFingerprint> Fingerprints;

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
			const FAssetPackageFingerprint Fingerprint{
				.FileSize = Data->FileSize,
				.LastWriteTimeTicks = Data->LastWriteTimeTicks,
				.ReaderVersion = Data->FormatVersion};
			Fingerprints.emplace(Data->PackagePath, Fingerprint);
			auto Add = [&](EAssetReferenceKind Kind, const FPackagePath& Target)
			{
				ReferenceEdges.push_back({.SourcePackage = Data->PackagePath,
					.SourceFingerprint = Fingerprint, .Kind = Kind, .TargetPath = Target});
			};
			for (const FPackagePath& Dependency : Data->Dependencies)
				if (Data->EntryKind != EAssetRegistryEntryKind::Redirector
					|| Dependency != Data->RedirectDestination)
					Add(EAssetReferenceKind::HardObject, Dependency);
			for (const FPackagePath& Dependency : Data->SoftDependencies)
				Add(EAssetReferenceKind::SoftObject, Dependency);
			if (Data->EntryKind == EAssetRegistryEntryKind::Redirector)
				Add(EAssetReferenceKind::Redirect, Data->RedirectDestination);
		}
		std::ranges::sort(ReferenceEdges, ReferenceLess);
		ReferenceEdges.erase(std::unique(ReferenceEdges.begin(), ReferenceEdges.end(),
			[](const FAssetPackageReferenceEdge& A, const FAssetPackageReferenceEdge& B)
			{ return A.SourcePackage == B.SourcePackage && A.TargetPath == B.TargetPath && A.Kind == B.Kind; }),
			ReferenceEdges.end());
		const bool bReferenceComplete = Fingerprints.size() == Candidate.Assets.size()
			&& ReferenceEdges.size() <= MaximumReferencesPerSnapshot;

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
			.Errors = Candidate.Errors,
			.CatalogCacheWarning = Candidate.CacheWarning};
		if (!Refresh.Succeeded()) return Refresh;

		FAssetResult PublishResult = PublishAssetRegistryPublication({
			.ExpectedRevision = PriorRevision,
			.Assets = Candidate.Assets,
			.ReferenceEdges = ReferenceEdges,
			.ReferenceFingerprints = Fingerprints,
			.ReferenceErrors = {},
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
		const uint64 PublishedRevision = Refresh.ResultingRevision;
		Refresh.bCatalogCacheDirty = !Private::WriteRegistryCache(
			MountManifest, std::move(Candidate.CacheEntries),
			Refresh.CatalogCacheWarning);
		{
			FCacheOperationalState& Operational = GetCacheOperationalState();
			std::lock_guard Lock(Operational.Mutex);
			if (GetAssetCatalogRevision() == PublishedRevision)
			{
				Operational.bCatalogDirty = Refresh.bCatalogCacheDirty;
				Operational.CatalogWarning = Refresh.CatalogCacheWarning;
			}
			else
			{
				Operational.bCatalogDirty = true;
			}
		}
		return Refresh;
	}

	auto Private::MarkAssetRegistryCachesDirty() -> void
	{
		FCacheOperationalState& Operational = GetCacheOperationalState();
		std::lock_guard Lock(Operational.Mutex);
		Operational.bCatalogDirty = true;
	}

	auto FlushAssetRegistryCaches() -> void
	{
		FCacheOperationalState& Operational = GetCacheOperationalState();
		std::lock_guard Lock(Operational.Mutex);
		const FAssetRegistryPublication Publication =
			Private::GetAssetRegistryState().CapturePublication();
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
	}

	auto IsAssetRegistryCacheDirty() -> bool
	{
		FCacheOperationalState& Operational = GetCacheOperationalState();
		std::lock_guard Lock(Operational.Mutex);
		return Operational.bCatalogDirty;
	}

	auto GetAssetRegistryCacheWarning() -> std::string
	{
		FCacheOperationalState& Operational = GetCacheOperationalState();
		std::lock_guard Lock(Operational.Mutex);
		return Operational.CatalogWarning;
	}
}
