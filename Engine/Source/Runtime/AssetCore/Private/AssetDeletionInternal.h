#pragma once

#include "AssetMutationRegistryInternal.h"

namespace Durin::Asset
{
	namespace Private
	{
		auto InspectAssetCompanionFilesForDeletion(
			const FAssetData& Data,
			std::vector<std::filesystem::path>& OutFiles) -> FAssetResult;
	}

	// Retains the exact catalog and external-reference state confirmed by an
	// authoring deletion until its physical transition is committed or undone.
	struct FAssetDeletionTransaction::FState
	{
		uint64 RegistryRevision = 0;
		uint64 ReferenceStoreRevision = 0;
		std::vector<FAssetDeletionBatchEntry> Entries;
		std::vector<FAssetDeletionBatchWarning> Warnings;
		std::vector<std::filesystem::path> PhysicalRoots;
		EAssetMutationTransactionState TransactionState =
			EAssetMutationTransactionState::Prepared;
	};
}
