#pragma once

#include "AssetRegistryAPI.h"
#include "AssetRegistry/RegistryResult.h"
#include "DObject/AssetPath.h"
#include "Hash/XxHash.h"

namespace Durin
{
	class DClass;
}

namespace Durin::Asset
{
	enum class EAssetRegistryEntryKind : uint8
	{
		Asset = 0,
		Redirector = 1
	};

	// Describes one independently addressable package-outer export.
	struct FTopLevelAssetData
	{
		FTopLevelAssetPath AssetPath;
		std::string AssetClassName;
		FObjectPath RedirectDestination;

		auto IsRedirector() const -> bool { return RedirectDestination.IsValid(); }
		auto operator==(const FTopLevelAssetData&) const -> bool = default;
	};

	// Describes one persistent package without loading its object graph.
	struct FAssetData
	{
		FPackagePath PackagePath;
		std::string PhysicalPath;
		std::vector<FTopLevelAssetData> TopLevelAssets;
		std::string AssetClassName;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		FPackagePath RedirectDestination;
		uint32 FormatVersion = 0;
		std::vector<FPackagePath> Dependencies;
		std::vector<FPackagePath> SoftDependencies;
		std::vector<std::string> SearchableNames;
		uint64 ObjectCount = 0;
		uint64 BulkSegmentExtent = 0;
		FXxHash128 BulkSegmentDigest;
		uintmax_t FileSize = 0;
		std::filesystem::file_time_type LastWriteTime{};
		int64 LastWriteTimeTicks = 0;

		auto operator==(const FAssetData&) const -> bool = default;
	};

	// Owns one exact registry lookup and the revision against which it was made.
	struct FAssetCatalogEntry
	{
		uint64 Revision = 0;
		std::optional<FAssetData> Data;

		auto Succeeded() const -> bool { return Data.has_value(); }
		explicit operator bool() const { return Succeeded(); }
		auto operator->() const -> const FAssetData* { return &Data.value(); }
		auto operator*() const -> const FAssetData& { return Data.value(); }
		auto operator==(std::nullptr_t) const -> bool { return !Succeeded(); }
		auto operator!=(std::nullptr_t) const -> bool { return Succeeded(); }
		auto Get() const -> const FAssetData& { return Data.value(); }
	};

	struct FTopLevelAssetCatalogEntry
	{
		uint64 Revision = 0;
		std::optional<FTopLevelAssetData> Asset;
		std::optional<FAssetData> Package;

		auto Succeeded() const -> bool { return Asset.has_value() && Package.has_value(); }
		explicit operator bool() const { return Succeeded(); }
		auto operator->() const -> const FTopLevelAssetData* { return &Asset.value(); }
		auto operator==(std::nullptr_t) const -> bool { return !Succeeded(); }
		auto operator!=(std::nullptr_t) const -> bool { return Succeeded(); }
	};

	// Owns an immutable registry projection that remains valid across refreshes.
	struct FAssetCatalogSnapshot
	{
		uint64 Revision = 0;
		std::unordered_map<FPackagePath, FAssetData> Assets;

		auto FindExact(const FPackagePath& Path) const -> const FAssetData*
		{
			const auto It = Assets.find(Path);
			return It == Assets.end() ? nullptr : &It->second;
		}

		auto FindTopLevelAssetExact(const FTopLevelAssetPath& Path) const
			-> const FTopLevelAssetData*
		{
			const FAssetData* Package = FindExact(Path.GetPackagePath());
			if (!Package) return nullptr;
			const auto It = std::ranges::find(
				Package->TopLevelAssets, Path, &FTopLevelAssetData::AssetPath);
			return It == Package->TopLevelAssets.end() ? nullptr : &*It;
		}
	};

	// Owns a revision-consistent projection containing only one asset and its
	// transitive package dependencies.
	struct FAssetDependencyClosureSnapshot
	{
		uint64 Revision = 0;
		std::vector<FAssetData> Assets;
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
		CorruptRedirector
	};

	struct FAssetPathResolveOptions
	{
		const DClass* ExpectedClass = nullptr;
	};

	// Owns the complete bounded resolution result from one registry revision.
	struct FAssetPathResolveResult
	{
		EAssetPathResolveState State = EAssetPathResolveState::NotFound;
		uint64 CatalogRevision = 0;
		FPackagePath RequestedPath;
		FPackagePath FinalPath;
		std::vector<FPackagePath> RedirectChain;
		std::optional<FAssetData> FinalAssetData;

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
		std::optional<FTopLevelAssetData> FinalAssetData;
		std::optional<FAssetData> FinalPackageData;

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

	ASSETREGISTRY_API auto FindAssetExact(
		const FPackagePath& Path) -> FAssetCatalogEntry;
	ASSETREGISTRY_API auto FindTopLevelAssetExact(
		const FTopLevelAssetPath& Path) -> FTopLevelAssetCatalogEntry;
	ASSETREGISTRY_API auto ResolveAssetPath(
		const FPackagePath& Path,
		const FAssetPathResolveOptions& Options = {}) -> FAssetPathResolveResult;
	ASSETREGISTRY_API auto ResolveObjectPath(
		const FObjectPath& Path,
		const FAssetPathResolveOptions& Options = {}) -> FObjectPathResolveResult;
	ASSETREGISTRY_API auto CaptureAssetCatalogSnapshot() -> FAssetCatalogSnapshot;
	ASSETREGISTRY_API auto CaptureAssetDependencyClosure(
		const FPackagePath& Root) -> FAssetDependencyClosureSnapshot;
	ASSETREGISTRY_API auto GetAssetCatalogRevision() -> uint64;
}
