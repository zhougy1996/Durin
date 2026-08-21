#pragma once

#include "AssetPackage.h"

namespace Durin::Asset
{
	enum class EAssetReferenceKind : uint8
	{
		HardObject,
		SoftObject,
		Redirect
	};

	// Identifies one stable route hop from an authored field to a reference value.
	enum class EAssetReferenceRouteKind : uint8
	{
		FixedArray,
		ArrayElement,
		MapValue,
		StructField
	};

	struct FAssetReferenceRouteSegment
	{
		EAssetReferenceRouteKind Kind = EAssetReferenceRouteKind::FixedArray;
		uint64 Index = 0;
		// Map values use the canonical key token that orders authored Map entries.
		std::vector<uint8> MapKeyToken;
		std::string DeclaringType;
		std::string FieldName;

		auto operator==(const FAssetReferenceRouteSegment&) const -> bool = default;
	};

	// Describes one non-null authored occurrence extracted from authoritative DAST bytes.
	struct FAssetReferenceEdge
	{
		FAssetPath SourcePackage;
		FAssetPackageFingerprint SourceFingerprint;
		uint64 SourceObjectId = 0;
		std::string SourceClass;
		std::string DeclaringType;
		std::string FieldName;
		EAssetReferenceKind Kind = EAssetReferenceKind::HardObject;
		std::string ExpectedClass;
		FAssetPath TargetPath;
		std::vector<FAssetReferenceRouteSegment> Route;
		std::string DisplayRoute;

		auto operator==(const FAssetReferenceEdge&) const -> bool = default;
	};

	// Extracts direct and recursively nested hard/soft paths without constructing package objects,
	// invoking PostLoad, resolving targets, or changing package residency.
	ASSETCORE_API auto ExtractAssetReferences(
		const FAssetPath& SourcePackage,
		const FAssetPackageInspection& Inspection,
		std::vector<FAssetReferenceEdge>& OutReferences) -> FAssetResult;

	// Owns the deterministic, fingerprint-bound derived graph for authored packages.
	class FAssetReferenceIndex
	{
	public:
		auto GetEdges() const -> std::span<const FAssetReferenceEdge> { return Edges; }
		ASSETCORE_API auto FindReferencers(const FAssetPath& Target) const
			-> std::vector<FAssetReferenceEdge>;
		ASSETCORE_API auto FindTargets(const FAssetPath& Source) const
			-> std::vector<FAssetPath>;
		auto IsComplete() const -> bool { return bComplete; }
		auto GetErrors() const -> std::span<const FAssetResult> { return Errors; }
		auto GetStats() const -> const FAssetReferenceIndexStats& { return Stats; }
		auto GetCacheWarning() const -> const std::string& { return CacheWarning; }

	private:
		std::vector<FAssetReferenceEdge> Edges;
		std::unordered_map<FAssetPath, FAssetPackageFingerprint> SourceFingerprints;
		std::vector<FAssetResult> Errors;
		FAssetReferenceIndexStats Stats;
		std::string CacheWarning;
		bool bComplete = false;
		bool bSnapshotDirty = false;

	#if defined(DURIN_ASSETCORE_INTERNAL)
		friend class FAssetCatalogStore;
		friend class FAssetMutationCoordinator;
	#endif
	};

	// Identifies one persistent path occurrence outside a .dasset package.
	ASSETCORE_API auto LoadAsset(
		const FAssetPath& Path,
		const DClass* ExpectedClass,
		DObject*& OutAsset,
		FAssetLoadReport* OutReport = nullptr) -> FAssetResult;

	template<typename T>
	auto LoadAsset(const FAssetPath& Path, T*& OutAsset, FAssetLoadReport* OutReport = nullptr) -> FAssetResult
	{
		static_assert(std::is_base_of_v<DObject, T>);
		DObject* Object = nullptr;
		FAssetResult Result = LoadAsset(
			Path, T::StaticClass(), Object, OutReport);
		OutAsset = static_cast<T*>(Object);
		return Result;
	}

