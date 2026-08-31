#include "AssetMutationRegistryInternal.h"

namespace Durin::Asset
{
	namespace Private
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

		auto AppendRegisteredReferenceStoreDeletionProjection(
			std::span<const FPackagePath> Paths,
			std::vector<FAssetDeletionBatchWarning>& OutWarnings,
			std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> void
		{
			if (Paths.empty()) return;
			for (const auto& [Handle, Entry] :
				 GetAssetReferenceStoreRegistry().Stores)
			{
				(void)Handle;
				FAssetReferenceStoreSnapshot Snapshot;
				auto Call = Entry.OwnerGate.TryEnter();
				const bool bAdmitted = !Entry.OwnerGate.IsValid() || Call;
				const FAssetResult SnapshotResult = Entry.Store && bAdmitted
					? Entry.Store->CaptureSnapshot(Snapshot)
					: FAssetResult{
						EAssetError::StaleData,
						"A registered asset reference store is unavailable."};
				if (!SnapshotResult)
				{
					OutBlockers.push_back({
						.Kind = EAssetDeletionBatchBlocker::ReferenceStoreInspectionFailed,
						.AssetPath = Paths.front(),
						.Details = std::format(
							"A persistent asset-reference owner could not be inspected: {}",
							SnapshotResult.Message)});
					continue;
				}
				for (const FPackagePath& Path : Paths)
				{
					std::vector<std::string> Occurrences;
					for (const FAssetReferenceStoreOccurrence& Occurrence :
						 Snapshot.Occurrences)
					{
						if (Occurrence.TargetPath != Path) continue;
						Occurrences.push_back(std::format(
							"{}:{} ({})",
							Occurrence.ProviderId,
							Occurrence.StableId,
							Occurrence.DisplayRoute));
					}
					std::ranges::sort(Occurrences);
					if (Occurrences.empty()) continue;
					const size_t OccurrenceCount = Occurrences.size();
					OutWarnings.push_back({
						.TargetPath = Path,
						.ExternalOccurrences = std::move(Occurrences),
						.Details = std::format(
							"Deleting {} leaves {} persistent external owner occurrence(s) dangling. Run Fix Up Redirectors or update those owners before confirming.",
							Path.ToString(), OccurrenceCount)});
				}
			}
		}
	}

	auto RegisterAssetReferenceStore(
		IAssetReferenceStore* Store,
		FModuleOwnedCallbackGate OwnerGate)
		-> FAssetReferenceStoreHandle
	{
		auto Call = OwnerGate.TryEnter();
		if (OwnerGate.IsValid() && !Call) return 0;
		if (!Store) return 0;
		auto& Registry = Private::GetAssetReferenceStoreRegistry();
		const FAssetReferenceStoreHandle Handle = Registry.NextHandle++;
		FModuleOwnedResourceLease Resource = OwnerGate.RetainResource();
		if (OwnerGate.IsValid() && !Resource) return 0;
		Registry.Stores.emplace(
			Handle,
			Private::FAssetReferenceStoreRegistry::FEntry{
				std::move(Resource), Store, std::move(OwnerGate)});
		++Registry.Revision;
		return Handle;
	}

	auto UnregisterAssetReferenceStore(FAssetReferenceStoreHandle Handle) -> void
	{
		if (Handle == 0) return;
		auto& Registry = Private::GetAssetReferenceStoreRegistry();
		if (Registry.Stores.erase(Handle) != 0) ++Registry.Revision;
	}
}
