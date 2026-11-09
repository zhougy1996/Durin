#pragma once

#include "CoreMinimal.h"

#include "AssetRegistryAPI.h"
#include "AssetRegistry/ContentChanges.h"
#include "AssetRegistry/RegistryResult.h"
#include "DObject/AssetPath.h"

namespace Durin
{
	// Describes one independently addressable package-outer export.
	struct FAssetData
	{
		FTopLevelAssetPath AssetPath;
		std::string AssetClassName;
		FObjectPath RedirectDestination;

		auto IsRedirector() const -> bool { return AssetClassName == "Durin::DAssetRedirector"; }
		auto operator==(const FAssetData&) const -> bool = default;
	};

	// Validates exact asset identity and redirect metadata independently of export order.
	template <typename TAsset>
	auto ArePackageAssetsValid(const std::vector<TAsset>& Assets,
		const FPackagePath& PackagePath, uint64 ObjectCount,
		const std::vector<FPackagePath>& Dependencies) -> bool
	{
		if (Assets.empty() || Assets.size() > ObjectCount) return false;
		std::unordered_set<FTopLevelAssetPath> Paths;
		for (const auto& Asset : Assets)
		{
			if (!Asset.AssetPath.IsValid()
				|| Asset.AssetPath.GetPackagePath() != PackagePath
				|| !Paths.insert(Asset.AssetPath).second || Asset.AssetClassName.empty()
				|| (Asset.AssetClassName == "Durin::DAssetRedirector")
					!= Asset.RedirectDestination.IsValid()) return false;
			if (Asset.RedirectDestination.IsValid()
				&& (Asset.RedirectDestination.GetAssetPath() == Asset.AssetPath
					|| (Asset.RedirectDestination.GetPackagePath() != PackagePath
						&& !std::ranges::contains(Dependencies,
							Asset.RedirectDestination.GetPackagePath()))))
				return false;
		}
		return true;
	}

	// Describes one persistent package without loading its object graph.
	struct FPackageData
	{
		FPackagePath PackagePath;
		std::string PhysicalPath;
		std::vector<FAssetData> TopLevelAssets;
		uint32 FormatVersion = 0;
		std::vector<FPackagePath> Dependencies;
		std::vector<FPackagePath> SoftDependencies;
		std::vector<FPackagePath> BuildDependencies;
		std::vector<std::string> SearchableNames;
		uint64 ObjectCount = 0;
		uint64 BulkSegmentExtent = 0;
		FXxHash128 BulkSegmentDigest;
		uintmax_t FileSize = 0;
		std::filesystem::file_time_type LastWriteTime{};
		int64 LastWriteTimeTicks = 0;

		auto operator==(const FPackageData&) const -> bool = default;
	};

	// Owns one exact registry lookup and the revision against which it was made.
	struct FPackageCatalogEntry
	{
		uint64 Revision = 0;
		std::optional<FPackageData> Data;

		auto Succeeded() const -> bool { return Data.has_value(); }
		explicit operator bool() const { return Succeeded(); }
		auto operator->() const -> const FPackageData* { return &Data.value(); }
		auto operator*() const -> const FPackageData& { return Data.value(); }
		auto operator==(std::nullptr_t) const -> bool { return !Succeeded(); }
		auto operator!=(std::nullptr_t) const -> bool { return Succeeded(); }
		auto Get() const -> const FPackageData& { return Data.value(); }
	};

	struct FAssetCatalogEntry
	{
		uint64 Revision = 0;
		std::optional<FAssetData> Asset;
		std::optional<FPackageData> Package;

		auto Succeeded() const -> bool { return Asset.has_value(); }
		explicit operator bool() const { return Succeeded(); }
		auto operator->() const -> const FAssetData* { return &Asset.value(); }
		auto operator==(std::nullptr_t) const -> bool { return !Succeeded(); }
		auto operator!=(std::nullptr_t) const -> bool { return Succeeded(); }
	};

	// Owns an immutable registry projection that remains valid across refreshes.
	struct FAssetCatalogSnapshot
	{
		uint64 Revision = 0;
		std::unordered_map<FPackagePath, FPackageData> Assets;

		auto FindExact(const FPackagePath& Path) const -> const FPackageData*
		{
			const auto It = Assets.find(Path);
			return It == Assets.end() ? nullptr : &It->second;
		}

		auto FindAssetExact(const FTopLevelAssetPath& Path) const
			-> const FAssetData*
		{
			const FPackageData* Package = FindExact(Path.GetPackagePath());
			if (!Package) return nullptr;
			const auto It = std::ranges::find(
				Package->TopLevelAssets, Path, &FAssetData::AssetPath);
			return It == Package->TopLevelAssets.end() ? nullptr : &*It;
		}
	};

	// Asset discovery includes current live metadata by default. Package metadata is
	// strictly on-disk; an unsaved asset has no FPackageData.
	enum class EAssetQuerySource : uint8 { All, OnDisk, InMemory };
	struct FAssetQuerySnapshot
	{
		uint64 Revision = 0;
		std::unordered_map<FTopLevelAssetPath, FAssetData> Assets;
		std::unordered_map<FPackagePath, FPackageData> Packages;

