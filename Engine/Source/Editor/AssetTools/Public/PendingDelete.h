#pragma once
#include "CoreMinimal.h"
#include "AssetToolsAPI.h"
#include "DObject/ReferencerInformation.h"
#include "DObject/StrongObjectPtr.h"
#include "AssetRegistry/PackageTypes.h"

namespace Durin
{
	class ASSETTOOLS_API FPendingDelete
	{
	public:
		explicit FPendingDelete(DObject* InObject);
		auto GetObject() const -> DObject* { return Object.Get(); }
		auto IsInternal() const -> bool { return bInternal; }
		auto IsInternal(bool Value) -> void { bInternal = Value; }
		auto IsReferencedByNativeOwner() const -> bool { return bNativeOwner; }
		auto CheckForReferences(std::span<DObject* const> ObjectsToDelete = {},
			std::span<const FStrongObjectPtr* const> ModelOwners = {}) -> void;
		std::vector<FPackagePath> DiskReferences;
		FReferencerInformationList MemoryReferences;
		int32 RemainingDiskReferences = 0;
		int32 RemainingMemoryReferences = 0;
	private:
		friend class FAssetDeleteModel;
		FStrongObjectPtr Object;
		bool bNativeOwner = false, bInternal = false;
	};
}
