#include "Asset/PackageBulkData.h"

namespace Durin
{
	namespace
	{
		auto IsPowerOfTwo(uint32 Value) -> bool
		{
			return Value != 0 && (Value & (Value - 1)) == 0;
		}
	}

	auto FormatPackageBulkDataError(const FPackageBulkDataError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EPackageBulkDataError::None: return {};
		case EPackageBulkDataError::UnsupportedFlags: return "Package bulk segment uses unsupported flags.";
		case EPackageBulkDataError::SegmentLimit: return "Package bulk segment exceeds the 1 GiB limit.";
		case EPackageBulkDataError::DigestPresence: return "Package bulk segment extent and digest presence disagree.";
		case EPackageBulkDataError::FieldLimit: return "Package bulk field count exceeds 65,536.";
		case EPackageBulkDataError::NonCanonicalIndex: return "Package bulk field indexes are not canonical.";
		case EPackageBulkDataError::UnsupportedStorage: return "Package bulk field uses unsupported storage metadata.";
		case EPackageBulkDataError::MissingContentIdentity: return "Package bulk field has no content identity.";
		case EPackageBulkDataError::InvalidInlineMetadata: return "Inline package bulk field metadata is noncanonical.";
		case EPackageBulkDataError::InvalidPlacement: return "Package bulk field uses an unsupported placement.";
		case EPackageBulkDataError::InvalidExternalMetadata: return "External package bulk field metadata is noncanonical.";
		case EPackageBulkDataError::RangeOutsideSegment: return "External package bulk field range exceeds the segment.";
		case EPackageBulkDataError::InvalidFinalExtent: return "Package bulk segment extent is not the final declared payload byte.";
		case EPackageBulkDataError::ExtentMismatch: return "Package bulk segment extent does not match its bytes.";
		case EPackageBulkDataError::SegmentDigestMismatch: return "Package bulk segment digest does not match its bytes.";
		case EPackageBulkDataError::NonzeroPadding: return "Package bulk segment contains nonzero alignment padding.";
		case EPackageBulkDataError::FieldDigestMismatch: return "Package bulk field digest does not match its directory entry.";
		}
		return {};
	}

	auto ValidatePackageBulkDataMetadata(
		const FPackageBulkSegmentSummary& Summary,
		std::span<const FPackageBulkDataEntry> Entries) -> FPackageBulkDataResult
	{
		if (Summary.Flags != 0)
			return {.Error = {.Code = EPackageBulkDataError::UnsupportedFlags, .Summary = Summary, .Actual = Summary.Flags}};
		if (Summary.Extent > PackageBulkDataMaximumSegmentBytes)
			return {.Error = {.Code = EPackageBulkDataError::SegmentLimit, .Summary = Summary, .Actual = Summary.Extent, .Expected = PackageBulkDataMaximumSegmentBytes}};
		if ((Summary.Extent == 0) != Summary.Digest.IsZero())
			return {.Error = {.Code = EPackageBulkDataError::DigestPresence, .Summary = Summary}};
		if (Entries.size() > PackageBulkDataMaximumFieldCount)
			return {.Error = {.Code = EPackageBulkDataError::FieldLimit, .Summary = Summary, .Actual = Entries.size(), .Expected = PackageBulkDataMaximumFieldCount}};

		uint64 PreviousExternalEnd = 0;
		bool bSawExternal = false;
		for (size_t Index = 0; Index < Entries.size(); ++Index)
		{
			const FPackageBulkDataEntry& Entry = Entries[Index];
			if (Entry.FieldIndex != Index + 1)
				return {.Error = {.Code = EPackageBulkDataError::NonCanonicalIndex, .Index = Index, .Summary = Summary, .Entry = Entry, .Actual = Entry.FieldIndex, .Expected = Index + 1}};
			if (Entry.StorageFlags != 0 || Entry.StoredSize != Entry.LogicalSize)
				return {.Error = {.Code = EPackageBulkDataError::UnsupportedStorage, .Index = Index, .Summary = Summary, .Entry = Entry}};
			if (Entry.ContentId.IsZero())
				return {.Error = {.Code = EPackageBulkDataError::MissingContentIdentity, .Index = Index, .Summary = Summary, .Entry = Entry}};

			if (Entry.Placement == EPackageBulkDataPlacement::Inline)
			{
				if (Entry.SegmentOffset != 0 || Entry.Alignment != 1
					|| Entry.StoredSize > EditorBulkDataExternalThreshold)
					return {.Error = {.Code = EPackageBulkDataError::InvalidInlineMetadata, .Index = Index, .Summary = Summary, .Entry = Entry}};
				continue;
			}
			if (Entry.Placement != EPackageBulkDataPlacement::External)
				return {.Error = {.Code = EPackageBulkDataError::InvalidPlacement, .Index = Index, .Summary = Summary, .Entry = Entry}};
			const uint64 ExpectedOffset = (PreviousExternalEnd + Entry.Alignment - 1)
				& ~static_cast<uint64>(Entry.Alignment - 1);
			if (Entry.StoredSize <= EditorBulkDataExternalThreshold
				|| Entry.Alignment != EditorBulkDataExternalAlignment
				|| !IsPowerOfTwo(Entry.Alignment)
				|| Entry.SegmentOffset % Entry.Alignment != 0
				|| Entry.SegmentOffset != ExpectedOffset)
				return {.Error = {.Code = EPackageBulkDataError::InvalidExternalMetadata, .Index = Index, .Summary = Summary, .Entry = Entry, .Actual = Entry.SegmentOffset, .Expected = ExpectedOffset}};
			if (Entry.SegmentOffset > Summary.Extent
				|| Entry.StoredSize > Summary.Extent - Entry.SegmentOffset)
				return {.Error = {.Code = EPackageBulkDataError::RangeOutsideSegment, .Index = Index, .Summary = Summary, .Entry = Entry}};
			PreviousExternalEnd = Entry.SegmentOffset + Entry.StoredSize;
			bSawExternal = true;
		}
		if (bSawExternal ? PreviousExternalEnd != Summary.Extent : Summary.Extent != 0)
			return {.Error = {.Code = EPackageBulkDataError::InvalidFinalExtent, .Summary = Summary, .Actual = Summary.Extent, .Expected = PreviousExternalEnd}};
		return {};
	}

	auto ValidatePackageBulkDataSegment(
		const FPackageBulkSegmentSummary& Summary,
		std::span<const FPackageBulkDataEntry> Entries,
		FByteView Segment) -> FPackageBulkDataResult
	{
		if (auto Validation = ValidatePackageBulkDataMetadata(Summary, Entries); !Validation) return Validation;
		if (Segment.size() != Summary.Extent)
			return {.Error = {.Code = EPackageBulkDataError::ExtentMismatch, .Summary = Summary, .Actual = Segment.size(), .Expected = Summary.Extent}};
		const auto ActualDigest = Summary.Extent != 0 ? FXxHash128::HashBuffer(Segment) : FXxHash128{};
		if (Summary.Extent != 0 && ActualDigest != Summary.Digest)
			return {.Error = {.Code = EPackageBulkDataError::SegmentDigestMismatch, .Summary = Summary, .ActualDigest = ActualDigest}};

		uint64 Cursor = 0;
		for (size_t Index = 0; Index < Entries.size(); ++Index)
		{
			const auto& Entry = Entries[Index];
			if (Entry.Placement != EPackageBulkDataPlacement::External) continue;
			for (; Cursor < Entry.SegmentOffset; ++Cursor)
				if (Segment[static_cast<size_t>(Cursor)] != std::byte{0})
					return {.Error = {.Code = EPackageBulkDataError::NonzeroPadding, .Index = Index, .Summary = Summary, .Entry = Entry, .Actual = std::to_integer<uint8>(Segment[static_cast<size_t>(Cursor)]), .Offset = Cursor}};
			const auto ActualDigest = FXxHash128::HashBuffer(Segment.subspan(
				static_cast<size_t>(Entry.SegmentOffset), static_cast<size_t>(Entry.StoredSize)));
			if (ActualDigest != Entry.ContentId)
				return {.Error = {.Code = EPackageBulkDataError::FieldDigestMismatch, .Index = Index, .Summary = Summary, .Entry = Entry, .ActualDigest = ActualDigest}};
			Cursor = Entry.SegmentOffset + Entry.StoredSize;
		}
		return {};
	}
}
