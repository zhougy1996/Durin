#pragma once

#include "AssetRegistry/ObjectStream.h"

namespace Durin::Asset::PackageObjectStream
{
	enum class EReferenceRouteKind : uint8
	{
		FixedArray,
		ArrayElement,
		MapValue,
		StructField
	};

	struct FReferenceRouteSegment
	{
		EReferenceRouteKind Kind = EReferenceRouteKind::FixedArray;
		uint64 Index = 0;
		std::vector<std::byte> MapKeyToken;
		std::string DeclaringType;
		std::string FieldName;
	};

	struct FReferenceOccurrence
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
		std::vector<FReferenceRouteSegment> Route;
		std::string DisplayRoute;
	};

	auto ExtractReferences(std::span<const std::byte> Bytes,
		const FAssetPath& SourcePackage,
		std::vector<FReferenceOccurrence>& OutReferences,
		const FReaderLimits& Limits = {},
		FReaderDiagnostic* OutDiagnostic = nullptr) -> FAssetResult;
}
