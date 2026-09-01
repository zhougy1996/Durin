#pragma once

#include "AssetSubsystemFwd.h"
#include "Asset/Result.h"
#include "Asset/PackageSerialization.h"
#include "DObject/DefaultDeltaPlan.h"
#include "DObject/PackageFormat.h"

namespace Durin::Asset::Private
{
	enum class ELinkerLoadPhase : uint8
	{
		CreateSkeleton,
		ResolveDependency,
		ApplyValues,
		RestoreLedger,
		PostLoad,
		Publish,
	};

	struct FLinkerLoadOptions
	{
		std::function<bool(ELinkerLoadPhase, uint64)> ShouldFail;
		std::function<FAssetResult(DPackage*)> OnSkeletonReady;
		std::function<void(DPackage*)> OnSkeletonRollback;
		uint32 SourceFormatVersion = ObjectPackage::DastV9FormatVersion;
		bool bCooked = false;
		FArchiveTarget Target;
	};

	auto CaptureLivePackageLinker(
		DPackage* Package,
		EDefaultDeltaMode DeltaMode,
		const FAssetPackageSerializationOptions& Options,
		ObjectPackage::FLinkerTables& OutLinker,
		std::string* OutError = nullptr) -> FAssetResult;

	auto ApplyLivePackageLinker(
		ObjectPackage::FLinkerTables Linker,
		const FPackagePath& PackagePath,
		DPackage*& OutPackage,
		FAssetLoadReport* OutReport,
		const FLinkerLoadOptions& Options = {},
		std::string* OutError = nullptr) -> FAssetResult;
}
