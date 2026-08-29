#pragma once

#include "AssetRegistryAPI.h"
#include "AssetRegistry/Catalog.h"
#include "AssetRegistry/PackageTypes.h"

namespace Durin::Asset
{
	enum class EAssetReferenceKind : uint8
	{
		HardObject,
		SoftObject,
		Redirect
	};

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
		std::vector<std::byte> MapKeyToken;
		std::string DeclaringType;
		std::string FieldName;

		auto operator==(const FAssetReferenceRouteSegment&) const -> bool = default;
	};

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

	class FAssetReferenceIndex
	{
	public:
		auto GetEdges() const -> std::span<const FAssetReferenceEdge> { return Edges; }
		auto GetSourceFingerprints() const
			-> const std::unordered_map<FAssetPath, FAssetPackageFingerprint>&
		{
			return SourceFingerprints;
		}
		ASSETREGISTRY_API auto FindReferencers(const FAssetPath& Target) const
			-> std::vector<FAssetReferenceEdge>;
		ASSETREGISTRY_API auto FindTargets(const FAssetPath& Source) const
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

		friend class FAssetRegistryState;
	};

	ASSETREGISTRY_API auto CaptureAssetReferenceIndex() -> FAssetReferenceIndex;
	ASSETREGISTRY_API auto FindRedirectorsTo(const FAssetPath& Destination)
		-> std::vector<FAssetPath>;
}
