#include "Asset/AuthoredBulkStorage.h"

#include "Misc/FileHelper.h"
#include "Serialization/Archive.h"

namespace Durin::Asset
{
	namespace
	{
		constexpr uint32 Magic = 0x4b424144; // DABK
		constexpr uint32 Version = 1;
		constexpr uint32 HeaderBytes = 64;
		constexpr uint32 EntryBytes = 96;
		constexpr uint64 Alignment = 16;
		constexpr uint64 MaximumEntries = 65536;
		constexpr uint64 MaximumBytes = 1024ull * 1024 * 1024;

		struct FEntry
		{
			FAuthoredBulkDataDescriptor Descriptor;
			uint64 Offset = 0;
		};

		auto Fail(std::string* OutError, std::string Message) -> bool
		{
			if (OutError) *OutError = std::move(Message);
			return false;
		}

		auto Align(uint64 Value) -> uint64
		{
			return (Value + Alignment - 1) & ~(Alignment - 1);
		}

		auto CollectDescriptors(
			DurinCodeGen::EPropertyGenFlags Kind,
			std::span<const uint8> Payload,
			std::vector<FAuthoredBulkDataDescriptor>& Out,
			uint32 Depth,
			std::string* OutError) -> bool
		{
			if (Depth > 64) return Fail(OutError, "Authored bulk inspection exceeded the struct depth limit.");
			FCanonicalMemoryReader Reader(Payload, EArchivePurpose::BulkData);
			if (Kind == DurinCodeGen::EPropertyGenFlags::BulkData)
			{
				uint8 StorageKind = 0;
				FAuthoredBulkDataDescriptor Descriptor;
				uint64 HashLow = 0, HashHigh = 0, ContainerLow = 0, ContainerHigh = 0;
				Reader << StorageKind << Descriptor.PayloadId << Descriptor.FormatId
					<< Descriptor.FormatVersion << Descriptor.LogicalByteCount
					<< Descriptor.StoredByteCount << HashLow << HashHigh
					<< ContainerLow << ContainerHigh;
				Descriptor.ContentHash = {HashLow, HashHigh};
				Descriptor.ContainerHash = {ContainerLow, ContainerHigh};
				Descriptor.StorageKind = StorageKind == 0
					? EAuthoredBulkStorageKind::Inline : EAuthoredBulkStorageKind::External;
				if (Reader.HasError() || StorageKind > 1 || !Descriptor.PayloadId.IsValid()
					|| !Descriptor.FormatId.IsValid() || Descriptor.FormatVersion == 0
					|| Descriptor.LogicalByteCount != Descriptor.StoredByteCount)
					return Fail(OutError, "Inspected authored bulk descriptor is invalid.");
				if (Descriptor.StorageKind == EAuthoredBulkStorageKind::External)
				{
					if (Reader.Tell() != Payload.size() || Descriptor.ContainerHash.IsZero())
						return Fail(OutError, "External authored bulk descriptor has trailing bytes or no container hash.");
					Out.push_back(Descriptor);
				}
				return true;
			}
			if (Kind != DurinCodeGen::EPropertyGenFlags::Struct) return true;
			std::string StructName;
			uint64 FieldCount = 0;
			Reader << StructName << FieldCount;
			if (Reader.HasError() || FieldCount > 100000)
				return Fail(OutError, "Inspected authored struct payload header is invalid.");
			for (uint64 Index = 0; Index < FieldCount; ++Index)
			{
				std::string DeclaringType, Name, Signature;
				uint8 FieldKind = 0;
				uint64 PayloadSize = 0;
				Reader << DeclaringType << Name << FieldKind << Signature << PayloadSize;
				if (Reader.HasError() || PayloadSize > Reader.GetRemainingPayloadBytes())
					return Fail(OutError, "Inspected authored struct field is truncated.");
				std::vector<uint8> FieldPayload(static_cast<size_t>(PayloadSize));
				if (PayloadSize != 0)
					Reader.SerializeRawBytes(std::as_writable_bytes(std::span(FieldPayload)));
				if (Reader.HasError() || !CollectDescriptors(
						static_cast<DurinCodeGen::EPropertyGenFlags>(FieldKind),
						FieldPayload, Out, Depth + 1, OutError)) return false;
			}
			if (Reader.Tell() != Payload.size())
				return Fail(OutError, "Inspected authored struct payload contains trailing bytes.");
			return true;
		}

