#pragma once

#include "AssetMutationRegistryInternal.h"

namespace Durin
{
	namespace AssetPrivate
	{
		auto InspectAssetCompanionFilesForDeletion(
			const FAssetData& Data,
			std::vector<std::filesystem::path>& OutFiles) -> FAssetResult;
	}

	// Retains the exact catalog and external-reference state confirmed for one
	// irreversible deletion command.
	struct FAssetDeletionJob::FState
	{
		uint64 RegistryRevision = 0;
		uint64 ReferenceStoreRevision = 0;
		std::vector<FAssetDeletionBatchEntry> Entries;
		std::vector<FAssetDeletionBatchWarning> Warnings;
		std::vector<std::filesystem::path> PhysicalRoots;
		bool bDeleted = false;
	};
}
