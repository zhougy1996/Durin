#include "Asset/PackageEditing.h"
#include "AssetLiveLoadGuard.h"
#include "AssetRuntimeStateInternal.h"
#include "AssetPackageCodec.h"
#include "AssetMutationReferenceInternal.h"
#include "AssetPackageFingerprintInternal.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto Error(EAssetWriteError Code, std::string Message) -> FAssetWriteResult
		{
			return {Code, std::move(Message)};
		}
	}

	auto CheckAssetPackageMutationAllowed() -> FAssetWriteResult
	{
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("mutation", ""); !Guard)
			return AssetWriteResultFromRead(Guard);
		if (GIsGameThreadIdInitialized) CheckGameThread();
		const auto& Runtime = FAssetRuntimeState::Get();
		if (!Runtime.IsAcceptingRequests())
			return Error(EAssetWriteError::ShuttingDown, "Asset package mutation is closed while the asset manager is shutting down.");
		if (Runtime.GetRuntimeConfiguration().IsCooked())
			return Error(EAssetWriteError::ReadOnlyMode, "Cooked runtime package mode does not permit authored package mutation.");
		return {};
	}

	auto BuildRelocatedAssetPackageBytes(
		FByteView SourceBytes,
		const FPackagePath& SourcePath,
		FByteView SourceBulkBytes,
		const FPackagePath& DestinationPath,
		FByteBuffer& OutBytes) -> FAssetWriteResult
	{
		const AssetPrivate::FAssetPackageCodec* Codec = nullptr;
		if (FAssetWriteResult Result = AssetWriteResultFromRead(AssetPrivate::ResolveAssetPackageReader(
			SourceBytes, Codec)); !Result)
			return Result;
		if (!Codec->bCanMutate)
			return Error(EAssetWriteError::UnsupportedVersion,
				"Relocation requires package mutation capability.");
		AssetPrivate::FAssetPackageEncodedClosure Closure;
		if (FAssetWriteResult Result = AssetWriteResultFromEncoding(Codec->Relocate(
			{.PackageBytes = SourceBytes,
				.BulkBytes = SourceBulkBytes,
				.PackagePath = SourcePath,
				.PhysicalPackageBytes = SourceBytes.size()},
			DestinationPath, Closure)); !Result)
			return Result;
		FAssetWriteResult Result = AssetWriteResultFromRead(Codec->Validate({
			.PackageBytes = Closure.PackageBytes,
			.BulkBytes = Closure.BulkBytes,
			.PackagePath = DestinationPath,
			.PhysicalPackageBytes = Closure.PackageBytes.size()}));
		if (!Result) return Result;
		OutBytes = std::move(Closure.PackageBytes);
		return {};
	}

	auto BuildAssetRedirectorPackageBytes(
		const FPackagePath& SourcePath,
		std::span<const FAssetRedirectorWriteMapping> Mappings,
		uint32 FormatVersion,
		FByteBuffer& OutBytes) -> FAssetWriteResult
	{
		const AssetPrivate::FAssetPackageCodec* Codec =
			AssetPrivate::FindAssetPackageWriter(FormatVersion);
		if (!Codec || !Codec->bCanMutate)
			return Error(EAssetWriteError::UnsupportedVersion,
				"Redirector creation requires package mutation capability.");
		AssetPrivate::FAssetPackageEncodedClosure Closure;
		FAssetWriteResult Result = AssetWriteResultFromEncoding(Codec->WriteRedirector(
			SourcePath, Mappings, Closure));
		if (!Result) return Result;
		OutBytes = std::move(Closure.PackageBytes);
		return {};
	}

	auto RewriteAssetPackageReferences(FByteView Bytes, FByteView BulkBytes,
		const FPackagePath& PackagePath, std::span<const FAssetPackageReferenceMapping> Mappings,
		uint64 ExpectedRewriteCount, FByteBuffer& OutBytes) -> FAssetWriteResult
	{
		return AssetPrivate::RewritePackageReferencesForMutation(
			Bytes, BulkBytes, PackagePath, Mappings, ExpectedRewriteCount, OutBytes);
	}

	auto CollectLoadedAssetPackageSoftReferences(DPackage* Package,
		const FPackagePath& TargetPath, std::vector<FSoftObjectPtr*>& OutValues) -> FAssetReadResult
	{
		return AssetPrivate::CollectLoadedPackageSoftReferencesForMutation(Package, TargetPath, OutValues);
	}

	auto FingerprintAssetPackageBytes(std::string_view PhysicalPath, FByteView Bytes,
		FAssetPackageFingerprint& OutFingerprint) -> FAssetReadResult
	{
		return AssetPrivate::MakePackageFingerprint(PhysicalPath, Bytes, OutFingerprint);
	}
}
