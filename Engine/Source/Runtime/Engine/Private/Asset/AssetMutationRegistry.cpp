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
		for (const auto& [Handle, Entry] : Registry.Stores)
		{
			(void)Handle;
			auto Call = Entry.OwnerGate.TryEnter();
			if (!Entry.Store || (Entry.OwnerGate.IsValid() && !Call))
				return {EAssetError::StaleData, "An asset reference store is unavailable."};
			FAssetReferenceStoreSnapshot Snapshot;
			const FAssetResult Result = Entry.Store->CaptureSnapshot(Snapshot);
			if (!Result) return Result;
			Capture.Stores.push_back(std::move(Snapshot));
		}
		OutCapture = std::move(Capture);
		return {};
	}

	auto RegisterAssetReferenceStore(
		IAssetReferenceStore* Store,
		FModuleOwnedCallbackGate OwnerGate)
		-> FAssetReferenceStoreHandle
	{
		auto Call = OwnerGate.TryEnter();
		if (OwnerGate.IsValid() && !Call) return 0;
		if (!Store) return 0;
		auto& Registry = AssetPrivate::GetAssetReferenceStoreRegistry();
		const FAssetReferenceStoreHandle Handle = Registry.NextHandle++;
		FModuleOwnedResourceLease Resource = OwnerGate.RetainResource();
		if (OwnerGate.IsValid() && !Resource) return 0;
		Registry.Stores.emplace(
			Handle,
			AssetPrivate::FAssetReferenceStoreRegistry::FEntry{
				std::move(Resource), Store, std::move(OwnerGate)});
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
