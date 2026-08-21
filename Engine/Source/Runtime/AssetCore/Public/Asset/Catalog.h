#pragma once

#include "AssetCoreAPI.h"
#include "Asset/Result.h"
#include "DObject/AssetPath.h"

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

	// Describes one persistent package without loading its object graph.
	struct FAssetData
	{
		FAssetPath PackagePath;
		std::string PhysicalPath;
		std::string AssetClassName;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		FAssetPath RedirectDestination;
		uint32 FormatVersion = 0;
		std::vector<FAssetPath> Dependencies;
		uintmax_t FileSize = 0;
		std::filesystem::file_time_type LastWriteTime{};
		int64 LastWriteTimeTicks = 0;

		auto operator==(const FAssetData&) const -> bool = default;
	};

	// Owns one exact catalog lookup and the revision against which it was made.
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

	// Owns an immutable catalog projection that remains valid across refreshes.
	struct FAssetCatalogSnapshot
	{
		uint64 Revision = 0;
		std::unordered_map<FAssetPath, FAssetData> Assets;

		auto FindExact(const FAssetPath& Path) const -> const FAssetData*
		{
			const auto It = Assets.find(Path);
			return It == Assets.end() ? nullptr : &It->second;
		}
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

	// Owns the complete bounded resolution result from one catalog revision.
	struct FAssetPathResolveResult
	{
		EAssetPathResolveState State = EAssetPathResolveState::NotFound;
		uint64 CatalogRevision = 0;
		FAssetPath RequestedPath;
		FAssetPath FinalPath;
		std::vector<FAssetPath> RedirectChain;
		std::optional<FAssetData> FinalAssetData;

		auto Succeeded() const -> bool
		{
			return State == EAssetPathResolveState::Resolved;
		}
		explicit operator bool() const { return Succeeded(); }
	};

	// Selects whether catalog discovery may reuse its persistent snapshot.
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

	struct FAssetReferenceIndexStats
	{
		uint64 ReusedSources = 0;
		uint64 ExtractedSources = 0;
		uint64 FailedSources = 0;
		uint64 PayloadReadAttempts = 0;
		uint64 PayloadBytesRead = 0;
	};

	// Reports completeness and publication for one atomic catalog refresh.
	struct FAssetCatalogRefreshResult
	{
		EAssetRegistryScanMode Mode = EAssetRegistryScanMode::Incremental;
		bool bCatalogComplete = false;
		bool bReferenceIndexComplete = false;
		bool bPublished = false;
		bool bRetainedPriorRevision = false;
		uint64 PriorRevision = 0;
		uint64 ResultingRevision = 0;
		FAssetRegistryScanStats CatalogStats;
		FAssetReferenceIndexStats ReferenceStats;
		std::vector<FAssetResult> Errors;
		std::string CatalogCacheWarning;
		std::string ReferenceCacheWarning;

		auto Succeeded() const -> bool
		{
			return bCatalogComplete && bReferenceIndexComplete;
		}
		explicit operator bool() const { return Succeeded(); }
	};

	ASSETCORE_API auto FindAssetExact(
		const FAssetPath& Path) -> FAssetCatalogEntry;
	ASSETCORE_API auto ResolveAssetPath(
		const FAssetPath& Path,
		const FAssetPathResolveOptions& Options = {}) -> FAssetPathResolveResult;
	ASSETCORE_API auto CaptureAssetCatalogSnapshot() -> FAssetCatalogSnapshot;
	ASSETCORE_API auto GetAssetCatalogRevision() -> uint64;
	ASSETCORE_API auto RefreshAssetCatalog(
		EAssetRegistryScanMode Mode = EAssetRegistryScanMode::Incremental)
		-> FAssetCatalogRefreshResult;
}
