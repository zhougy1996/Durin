#pragma once

#include "CoreMinimal.h"
#include "Asset/PackageSerialization.h"
#include "AssetRegistry/Publication.h"
#include "DObject/PackagePersistence.h"
#include "DObject/StrongObjectPtr.h"
#include "AssetRegistry/PackageHeader.h"

namespace Durin::AssetPrivate
{
	// One owner for the package pin, captured revision/header and physical writer.
	// Encoded bytes transfer directly to the writer during preparation; only the
	// immutable header remains for publication. Destroy on GameThread.
	struct FPreparedPackageSave
	{
		TStrongObjectPtr<DPackage> Package;
		uint64 Revision = 0;
		uint64 DetachedBytes = 0;
		FAssetPackageHeader Header;
		std::filesystem::path Destination;
		std::unique_ptr<IPackageWriteOperation> Write;
	};

	auto BuildSavedAssetMetadata(const FAssetPackageHeader& Header,
		const std::filesystem::path& Destination, uintmax_t FileSize,
		std::filesystem::file_time_type LastWriteTime) -> FAssetData;
	auto ToAssetWriteResult(FPackageWriteResult Result) -> FAssetWriteResult;

	auto ValidatePackageSaveAdmission(const FPackagePath& Path) -> FAssetWriteResult;

	// Called after staging. Revalidates live-state admission before physical commit,
	// then owns Registry publication, compensation, revision acknowledgement and finalization.
	auto CommitPreparedPackageSave(const FPreparedPackageSave& Prepared,
		const FAssetBundleSaveOptions& Options,
		const FAssetRegistryPublication& Expected) -> FAssetWriteResult;
}
