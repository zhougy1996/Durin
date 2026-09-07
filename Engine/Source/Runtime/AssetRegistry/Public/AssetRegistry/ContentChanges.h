#pragma once

#include "AssetRegistryAPI.h"

namespace Durin
{
	// Paths describe committed effects, not attempted operations. Empty paths denote absence.
	enum class EContentChangeKind : uint8 { Added, Modified, Removed, Renamed };
	struct FContentChange
	{
		EContentChangeKind Kind = EContentChangeKind::Modified;
		std::string OldPhysicalPath;
		std::string NewPhysicalPath;
		std::string OldAssetPath;
		std::string NewAssetPath;
		bool bDirectory = false;
	};

	// Ordered changes spanning (FromRevision, ToRevision]; unknown coverage requires a rescan.
	struct FContentChangeBatch
	{
		uint64 FromRevision = 0;
		uint64 ToRevision = 0;
		bool bFullRefresh = false;
		std::vector<FContentChange> Changes;
	};

	// Owner synchronizes access. Readers have independent cursors and never consume entries.
	class FContentChangeJournal
	{
	public:
		explicit FContentChangeJournal(size_t Capacity = 256, size_t MaxChanges = 16384)
			: Capacity(Capacity), MaxChanges(MaxChanges) {}
		auto Append(FContentChangeBatch Batch) -> void
		{
			if (Batch.Changes.size() > MaxChanges) Batch.bFullRefresh = true;
			if (Batch.bFullRefresh) Batch.Changes.clear();
			Batches.push_back(std::move(Batch));
			while (Batches.size() > Capacity) Batches.erase(Batches.begin());
		}
		auto Read(uint64 From, uint64 To) const -> FContentChangeBatch
		{
			FContentChangeBatch Result{From, To};
			if (From == To) return Result;
			uint64 Cursor = From;
			for (const auto& Batch : Batches)
			{
				if (Batch.ToRevision <= Cursor) continue;
				if (Batch.FromRevision != Cursor || Batch.ToRevision > To || Batch.bFullRefresh) break;
				Result.Changes.insert(Result.Changes.end(), Batch.Changes.begin(), Batch.Changes.end());
				Cursor = Batch.ToRevision;
				if (Result.Changes.size() > MaxChanges) break;
				if (Cursor == To) return Result;
			}
			Result.bFullRefresh = true;
			Result.Changes.clear();
			return Result;
		}
	private:
		size_t Capacity;
		size_t MaxChanges;
		std::vector<FContentChangeBatch> Batches;
	};
}
