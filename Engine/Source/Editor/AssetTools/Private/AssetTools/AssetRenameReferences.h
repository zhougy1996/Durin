#pragma once

#include "CoreMinimal.h"
#include "AssetRenameData.h"
#include "Asset/References.h"
#include "Asset/MutationExtensions.h"
#include "Asset/PackageEditing.h"
#include "DObject/StrongObjectPtr.h"

namespace Durin::AssetToolsPrivate
{
	struct FSoftObjectPathFixup
	{
		FSoftObjectPtr* Value = nullptr;
		FSoftObjectPath Before, After;
		DPackage* Package = nullptr;
	};

	// UE-style per-item editor record; the public rename request remains FAssetRenameData.
	struct FAssetRenameDataWithReferencers : FAssetRenameData
	{
		explicit FAssetRenameDataWithReferencers(const FAssetRenameData& Data)
			: FAssetRenameData(Data), Object(Data.Asset.Get()) {}
		TStrongObjectPtr<DObject> Object;
		std::vector<FPackagePath> ReferencingPackageNames;
		std::vector<FSoftObjectPathFixup> SoftReferences;
		bool bRenameFailed = true, bIdentityChanged = false, bCreateRedirector = false;
	};

	// One synchronous repair's shared facts. Catalog changes refresh package candidates;
	// exact occurrences are reused only while current file stamps and bytes still match.
	class FAssetReferenceFixup
	{
	public:
		auto GetRegistry() -> const FAssetRegistrySnapshot&;
		auto GetPackageReferences(const FPackageData&) -> std::expected<const std::vector<FAssetReferenceEdge>*, UnifiedError::FError>;
		auto GetStores() -> std::expected<const FAssetReferenceStoreCapture*, UnifiedError::FError>;
		auto GetStoreRegistrations() const -> const FAssetReferenceStoreRegistrations& { return StoreRegistrations; }
	private:
		struct FPackageReferences
		{
			std::string PhysicalPath;
			FAssetPackageFingerprint Fingerprint;
			std::vector<FAssetReferenceEdge> Occurrences;
		};
		std::optional<FAssetRegistrySnapshot> Registry;
		std::unordered_map<FPackagePath, FPackageReferences> Packages;
		std::optional<FAssetReferenceStoreCapture> Stores;
		FAssetReferenceStoreRegistrations StoreRegistrations;
	};

	auto CollectSoftObjectPathFixups(DPackage*, std::span<const FAssetRenameData>,
		std::vector<FSoftObjectPathFixup>&) -> std::expected<void, UnifiedError::FError>;
}
