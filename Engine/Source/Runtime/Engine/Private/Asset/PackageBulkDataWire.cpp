#include "PackageBulkDataWire.h"

namespace Durin::Asset::Private
{
	namespace
	{
		template<typename T>
		auto WriteFixed(std::vector<std::byte>& Bytes, T Value) -> void
		{
			for (size_t Index = 0; Index < sizeof(T); ++Index)
				Bytes.push_back(static_cast<std::byte>((Value >> (Index * 8)) & 0xff));
		}

		template<typename T>
		auto ReadFixed(std::span<const std::byte> Bytes, size_t& Offset, T& OutValue) -> bool
		{
			if (Bytes.size() - Offset < sizeof(T)) return false;
			T Value = 0;
			for (size_t Index = 0; Index < sizeof(T); ++Index)
				Value |= static_cast<T>(std::to_integer<uint8>(Bytes[Offset++])) << (Index * 8);
			OutValue = Value;
			return true;
		}

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

		std::vector<std::byte> Bytes;
		Bytes.reserve(16 + Entries.size() * PackageBulkDataDirectoryEntryBytes);
		WriteFixed(Bytes, PackageBulkDataDirectoryVersion);
		WriteFixed(Bytes, PackageBulkDataDirectoryEntryBytes);
		WriteFixed(Bytes, static_cast<uint64>(Entries.size()));
		for (const FPackageBulkDataEntry& Entry : Entries)
		{
			WriteFixed(Bytes, Entry.FieldIndex);
			WriteFixed(Bytes, static_cast<uint32>(Entry.Placement));
			WriteFixed(Bytes, Entry.StorageFlags);
			WriteFixed(Bytes, Entry.LogicalSize);
			WriteFixed(Bytes, Entry.StoredSize);
			WriteFixed(Bytes, Entry.SegmentOffset);
			WriteFixed(Bytes, Entry.Alignment);
			WriteFixed(Bytes, uint32{0});
			WriteFixed(Bytes, Entry.ContentId.HashLow);
			WriteFixed(Bytes, Entry.ContentId.HashHigh);
			WriteFixed(Bytes, uint64{0});
		}
		OutBytes = std::move(Bytes);
		if (OutError) OutError->clear();
		return true;
	}

	auto DecodePackageBulkDataDirectory(
		std::span<const std::byte> Bytes,
		std::vector<FPackageBulkDataEntry>& OutEntries,
		std::string* OutError) -> bool
	{
		size_t Offset = 0;
		uint32 Version = 0;
		uint32 EntryBytes = 0;
		uint64 Count = 0;
		if (!ReadFixed(Bytes, Offset, Version) || Version != PackageBulkDataDirectoryVersion
			|| !ReadFixed(Bytes, Offset, EntryBytes) || EntryBytes != PackageBulkDataDirectoryEntryBytes
			|| !ReadFixed(Bytes, Offset, Count) || Count > PackageBulkDataMaximumFieldCount
			|| Count > (Bytes.size() - Offset) / PackageBulkDataDirectoryEntryBytes
			|| Offset + Count * PackageBulkDataDirectoryEntryBytes != Bytes.size())
			return Fail("DAST v7 Payload Directory header is malformed.", OutError);

		std::vector<FPackageBulkDataEntry> Entries;
		Entries.reserve(static_cast<size_t>(Count));
		for (uint64 Index = 0; Index < Count; ++Index)
		{
			FPackageBulkDataEntry Entry;
			uint32 Placement = 0;
			uint32 Reserved32 = 0;
			uint64 Reserved64 = 0;
			if (!ReadFixed(Bytes, Offset, Entry.FieldIndex)
				|| !ReadFixed(Bytes, Offset, Placement)
				|| !ReadFixed(Bytes, Offset, Entry.StorageFlags)
				|| !ReadFixed(Bytes, Offset, Entry.LogicalSize)
				|| !ReadFixed(Bytes, Offset, Entry.StoredSize)
				|| !ReadFixed(Bytes, Offset, Entry.SegmentOffset)
				|| !ReadFixed(Bytes, Offset, Entry.Alignment)
				|| !ReadFixed(Bytes, Offset, Reserved32) || Reserved32 != 0
				|| !ReadFixed(Bytes, Offset, Entry.ContentId.HashLow)
				|| !ReadFixed(Bytes, Offset, Entry.ContentId.HashHigh)
				|| !ReadFixed(Bytes, Offset, Reserved64) || Reserved64 != 0)
				return Fail("DAST v7 Payload Directory entry is malformed.", OutError);
			Entry.Placement = static_cast<EPackageBulkDataPlacement>(Placement);
			Entries.push_back(Entry);
		}
		OutEntries = std::move(Entries);
		if (OutError) OutError->clear();
		return true;
	}
}
