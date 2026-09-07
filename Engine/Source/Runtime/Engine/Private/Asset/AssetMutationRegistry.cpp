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
		// Callbacks may retire registrations. Never keep a live map iterator across
		// one, and stop before dereferencing another borrowed provider after a change.
		const auto Owners = Registry.CaptureOwners;
		const auto Stores = Registry.Stores;
		for (const auto& [Handle, Store] : Stores)
		{
			(void)Handle;

			if (!Store)
				return {EAssetError::StaleData, "An asset reference store is unavailable."};
			FAssetReferenceStoreSnapshot Snapshot;
			const FAssetResult Result = Store->CaptureSnapshot(Snapshot);
			if (!Result) return Result;
			if (Registry.Revision != Capture.RegistryRevision)
				return {EAssetError::StaleData,
					"Asset reference store registrations changed during capture."};
			Capture.Stores.push_back(std::move(Snapshot));
		}
		OutCapture = std::move(Capture);
		return {};
	}

	auto RegisterAssetReferenceStore(
		IAssetReferenceStore* Store)
		-> FAssetReferenceStoreHandle
	{
		return RegisterAssetReferenceStore(Store, {});
	}

	auto RegisterAssetReferenceStore(
		IAssetReferenceStore* Store, std::shared_ptr<void> CaptureOwner)
		-> FAssetReferenceStoreHandle
	{
		if (!Store) return 0;
		auto& Registry = AssetPrivate::GetAssetReferenceStoreRegistry();
		const FAssetReferenceStoreHandle Handle = Registry.NextHandle++;
		Registry.Stores.emplace(Handle, Store);
		Registry.CaptureOwners.emplace(Handle, std::move(CaptureOwner));
		++Registry.Revision;
		return Handle;
	}

	auto UnregisterAssetReferenceStore(FAssetReferenceStoreHandle Handle) -> void
	{
		if (Handle == 0) return;
		auto& Registry = AssetPrivate::GetAssetReferenceStoreRegistry();
		if (Registry.Stores.erase(Handle) != 0) ++Registry.Revision;
		// Detach before destruction: a provider destructor may register or retire.
		auto Retired = Registry.CaptureOwners.extract(Handle);
	}
}