		auto Parse(std::span<const uint8> Bytes, FXxHash128 ExpectedContainerHash,
			std::vector<FEntry>& OutEntries, uint64& OutDataOffset,
			std::string* OutError) -> bool
		{
			OutEntries.clear();
			OutDataOffset = 0;
			if (Bytes.size() < HeaderBytes || Bytes.size() > MaximumBytes)
				return Fail(OutError, "Authored bulk companion size is outside the supported bound.");
			FCanonicalMemoryReader Reader(Bytes, EArchivePurpose::BulkData);
			uint32 ReadMagic = 0, ReadVersion = 0, ReadHeaderBytes = 0, ReadEntryBytes = 0;
			uint64 EntryCount = 0, DirectoryOffset = 0, DataOffset = 0;
			uint64 ContainerHashLow = 0, ContainerHashHigh = 0, Reserved = 0;
			Reader << ReadMagic << ReadVersion << ReadHeaderBytes << ReadEntryBytes
				<< EntryCount << DirectoryOffset << DataOffset
				<< ContainerHashLow << ContainerHashHigh << Reserved;
			const FXxHash128 ContainerHash{ContainerHashLow, ContainerHashHigh};
			if (Reader.HasError() || ReadMagic != Magic || ReadVersion != Version
				|| ReadHeaderBytes != HeaderBytes || ReadEntryBytes != EntryBytes
				|| EntryCount > MaximumEntries || DirectoryOffset != HeaderBytes
				|| Reserved != 0 || ContainerHash.IsZero()
				|| (!ExpectedContainerHash.IsZero() && ContainerHash != ExpectedContainerHash)
				|| EntryCount > (std::numeric_limits<uint64>::max() - HeaderBytes) / EntryBytes
				|| DataOffset != Align(HeaderBytes + EntryCount * EntryBytes)
				|| DataOffset > Bytes.size())
				return Fail(OutError, "Authored bulk companion header is invalid.");

			OutEntries.reserve(static_cast<size_t>(EntryCount));
			for (uint64 Index = 0; Index < EntryCount; ++Index)
			{
				FEntry Entry;
				uint32 Flags = 0;
				uint64 HashLow = 0, HashHigh = 0, Reserved0 = 0, Reserved1 = 0;
				Reader << Entry.Descriptor.PayloadId << Entry.Descriptor.FormatId
					<< Entry.Descriptor.FormatVersion << Flags
					<< Entry.Descriptor.LogicalByteCount << Entry.Descriptor.StoredByteCount
					<< HashLow << HashHigh << Entry.Offset << Reserved0 << Reserved1;
				Entry.Descriptor.ContentHash = {HashLow, HashHigh};
				Entry.Descriptor.ContainerHash = ContainerHash;
				Entry.Descriptor.StorageKind = EAuthoredBulkStorageKind::External;
				if (Reader.HasError() || !Entry.Descriptor.PayloadId.IsValid()
					|| !Entry.Descriptor.FormatId.IsValid() || Entry.Descriptor.FormatVersion == 0
					|| Flags != 0 || Entry.Descriptor.LogicalByteCount != Entry.Descriptor.StoredByteCount
					|| Entry.Descriptor.StoredByteCount > MaximumBytes || Entry.Offset < DataOffset
					|| Entry.Offset % Alignment != 0 || Reserved0 != 0 || Reserved1 != 0)
					return Fail(OutError, "Authored bulk companion directory entry is invalid.");
				if (!OutEntries.empty() && !(OutEntries.back().Descriptor.PayloadId < Entry.Descriptor.PayloadId))
					return Fail(OutError, "Authored bulk companion payload ids are duplicate or noncanonical.");
				OutEntries.push_back(Entry);
			}
			if (Reader.Tell() > DataOffset) return Fail(OutError, "Authored bulk directory overlaps payload data.");
			for (uint64 Offset = Reader.Tell(); Offset < DataOffset; ++Offset)
				if (Bytes[static_cast<size_t>(Offset)] != 0)
					return Fail(OutError, "Authored bulk directory padding is nonzero.");

			uint64 ExpectedOffset = DataOffset;
			for (const FEntry& Entry : OutEntries)
			{
				ExpectedOffset = Align(ExpectedOffset);
				if (Entry.Offset != ExpectedOffset || Entry.Descriptor.StoredByteCount > Bytes.size() - Entry.Offset)
					return Fail(OutError, "Authored bulk payload ranges overlap, contain gaps, or exceed the file.");
				const auto Payload = Bytes.subspan(
					static_cast<size_t>(Entry.Offset),
					static_cast<size_t>(Entry.Descriptor.StoredByteCount));
				if (FXxHash128::HashBuffer(Payload) != Entry.Descriptor.ContentHash)
					return Fail(OutError, "Authored bulk payload content hash verification failed.");
				ExpectedOffset += Entry.Descriptor.StoredByteCount;
			}
			if (ExpectedOffset != Bytes.size())
				return Fail(OutError, "Authored bulk companion contains trailing or unconsumed bytes.");
			OutDataOffset = DataOffset;
			if (OutError) OutError->clear();
			return true;
		}
	}

