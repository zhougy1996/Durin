#pragma once

#include "CoreMinimal.h"
#include "Asset/BulkData.h"
#include "CookRegistry.h"
#include "Asset/PackageSerialization.h"
#include "DObject/ObjectKey.h"
#include "DObject/PackageCapture.h"

namespace Durin::AssetPrivate
{
	// Owns custom-field payloads for one synchronous package capture, never live asset state.
	struct FPreparedCookedObject
	{
		FBulkData Primary;
		FBulkData Collision;
		uint64 PackageRevision = 0;
	};

	// Nested captures have independent tables. Only exact target/revision/handle matches are readable.
	class FScopedCookedPreparation
	{
	public:
		FScopedCookedPreparation();
		~FScopedCookedPreparation();
		FScopedCookedPreparation(const FScopedCookedPreparation&) = delete;
		auto operator=(const FScopedCookedPreparation&) -> FScopedCookedPreparation& = delete;
		auto Prepare(DPackage* Package, const FAssetPackageSerializationOptions& Options) -> std::expected<void, FPackageCaptureError>;
		static auto Find(const DObject& Object, const FArchiveTarget& Target) -> FPreparedCookedObject*;
		auto GetSnapshot() const -> const std::shared_ptr<const FCookRegistrySnapshot>& { return Snapshot; }
	private:
		std::unique_ptr<FEngineCookRegistrationScope> Builtins;
		std::shared_ptr<const FCookRegistrySnapshot> Snapshot;
		FScopedCookedPreparation* Previous;
		FArchiveTarget Target;
		std::unordered_map<FObjectKey, FPreparedCookedObject> Products;
	};
}
