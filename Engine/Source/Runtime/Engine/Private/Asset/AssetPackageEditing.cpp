#include "Materials/MaterialFunctionInterface.h"
#include "DObject/Object.h"
#include "DObject/Class.h"
#include "Asset/PackageEditing.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"
#include "AssetRegistry/Catalog.h"
#include "AssetLiveLoadGuard.h"
#include "AssetRuntimeStateInternal.h"
#include "AssetMutationReferenceInternal.h"
#include "AssetPackageFingerprintInternal.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	auto PublishAssetRenamed(DObject& Asset, const FSoftObjectPath& OldPath) -> void
	{
		FAssetData Data;
		if (!FTopLevelAssetPath::TryCreate(Asset.GetObjectPath(), Data.AssetPath)) return;
		Data.AssetClassName = Asset.GetClass()->GetQualifiedName().ToString();
		Data.PrimaryAssetId = Asset.GetPrimaryAssetId();
		Data.AssetBundleData = Asset.GetAssetBundleData();
		OnAssetRenamed().Broadcast(Data, OldPath);
		if (auto* Function = Cast<DMaterialFunctionInterface>(&Asset)) NotifyMaterialFunctionChanged(*Function);
	}

	namespace
	{
		auto Error(UnifiedError::FErrorId Code, std::string Message) -> std::expected<void, UnifiedError::FError>
		{
			return std::unexpected(UnifiedError::FError{Code, std::move(Message)});
		}
	}

	namespace AssetPrivate
	{
		auto ValidateMutationPackageMetadata(const FMutationPackageMetadata& Metadata,
			uint64 ObjectCount, const FPackagePath* SourcePath) -> std::expected<void, Durin::UnifiedError::FError>
		{
			const FPackagePath Path = SourcePath ? *SourcePath
				: (Metadata.TopLevelAssets.empty() ? FPackagePath{}
					: Metadata.TopLevelAssets.front().AssetPath.GetPackagePath());
			if (!ArePackageAssetsValid(Metadata.TopLevelAssets, Path, ObjectCount, Metadata.Dependencies))
				return std::unexpected(Durin::UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "CorruptRedirector: the package contains invalid exact asset metadata."});
			return {};
		}
	}

	auto CheckAssetPackageMutationAllowed() -> std::expected<void, UnifiedError::FError>
	{
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("mutation", ""); !Guard)
			return Guard;
		if (GIsGameThreadIdInitialized) CheckGameThread();
		const auto& Runtime = FAssetRuntimeState::Get();
		if (auto Admission = Runtime.CheckRequestAdmission(); !Admission)
			return Admission;
		if (Runtime.GetRuntimeConfiguration().IsCooked())
			return Error(UnifiedError::FErrorId{"AssetMutation", "ReadOnlyMode"}, "Cooked runtime package mode does not permit authored package mutation.");
		return {};
	}

	auto RewriteAssetPackageReferences(FByteView Bytes, FByteView BulkBytes,
		const FPackagePath& PackagePath, std::span<const FAssetPackageReferenceMapping> Mappings,
		uint64 ExpectedRewriteCount, FByteBuffer& OutBytes) -> std::expected<void, UnifiedError::FError>
	{
		return AssetPrivate::RewritePackageReferencesForMutation(
			Bytes, BulkBytes, PackagePath, Mappings, ExpectedRewriteCount, OutBytes);
	}

	auto CollectLoadedAssetPackageSoftReferences(DPackage* Package,
		const FPackagePath& TargetPath, std::vector<FSoftObjectPtr*>& OutValues) -> std::expected<void, Durin::UnifiedError::FError>
	{
		return AssetPrivate::CollectLoadedPackageSoftReferencesForMutation(Package, TargetPath, OutValues);
	}

	auto FingerprintAssetPackageBytes(std::string_view PhysicalPath, FByteView Bytes,
		FAssetPackageFingerprint& OutFingerprint) -> std::expected<void, Durin::UnifiedError::FError>
	{
		return AssetPrivate::MakePackageFingerprint(PhysicalPath, Bytes, OutFingerprint);
	}
}