	auto ResolveAuthoredBulkCompanionPath(
		const std::filesystem::path& PackagePath, FXxHash128 ContainerHash,
		std::filesystem::path& OutPath, std::string* OutError) -> bool
	{
		OutPath.clear();
		if (PackagePath.extension() != ".dasset" || ContainerHash.IsZero())
			return Fail(OutError, "Authored bulk companion requires a .dasset path and valid container hash.");
		OutPath = PackagePath.parent_path()
			/ std::format("{}.{}{}", PackagePath.stem().string(),
				ContainerHash.ToString(), AuthoredBulkCompanionSuffix);
		if (OutError) OutError->clear();
		return true;
	}

	auto BuildAuthoredBulkCompanion(
		std::span<const FAuthoredBulkPayload> Payloads, FXxHash128 ContainerHash,
		std::vector<uint8>& OutBytes, std::string* OutError) -> bool
	{
		OutBytes.clear();
		if (Payloads.empty() || Payloads.size() > MaximumEntries || ContainerHash.IsZero())
			return Fail(OutError, "Authored bulk companion requires a bounded nonempty payload set and container hash.");
		std::vector<const FAuthoredBulkPayload*> Sorted;
		for (const auto& Payload : Payloads) Sorted.push_back(&Payload);
		std::ranges::sort(Sorted, {}, [](const FAuthoredBulkPayload* Payload) {
			return Payload->Descriptor.PayloadId;
		});

		uint64 DataOffset = Align(HeaderBytes + Sorted.size() * EntryBytes);
		uint64 CurrentOffset = DataOffset;
		std::vector<uint64> Offsets;
		for (const FAuthoredBulkPayload* Payload : Sorted)
		{
			const auto& Descriptor = Payload->Descriptor;
			if (!Descriptor.PayloadId.IsValid() || !Descriptor.FormatId.IsValid()
				|| Descriptor.FormatVersion == 0
				|| Descriptor.StorageKind != EAuthoredBulkStorageKind::External
				|| Descriptor.ContainerHash != ContainerHash
				|| Descriptor.LogicalByteCount != Payload->Buffer.GetSize()
				|| Descriptor.StoredByteCount != Payload->Buffer.GetSize()
				|| FXxHash128::HashBuffer(Payload->Buffer.GetBytes()) != Descriptor.ContentHash)
				return Fail(OutError, "Authored bulk companion input descriptor or bytes are invalid.");
			if (!Offsets.empty() && Sorted[Offsets.size() - 1]->Descriptor.PayloadId == Descriptor.PayloadId)
				return Fail(OutError, "Authored bulk companion contains duplicate payload ids.");
			CurrentOffset = Align(CurrentOffset);
			if (Descriptor.StoredByteCount > MaximumBytes - CurrentOffset)
				return Fail(OutError, "Authored bulk companion exceeds the 1 GiB bound.");
			Offsets.push_back(CurrentOffset);
			CurrentOffset += Descriptor.StoredByteCount;
		}

		OutBytes.reserve(static_cast<size_t>(CurrentOffset));
		FCanonicalMemoryWriter Writer(OutBytes, EArchivePurpose::BulkData);
		uint32 WriteMagic = Magic, WriteVersion = Version, WriteHeaderBytes = HeaderBytes,
			WriteEntryBytes = EntryBytes;
		uint64 Count = Sorted.size(), DirectoryOffset = HeaderBytes;
		uint64 ContainerHashLow = ContainerHash.HashLow, ContainerHashHigh = ContainerHash.HashHigh;
		uint64 Reserved = 0;
		Writer << WriteMagic << WriteVersion << WriteHeaderBytes << WriteEntryBytes
			<< Count << DirectoryOffset << DataOffset
			<< ContainerHashLow << ContainerHashHigh << Reserved;
		for (size_t Index = 0; Index < Sorted.size(); ++Index)
		{
			const auto& Descriptor = Sorted[Index]->Descriptor;
			FGuid PayloadId = Descriptor.PayloadId, FormatId = Descriptor.FormatId;
			uint32 FormatVersion = Descriptor.FormatVersion, Flags = 0;
			uint64 Logical = Descriptor.LogicalByteCount, Stored = Descriptor.StoredByteCount;
			uint64 HashLow = Descriptor.ContentHash.HashLow, HashHigh = Descriptor.ContentHash.HashHigh;
			uint64 Offset = Offsets[Index], Zero = 0;
			Writer << PayloadId << FormatId << FormatVersion << Flags << Logical << Stored
				<< HashLow << HashHigh << Offset << Zero << Zero;
		}
		while (OutBytes.size() < DataOffset) OutBytes.push_back(0);
		for (size_t Index = 0; Index < Sorted.size(); ++Index)
		{
			while (OutBytes.size() < Offsets[Index]) OutBytes.push_back(0);
			const auto Bytes = Sorted[Index]->Buffer.GetBytes();
			OutBytes.insert(OutBytes.end(), reinterpret_cast<const uint8*>(Bytes.data()),
				reinterpret_cast<const uint8*>(Bytes.data()) + Bytes.size());
		}
		if (Writer.HasError() || OutBytes.size() != CurrentOffset)
			return Fail(OutError, "Authored bulk companion encoding failed.");
		return ValidateAuthoredBulkCompanion(OutBytes, ContainerHash, OutError);
	}