	ASSETCORE_API auto ResolveSoftObject(
		FSoftObjectPtr& Reference,
		const DClass* ExpectedClass,
		ESoftObjectNullPolicy NullPolicy = ESoftObjectNullPolicy::Reject) -> FSoftObjectResolveResult;

	ASSETCORE_API auto LoadSoftObject(
		FSoftObjectPtr& Reference,
		const DClass* ExpectedClass,
		DObject*& OutObject,
		ESoftObjectNullPolicy NullPolicy = ESoftObjectNullPolicy::Reject,
		FAssetLoadReport* OutReport = nullptr) -> FAssetResult;

	template<typename T>
	auto ResolveSoftObject(
		TSoftObjectPtr<T>& Reference,
		ESoftObjectNullPolicy NullPolicy = ESoftObjectNullPolicy::Reject) -> TSoftObjectResolveResult<T>
	{
		static_assert(std::is_base_of_v<DObject, T>);
		FSoftObjectResolveResult Result = ResolveSoftObject(
			Reference.GetBase(), T::StaticClass(), NullPolicy);
		return {
			.Result = std::move(Result.Result),
			.State = Result.State,
			.Object = static_cast<T*>(Result.Object),
			.ResolvedPath = std::move(Result.ResolvedPath),
			.bRedirected = Result.bRedirected};
	}

	template<typename T>
	auto LoadSoftObject(
		TSoftObjectPtr<T>& Reference,
		T*& OutObject,
		ESoftObjectNullPolicy NullPolicy = ESoftObjectNullPolicy::Reject,
		FAssetLoadReport* OutReport = nullptr) -> FAssetResult
	{
		static_assert(std::is_base_of_v<DObject, T>);
		DObject* Object = nullptr;
		FAssetResult Result = LoadSoftObject(
			Reference.GetBase(), T::StaticClass(), Object, NullPolicy, OutReport);
		OutObject = Result ? static_cast<T*>(Object) : nullptr;
		return Result;
	}

	ASSETCORE_API auto LoadAsset(
		const FAssetPath& Path,
		DObject*& OutAsset,
		FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
	ASSETCORE_API auto FindResidentPackage(const FAssetPath& Path) -> DPackage*;
	ASSETCORE_API auto GetResidentPackagePublicationState(const FAssetPath& Path)
		-> std::optional<EAssetPackagePublicationState>;
	ASSETCORE_API auto UnloadPackage(
		const FAssetPath& Path,
		EAssetPackageUnloadPolicy Policy = EAssetPackageUnloadPolicy::RejectUnsaved)
		-> FAssetResult;
	ASSETCORE_API auto UnloadPackage(
		DPackage* Package,
		EAssetPackageUnloadPolicy Policy = EAssetPackageUnloadPolicy::RejectUnsaved)
		-> FAssetResult;
	ASSETCORE_API auto CapturePackageLoadSnapshot() -> FAssetPackageLoadSnapshot;
	ASSETCORE_API auto ReleasePackagesLoadedSince(
		const FAssetPackageLoadSnapshot& Snapshot) -> FAssetResult;
	ASSETCORE_API auto ShutdownAssetManager() -> void;
	// Reopens a shut-down runtime; a different live configuration is rejected.
	ASSETCORE_API auto InitializeAssetManager(
		FAssetRuntimeConfiguration Configuration = FAssetRuntimeConfiguration::Authored())
		-> FAssetResult;
	// Returns the fixed configuration for the current runtime lifetime.
	ASSETCORE_API auto GetAssetRuntimeConfiguration()
		-> const FAssetRuntimeConfiguration&;
	ASSETCORE_API auto CaptureAssetReferenceIndex() -> FAssetReferenceIndex;
	ASSETCORE_API auto FindRedirectorsTo(const FAssetPath& Destination)
		-> std::vector<FAssetPath>;
	ASSETCORE_API auto BuildCookReachability(
		std::span<const FAssetPath> Roots,
		std::vector<FAssetPath>& OutPackages) -> FAssetResult;
}
