#pragma once

#include "DObject/TransactionDiff.h"

namespace Durin
{
	// Slots are ordered by the archive. A table entry is allocated on its first use.
	template<typename Visitor>
	auto VisitTransactionReferenceIndices(const FSerializedObject& Data, const FTransactionSerializationContext& Context,
		Visitor&& Visit) -> FTransactionCaptureResult
	{
		size_t HardCount = 0, WeakCount = 0, End = 0;
		const FByteView Bytes(*Data.SerializedData);
		for (const auto& Slot : Context.ReferenceSlots)
		{
			if (Slot.Offset < End || Slot.Offset > Bytes.size() || Bytes.size() - Slot.Offset < sizeof(int32))
				return std::unexpected("Transaction reference slot exceeds the serialized layout.");
			End = Slot.Offset + sizeof(int32);
			int32 Index;
			std::memcpy(&Index, Bytes.data() + Slot.Offset, sizeof(Index));
			if constexpr (std::endian::native != std::endian::little) Index = std::byteswap(Index);
			auto& Count = Slot.bWeak ? WeakCount : HardCount;
			const auto Size = Slot.bWeak ? Data.WeakReferencedObjects.size() : Data.ReferencedObjects.size();
			if (Index < -1 || (Index >= 0 && (size_t(Index) >= Size || size_t(Index) > Count)))
				return std::unexpected("Transaction reference index is invalid.");
			if (Index >= 0 && size_t(Index) == Count) ++Count;
			Visit(Slot, Index);
		}
		if (HardCount != Data.ReferencedObjects.size() || WeakCount != Data.WeakReferencedObjects.size())
			return std::unexpected("Transaction reference layout does not cover its tables.");
		return {};
	}
}

namespace Durin::DiffUtil
{
	auto GetArchetypeDiffableObject(const FDiffableObject& Restored) -> std::expected<FDiffableObject, std::string>;
}
