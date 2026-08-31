#pragma once

#include "Asset/PackageObjectStreamReader.h"
#include "Asset/PackageSerialization.h"
#include "DObject/PackageFormat.h"

namespace Durin::Asset::Private
{
	auto CaptureLivePackageLinker(
		DPackage* Package,
		EDefaultDeltaMode DeltaMode,
		const FAssetPackageSerializationOptions& Options,
		ObjectPackage::FLinkerTables& OutLinker,
		std::string* OutError = nullptr) -> FAssetResult;

	auto ApplyLivePackageLinker(
		const ObjectPackage::FLinkerTables& Linker,
		const FPackagePath& PackagePath,
		DPackage*& OutPackage,
		FAssetLoadReport* OutReport,
		const PackageObjectStream::FLiveLoadOptions& Options = {},
		std::string* OutError = nullptr) -> FAssetResult;
}
