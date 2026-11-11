#pragma once

#include "CoreMinimal.h"
#include "Asset/PackageSerialization.h"
#include "Asset/Testing/PackageSaveTesting.h"
#include "AssetRegistry/Publication.h"
#include "DObject/PackagePersistence.h"
#include "DObject/StrongObjectPtr.h"
#include "AssetRegistry/PackageHeader.h"

namespace Durin::AssetPrivate
{
	struct FPackageSaveCommitContext
	{
		Testing::FPackageSaveFault Fault;
		size_t PackageIndex = 0;
	};

	// One owner for the package pin, captured revision/header and physical writer.
	// Encoded bytes transfer directly to the writer during preparation; only the
	// immutable header remains for publication. Destroy on GameThread.
	struct FPreparedPackageSave
	{
		TStrongObjectPtr<DPackage> Package;
		std::vector<FStrongObjectPtr> Exports;
		std::vector<FObjectKey> DiskExports;
		FXxHash128 MainDigest{};
		uint64 Revision = 0;
		uint64 DetachedBytes = 0;
		FAssetPackageHeader Header;
		std::filesystem::path Destination;
		std::unique_ptr<IPackageWriteOperation> Write;
	};

	auto BuildSavedAssetMetadata(const FAssetPackageHeader& Header,
		const std::filesystem::path& Destination, uintmax_t FileSize,
		std::filesystem::file_time_type LastWriteTime) -> FPackageData;
	auto ToAssetWriteResult(FPackageWriteResult Result) -> FAssetWriteResult;

	auto ValidatePackageSaveAdmission(const FPackagePath& Path) -> FAssetWriteResult;

	// Called after staging. Revalidates live-state admission before physical commit,
	// then owns Registry publication, compensation, revision acknowledgement and finalization.
	auto CommitPreparedPackageSave(const FPreparedPackageSave& Prepared,
		const FPackageSaveCommitContext& Context,
		const FAssetRegistryPublication& Expected) -> FAssetWriteResult;
}
