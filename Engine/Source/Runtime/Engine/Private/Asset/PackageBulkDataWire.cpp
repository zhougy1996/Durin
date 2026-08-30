#include "PackageBulkDataWire.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::Asset::Private
{
	namespace
	{
		auto Fail(std::string Message, std::string* OutError) -> bool
		{
			if (OutError) *OutError = std::move(Message);
			return false;
		}
	}

	auto EncodePackageBulkDataDirectory(
		std::span<const FPackageBulkDataEntry> Entries,
		std::vector<std::byte>& OutBytes,
		std::string* OutError) -> bool
	{
		FPackageBulkSegmentSummary Synthetic;
		for (const FPackageBulkDataEntry& Entry : Entries)
			if (Entry.Placement == EPackageBulkDataPlacement::External)
				Synthetic.Extent = Entry.SegmentOffset + Entry.StoredSize;
		if (Synthetic.Extent != 0) Synthetic.Digest = {1, 1};
		if (!ValidatePackageBulkDataMetadata(Synthetic, Entries, OutError)) return false;

		FBinaryWriter Writer;
		Writer.WriteU32(PackageBulkDataDirectoryVersion);
		Writer.WriteU32(PackageBulkDataDirectoryEntryBytes);
		Writer.WriteU64(static_cast<uint64>(Entries.size()));
		for (const FPackageBulkDataEntry& Entry : Entries)
		{
			Writer.WriteU64(Entry.FieldIndex);
			Writer.WriteU32(static_cast<uint32>(Entry.Placement));
			Writer.WriteU32(Entry.StorageFlags);
			Writer.WriteU64(Entry.LogicalSize);
			Writer.WriteU64(Entry.StoredSize);
			Writer.WriteU64(Entry.SegmentOffset);
			Writer.WriteU32(Entry.Alignment);
			Writer.WriteU32(0);
			Writer.WriteHash128(Entry.ContentId);
			Writer.WriteU64(0);
		}
		OutBytes = Writer.TakeBytes();
		if (OutError) OutError->clear();
		return true;
	}

	auto DecodePackageBulkDataDirectory(
		std::span<const std::byte> Bytes,
		std::vector<FPackageBulkDataEntry>& OutEntries,
		std::string* OutError) -> bool
	{
		FBinaryReader Reader(Bytes);
		uint32 Version = 0;
		uint32 EntryBytes = 0;
		uint64 Count = 0;
		if (!Reader.ReadU32(Version) || Version != PackageBulkDataDirectoryVersion
			|| !Reader.ReadU32(EntryBytes) || EntryBytes != PackageBulkDataDirectoryEntryBytes
			|| !Reader.ReadU64(Count) || Count > PackageBulkDataMaximumFieldCount
			|| Count > Reader.GetRemainingBytes() / PackageBulkDataDirectoryEntryBytes
			|| Count * PackageBulkDataDirectoryEntryBytes != Reader.GetRemainingBytes())
			return Fail("DAST v7 Payload Directory header is malformed.", OutError);

		std::vector<FPackageBulkDataEntry> Entries;
		Entries.reserve(static_cast<size_t>(Count));
		for (uint64 Index = 0; Index < Count; ++Index)
		{
			FPackageBulkDataEntry Entry;
			uint32 Placement = 0;
			uint32 Reserved32 = 0;
			uint64 Reserved64 = 0;
			if (!Reader.ReadU64(Entry.FieldIndex)
				|| !Reader.ReadU32(Placement)
				|| !Reader.ReadU32(Entry.StorageFlags)
				|| !Reader.ReadU64(Entry.LogicalSize)
				|| !Reader.ReadU64(Entry.StoredSize)
				|| !Reader.ReadU64(Entry.SegmentOffset)
				|| !Reader.ReadU32(Entry.Alignment)
				|| !Reader.ReadU32(Reserved32) || Reserved32 != 0
				|| !Reader.ReadHash128(Entry.ContentId)
				|| !Reader.ReadU64(Reserved64) || Reserved64 != 0)
				return Fail("DAST v7 Payload Directory entry is malformed.", OutError);
			Entry.Placement = static_cast<EPackageBulkDataPlacement>(Placement);
			Entries.push_back(Entry);
		}
		OutEntries = std::move(Entries);
		if (OutError) OutError->clear();
		return true;
	}
}
