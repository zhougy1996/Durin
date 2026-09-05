#include "Asset/PackageBulkData.h"

namespace Durin
{
	namespace
	{
		auto PackageBulkDataFail(std::string Message, std::string* OutError) -> bool
		{
			if (OutError) *OutError = std::move(Message);
			return false;
		}

		auto IsPowerOfTwo(uint32 Value) -> bool
		{
			return Value != 0 && (Value & (Value - 1)) == 0;
		}
	}

	auto ValidatePackageBulkDataMetadata(
		const FPackageBulkSegmentSummary& Summary,
		std::span<const FPackageBulkDataEntry> Entries,
		std::string* OutError) -> bool
	{
		if (Summary.Flags != 0)
			return PackageBulkDataFail("Package bulk segment uses unsupported flags.", OutError);
		if (Summary.Extent > PackageBulkDataMaximumSegmentBytes)
			return PackageBulkDataFail("Package bulk segment exceeds the 1 GiB limit.", OutError);
		if ((Summary.Extent == 0) != Summary.Digest.IsZero())
			return PackageBulkDataFail("Package bulk segment extent and digest presence disagree.", OutError);
		if (Entries.size() > PackageBulkDataMaximumFieldCount)
			return PackageBulkDataFail("Package bulk field count exceeds 65,536.", OutError);

		uint64 PreviousExternalEnd = 0;
		bool bSawExternal = false;
		for (size_t Index = 0; Index < Entries.size(); ++Index)
		{
			const FPackageBulkDataEntry& Entry = Entries[Index];
			if (Entry.FieldIndex != Index + 1)
				return PackageBulkDataFail("Package bulk field indexes are not canonical.", OutError);
			if (Entry.StorageFlags != 0 || Entry.StoredSize != Entry.LogicalSize)
				return PackageBulkDataFail("Package bulk field uses unsupported storage metadata.", OutError);
			if (Entry.ContentId.IsZero())
				return PackageBulkDataFail("Package bulk field has no content identity.", OutError);

			if (Entry.Placement == EPackageBulkDataPlacement::Inline)
			{
				if (Entry.SegmentOffset != 0 || Entry.Alignment != 1
					|| Entry.StoredSize > EditorBulkDataExternalThreshold)
					return PackageBulkDataFail("Inline package bulk field metadata is noncanonical.", OutError);
				continue;
			}
			if (Entry.Placement != EPackageBulkDataPlacement::External)
				return PackageBulkDataFail("Package bulk field uses an unsupported placement.", OutError);
			const uint64 ExpectedOffset = (PreviousExternalEnd + Entry.Alignment - 1)
				& ~static_cast<uint64>(Entry.Alignment - 1);
			if (Entry.StoredSize <= EditorBulkDataExternalThreshold
				|| Entry.Alignment != EditorBulkDataExternalAlignment
				|| !IsPowerOfTwo(Entry.Alignment)
				|| Entry.SegmentOffset % Entry.Alignment != 0
				|| Entry.SegmentOffset != ExpectedOffset)
				return PackageBulkDataFail("External package bulk field metadata is noncanonical.", OutError);
			if (Entry.SegmentOffset > Summary.Extent
				|| Entry.StoredSize > Summary.Extent - Entry.SegmentOffset)
				return PackageBulkDataFail("External package bulk field range exceeds the segment.", OutError);
			PreviousExternalEnd = Entry.SegmentOffset + Entry.StoredSize;
			bSawExternal = true;
		}
		if (bSawExternal ? PreviousExternalEnd != Summary.Extent : Summary.Extent != 0)
			return PackageBulkDataFail("Package bulk segment extent is not the final declared payload byte.", OutError);
		if (OutError) OutError->clear();
		return true;
	}

	auto ValidatePackageBulkDataSegment(
		const FPackageBulkSegmentSummary& Summary,
		std::span<const FPackageBulkDataEntry> Entries,
		FByteView Segment,
		std::string* OutError) -> bool
	{
		if (!ValidatePackageBulkDataMetadata(Summary, Entries, OutError)) return false;
		if (Segment.size() != Summary.Extent)
			return PackageBulkDataFail("Package bulk segment extent does not match its bytes.", OutError);
		if (Summary.Extent != 0 && FXxHash128::HashBuffer(Segment) != Summary.Digest)
			return PackageBulkDataFail("Package bulk segment digest does not match its bytes.", OutError);

		uint64 Cursor = 0;
		for (const FPackageBulkDataEntry& Entry : Entries)
		{
			if (Entry.Placement != EPackageBulkDataPlacement::External) continue;
			for (; Cursor < Entry.SegmentOffset; ++Cursor)
				if (Segment[static_cast<size_t>(Cursor)] != std::byte{0})
					return PackageBulkDataFail("Package bulk segment contains nonzero alignment padding.", OutError);
			if (FXxHash128::HashBuffer(Segment.subspan(
					static_cast<size_t>(Entry.SegmentOffset),
					static_cast<size_t>(Entry.StoredSize))) != Entry.ContentId)
				return PackageBulkDataFail("Package bulk field digest does not match its directory entry.", OutError);
			Cursor = Entry.SegmentOffset + Entry.StoredSize;
		}
		if (OutError) OutError->clear();
		return true;
	}
}
