#pragma once

#include "EngineAPI.h"
#include "AssetRegistry/References.h"
#include "AssetRegistry/Catalog.h"
#include "Asset/PackageInspection.h"

namespace Durin::Asset
{
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
		FByteArray MapKeyToken;
		std::string DeclaringType;
		std::string FieldName;

		auto operator==(const FAssetReferenceRouteSegment&) const -> bool = default;
	};

	// Transient exact object/property occurrence used only by explicit Engine tooling.
	struct FAssetReferenceEdge
	{
		FPackagePath SourcePackage;
		FAssetPackageFingerprint SourceFingerprint;
		uint64 SourceObjectId = 0;
		std::string SourceClass;
		std::string DeclaringType;
		std::string FieldName;
		EAssetReferenceKind Kind = EAssetReferenceKind::HardObject;
		std::string ExpectedClass;
		FObjectPath TargetPath;
		std::vector<FAssetReferenceRouteSegment> Route;
		std::string DisplayRoute;

		auto operator==(const FAssetReferenceEdge&) const -> bool = default;
	};

	ENGINE_API auto ExtractAssetReferences(
		const FPackagePath& SourcePackage,
		const FAssetPackageInspection& Inspection,
		std::vector<FAssetReferenceEdge>& OutReferences
	) -> FAssetResult;

	ENGINE_API auto BuildCookReachability(
		std::span<const FPackagePath> Roots,
		std::vector<FPackagePath>& OutPackages
	) -> FAssetResult;
	ENGINE_API auto BuildCookReachability(
		const FAssetRegistrySnapshot& RegistrySnapshot,
		std::span<const FPackagePath> Roots,
		std::vector<FPackagePath>& OutPackages
	) -> FAssetResult;
} // namespace Durin::Asset