		auto FindAssetExact(const FTopLevelAssetPath& Path) const -> const FAssetData*
		{
			const auto It = Assets.find(Path);
			return It == Assets.end() ? nullptr : &It->second;
		}
		auto FindPackageExact(const FPackagePath& Path) const -> const FPackageData*
		{
			const auto It = Packages.find(Path);
			return It == Packages.end() ? nullptr : &It->second;
		}
	};
	// All/InMemory queries and the unified change stream belong to GameThread.
	ASSETREGISTRY_API auto CaptureAssetQuerySnapshot(EAssetQuerySource Source = EAssetQuerySource::All) -> FAssetQuerySnapshot;
	ASSETREGISTRY_API auto GetAssetQueryRevision() -> uint64;
	ASSETREGISTRY_API auto CaptureAssetQueryChanges(uint64 FromRevision) -> FContentChangeBatch;

	// Owns a revision-consistent projection containing only one asset and its
	// transitive package dependencies.
	struct FAssetDependencyClosureSnapshot
	{
		uint64 Revision = 0;
		std::vector<FPackageData> Assets;
		FAssetRegistryResult Result;

		explicit operator bool() const { return Result.Succeeded(); }
	};

	enum class EAssetPathResolveState : uint8
	{
		Resolved,
		NotFound,
		MissingRedirectTarget,
		RedirectCycle,
		RedirectDepthExceeded,
		UnknownTargetClass,
		RedirectTypeMismatch,
		CorruptRedirector,
		// A traversed package is quarantined until its registry projection is synchronized.
		ProjectionPending
	};

	// Pure metadata queries use only these owned scalar arguments.
	struct FAssetPathQueryOptions
	{
		uint32 MaximumRedirectDepth = 64;
	};

	// Owns the complete bounded resolution result from one registry revision.
	struct FPackagePathResolveResult
	{
		EAssetPathResolveState State = EAssetPathResolveState::NotFound;
		uint64 CatalogRevision = 0;
		FPackagePath RequestedPath;
		FPackagePath FinalPath;
		std::vector<FPackagePath> RedirectChain;
		std::optional<FPackageData> FinalPackageData;

		auto Succeeded() const -> bool
		{
			return State == EAssetPathResolveState::Resolved;
		}
		explicit operator bool() const { return Succeeded(); }
	};

	// Owns exact object-path redirect resolution from one registry revision.
	struct FObjectPathResolveResult
	{
		EAssetPathResolveState State = EAssetPathResolveState::NotFound;
		uint64 CatalogRevision = 0;
		FObjectPath RequestedPath;
		FObjectPath FinalPath;
		std::vector<FObjectPath> RedirectChain;
		std::optional<FAssetData> FinalAssetData;
		std::optional<FPackageData> FinalPackageData;

		auto Succeeded() const -> bool
		{
			return State == EAssetPathResolveState::Resolved;
		}
		explicit operator bool() const { return Succeeded(); }
	};

	enum class EAssetRegistryScanMode : uint8
	{
		Incremental,
		FullValidation
	};

	struct FAssetRegistryScanStats
	{
		uint64 Enumerated = 0;
		uint64 Reused = 0;
		uint64 Reparsed = 0;
		uint64 Removed = 0;
		uint64 Failed = 0;
		uint64 Redirectors = 0;
		uint64 HeaderReadAttempts = 0;
		uint64 HeaderBytesRead = 0;
		uint64 HeaderFileBytesRead = 0;
		double DurationMilliseconds = 0.0;
	};

	// Reports completeness and publication for one atomic registry refresh.
	struct FAssetCatalogRefreshResult
	{
		EAssetRegistryScanMode Mode = EAssetRegistryScanMode::Incremental;
		bool bCatalogComplete = false;
		bool bReferenceIndexComplete = false;
		bool bPublished = false;
		bool bRetainedPriorRevision = false;
		bool bCatalogCacheDirty = false;
		uint64 PriorRevision = 0;
		uint64 ResultingRevision = 0;
		FAssetRegistryScanStats CatalogStats;
		std::vector<FAssetRegistryResult> Errors;
		std::string CatalogCacheWarning;

		auto Succeeded() const -> bool
		{
			return bCatalogComplete && bReferenceIndexComplete;
		}
		explicit operator bool() const { return Succeeded(); }
	};

	ASSETREGISTRY_API auto FindPackageExact(
		const FPackagePath& Path) -> FPackageCatalogEntry;
	ASSETREGISTRY_API auto FindAssetExact(
		const FTopLevelAssetPath& Path, EAssetQuerySource Source = EAssetQuerySource::All) -> FAssetCatalogEntry;
	ASSETREGISTRY_API auto ResolvePackagePath(
		const FPackagePath& Path,
		const FAssetPathQueryOptions& Options = {}) -> FPackagePathResolveResult;
	ASSETREGISTRY_API auto ResolveAssetObjectPath(
		const FObjectPath& Path,
		const FAssetPathQueryOptions& Options = {}) -> FObjectPathResolveResult;
	ASSETREGISTRY_API auto CaptureAssetCatalogSnapshot() -> FAssetCatalogSnapshot;
	ASSETREGISTRY_API auto CaptureAssetDependencyClosure(
		const FPackagePath& Root) -> FAssetDependencyClosureSnapshot;
	ASSETREGISTRY_API auto GetAssetCatalogRevision() -> uint64;
	// Captures complete change coverage through the current revision under the catalog lock.
	ASSETREGISTRY_API auto CaptureAssetCatalogChanges(uint64 FromRevision) -> FContentChangeBatch;
}