	auto ValidateAuthoredBulkCompanion(
		std::span<const uint8> Bytes, FXxHash128 ExpectedContainerHash,
		std::string* OutError) -> bool
	{
		std::vector<FEntry> Entries;
		uint64 DataOffset = 0;
		return Parse(Bytes, ExpectedContainerHash, Entries, DataOffset, OutError);
	}

	auto LoadAuthoredBulkPayload(
		const std::filesystem::path& CompanionPath,
		const FAuthoredBulkDataDescriptor& Descriptor,
		FSharedByteBuffer& OutBuffer, std::string* OutError) -> bool
	{
		OutBuffer = {};
		std::vector<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, CompanionPath.generic_string()))
			return Fail(OutError, "Authored bulk companion is missing or unreadable.");
		std::vector<FEntry> Entries;
		uint64 DataOffset = 0;
		if (!Parse(Bytes, Descriptor.ContainerHash, Entries, DataOffset, OutError)) return false;
		const auto It = std::ranges::find(Entries, Descriptor.PayloadId,
			[](const FEntry& Entry) { return Entry.Descriptor.PayloadId; });
		if (It == Entries.end() || It->Descriptor != Descriptor)
			return Fail(OutError, "Authored bulk companion descriptor does not match the package reference.");
		OutBuffer = FSharedByteBuffer::Copy(std::as_bytes(std::span(Bytes)).subspan(
			static_cast<size_t>(It->Offset), static_cast<size_t>(It->Descriptor.StoredByteCount)));
		if (OutError) OutError->clear();
		return true;
	}

	auto InspectAuthoredBulkCompanionPaths(
		const std::filesystem::path& PackagePath,
		const FAssetPackageInspection& Inspection,
		std::vector<std::filesystem::path>& OutPaths,
		std::string* OutError) -> bool
	{
		OutPaths.clear();
		std::vector<FAuthoredBulkDataDescriptor> Descriptors;
		for (const FAssetPackageObjectInspection& Object : Inspection.Objects)
			for (const FAssetPackageField& Field : Object.Fields)
				if (!CollectDescriptors(Field.Kind, Field.Payload, Descriptors, 0, OutError))
					return false;
		std::ranges::sort(Descriptors, [](const auto& Left, const auto& Right) {
			return std::pair(Left.ContainerHash.HashHigh, Left.ContainerHash.HashLow)
				< std::pair(Right.ContainerHash.HashHigh, Right.ContainerHash.HashLow);
		});
		for (const FAuthoredBulkDataDescriptor& Descriptor : Descriptors)
		{
			std::filesystem::path Path;
			if (!ResolveAuthoredBulkCompanionPath(
					PackagePath, Descriptor.ContainerHash, Path, OutError)) return false;
			if (OutPaths.empty() || OutPaths.back() != Path) OutPaths.push_back(std::move(Path));
		}
		if (OutError) OutError->clear();
		return true;
	}
}
