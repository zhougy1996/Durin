#include "AssetMutationRegistryInternal.h"

namespace Durin
{
	namespace AssetPrivate
	{
		auto GetAssetReferenceStoreRegistry() -> FAssetReferenceStoreRegistry&
		{
			static FAssetReferenceStoreRegistry Registry;
			return Registry;
		}

		auto GetAssetReferenceStoreRevision() -> uint64
		{
			return GetAssetReferenceStoreRegistry().Revision;
		}

	}

	auto CaptureAssetReferenceStores(FAssetReferenceStoreCapture& OutCapture)
		-> FAssetResult
	{
		OutCapture = {};
		const auto& Registry = AssetPrivate::GetAssetReferenceStoreRegistry();
		FAssetReferenceStoreCapture Capture{.RegistryRevision = Registry.Revision};
		for (const auto& [Handle, Store] : Registry.Stores)
		{
			(void)Handle;

			if (!Store)
				return {EAssetError::StaleData, "An asset reference store is unavailable."};
			FAssetReferenceStoreSnapshot Snapshot;
			const FAssetResult Result = Store->CaptureSnapshot(Snapshot);
			if (!Result) return Result;
			Capture.Stores.push_back(std::move(Snapshot));
		}
		OutCapture = std::move(Capture);
		return {};
	}

	auto RegisterAssetReferenceStore(
		IAssetReferenceStore* Store)
		-> FAssetReferenceStoreHandle
	{
		if (!Store) return 0;
		auto& Registry = AssetPrivate::GetAssetReferenceStoreRegistry();
		const FAssetReferenceStoreHandle Handle = Registry.NextHandle++;
		Registry.Stores.emplace(Handle, Store);
		++Registry.Revision;
		return Handle;
	}

	auto UnregisterAssetReferenceStore(FAssetReferenceStoreHandle Handle) -> void
	{
		if (Handle == 0) return;
		auto& Registry = AssetPrivate::GetAssetReferenceStoreRegistry();
		if (Registry.Stores.erase(Handle) != 0) ++Registry.Revision;
	}
}
