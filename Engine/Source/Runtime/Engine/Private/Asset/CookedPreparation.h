#pragma once

#include "CoreMinimal.h"
#include "Asset/BulkData.h"
#include "Asset/Cook.h"
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
		std::function<bool()> ValidateInputs;
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
		auto Validate(DPackage* Package) const -> std::expected<void, FPackageCaptureError>;
		static auto Find(const DObject& Object, const FArchiveTarget& Target) -> FPreparedCookedObject*;
		static auto GetContext(DObject& Object, const FObjectCookContext& Invocation) -> FCookPreparationContext*;
	private:
		auto Finish(FCookPreparationContext& Context) -> std::expected<void, FPackageCaptureError>;
		uint64 Revision = 0;
		uint64 RetainedBytes = 0;
		FScopedCookedPreparation* Previous;
		FCookPreparationContext* Current = nullptr;
		std::vector<std::unique_ptr<FCookPreparationContext>> Operations;
		FArchiveTarget Target;
		std::unordered_map<FObjectKey, FPreparedCookedObject> Products;
	};
}
