#include "Asset/AuthoredBulkStorage.h"

#include "BulkContainerInfrastructure.h"
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
			BulkContainer::FBoundedReader Reader(Bytes, MaximumBytes);
			uint32 ReadMagic = 0, ReadVersion = 0, ReadHeaderBytes = 0, ReadEntryBytes = 0;
			uint64 EntryCount = 0, DirectoryOffset = 0, DataOffset = 0;
			uint64 ContainerHashLow = 0, ContainerHashHigh = 0, Reserved = 0;
			if (!Reader.Read(ReadMagic) || !Reader.Read(ReadVersion)
				|| !Reader.Read(ReadHeaderBytes) || !Reader.Read(ReadEntryBytes)
				|| !Reader.Read(EntryCount) || !Reader.Read(DirectoryOffset)
				|| !Reader.Read(DataOffset) || !Reader.Read(ContainerHashLow)
				|| !Reader.Read(ContainerHashHigh) || !Reader.Read(Reserved))
				return Fail(OutError, "Authored bulk companion header is invalid.");
			const FXxHash128 ContainerHash{ContainerHashLow, ContainerHashHigh};
			uint64 DirectoryBytes = 0, DirectoryEnd = 0, ExpectedDataOffset = 0;
			if (ReadMagic != Magic || ReadVersion != Version
				|| ReadHeaderBytes != HeaderBytes || ReadEntryBytes != EntryBytes
				|| EntryCount > MaximumEntries || DirectoryOffset != HeaderBytes
				|| Reserved != 0 || ContainerHash.IsZero()
				|| (!ExpectedContainerHash.IsZero() && ContainerHash != ExpectedContainerHash)
				|| !BulkContainer::TryMultiply(EntryCount, EntryBytes, MaximumBytes, DirectoryBytes)
				|| !BulkContainer::TryAdd(HeaderBytes, DirectoryBytes, MaximumBytes, DirectoryEnd)
				|| !BulkContainer::TryAlignUp(DirectoryEnd, Alignment, MaximumBytes, ExpectedDataOffset)
				|| DataOffset != ExpectedDataOffset
				|| DataOffset > Bytes.size())
				return Fail(OutError, "Authored bulk companion header is invalid.");

			OutEntries.reserve(static_cast<size_t>(EntryCount));
			std::vector<BulkContainer::FPayloadRange> Ranges;
			Ranges.reserve(static_cast<size_t>(EntryCount));
			for (uint64 Index = 0; Index < EntryCount; ++Index)
			{
				FEntry Entry;
				uint32 Flags = 0;
				uint64 HashLow = 0, HashHigh = 0, Reserved0 = 0, Reserved1 = 0;
				if (!Reader.ReadGuid(Entry.Descriptor.PayloadId)
					|| !Reader.ReadGuid(Entry.Descriptor.FormatId)
					|| !Reader.Read(Entry.Descriptor.FormatVersion) || !Reader.Read(Flags)
					|| !Reader.Read(Entry.Descriptor.LogicalByteCount)
					|| !Reader.Read(Entry.Descriptor.StoredByteCount)
					|| !Reader.Read(HashLow) || !Reader.Read(HashHigh)
					|| !Reader.Read(Entry.Offset) || !Reader.Read(Reserved0)
					|| !Reader.Read(Reserved1))
					return Fail(OutError, "Authored bulk companion directory entry is invalid.");
				Entry.Descriptor.ContentHash = {HashLow, HashHigh};
				Entry.Descriptor.ContainerHash = ContainerHash;
				Entry.Descriptor.StorageKind = EAuthoredBulkStorageKind::External;
				if (!Entry.Descriptor.PayloadId.IsValid()
					|| !Entry.Descriptor.FormatId.IsValid() || Entry.Descriptor.FormatVersion == 0
					|| Flags != 0 || Entry.Descriptor.LogicalByteCount != Entry.Descriptor.StoredByteCount
					|| Entry.Descriptor.StoredByteCount > MaximumBytes || Entry.Offset < DataOffset
					|| Entry.Offset % Alignment != 0 || Reserved0 != 0 || Reserved1 != 0)
					return Fail(OutError, "Authored bulk companion directory entry is invalid.");
				if (!OutEntries.empty() && !(OutEntries.back().Descriptor.PayloadId < Entry.Descriptor.PayloadId))
					return Fail(OutError, "Authored bulk companion payload ids are duplicate or noncanonical.");
				Ranges.push_back({Entry.Offset, Entry.Descriptor.StoredByteCount, Alignment});
				OutEntries.push_back(Entry);
			}
			BulkContainer::FFailure LayoutFailure;
			const BulkContainer::FLayoutPolicy LayoutPolicy{
				MaximumEntries, MaximumBytes, MaximumBytes, true, false};
			if (!BulkContainer::ValidateLayout(
				Bytes, Reader.Tell(), DataOffset, Ranges, LayoutPolicy, &LayoutFailure))
			{
				if (LayoutFailure.Category == BulkContainer::EFailure::NonzeroPadding
					&& LayoutFailure.Offset < DataOffset)
					return Fail(OutError, "Authored bulk directory padding is nonzero.");
				if (LayoutFailure.Category == BulkContainer::EFailure::TrailingBytes)
					return Fail(OutError, "Authored bulk companion contains trailing or unconsumed bytes.");
				return Fail(OutError, "Authored bulk payload ranges overlap, contain gaps, or exceed the file.");
			}

			for (const FEntry& Entry : OutEntries)
			{
				std::span<const uint8> Payload;
				if (!BulkContainer::TryProjectRange(
					Bytes, Entry.Offset, Entry.Descriptor.StoredByteCount, Payload))
					return Fail(OutError, "Authored bulk payload ranges overlap, contain gaps, or exceed the file.");
				if (FXxHash128::HashBuffer(Payload) != Entry.Descriptor.ContentHash)
					return Fail(OutError, "Authored bulk payload content hash verification failed.");
			}
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
		if (!BulkContainer::TryMakeSortedProjection<FAuthoredBulkPayload>(
			Payloads, [](const FAuthoredBulkPayload& Payload) {
				return Payload.Descriptor.PayloadId;
			}, Sorted))
			return Fail(OutError, "Authored bulk companion contains duplicate payload ids.");

		uint64 DirectoryBytes = 0, DirectoryEnd = 0, DataOffset = 0;
		if (!BulkContainer::TryMultiply(Sorted.size(), EntryBytes, MaximumBytes, DirectoryBytes)
			|| !BulkContainer::TryAdd(HeaderBytes, DirectoryBytes, MaximumBytes, DirectoryEnd)
			|| !BulkContainer::TryAlignUp(DirectoryEnd, Alignment, MaximumBytes, DataOffset))
			return Fail(OutError, "Authored bulk companion exceeds the 1 GiB bound.");
		std::vector<BulkContainer::FLayoutItem> LayoutItems;
		LayoutItems.reserve(Sorted.size());
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
			LayoutItems.push_back({Descriptor.StoredByteCount, Alignment});
		}
		std::vector<BulkContainer::FPayloadRange> Ranges;
		uint64 FileSize = 0;
		const BulkContainer::FLayoutPolicy LayoutPolicy{
			MaximumEntries, MaximumBytes, MaximumBytes, true, false};
		if (!BulkContainer::TryBuildLayout(
			DataOffset, LayoutItems, LayoutPolicy, Ranges, FileSize))
			return Fail(OutError, "Authored bulk companion exceeds the 1 GiB bound.");

		BulkContainer::FBoundedWriter Writer(MaximumBytes);
		uint32 WriteMagic = Magic, WriteVersion = Version, WriteHeaderBytes = HeaderBytes,
			WriteEntryBytes = EntryBytes;
		uint64 Count = Sorted.size(), DirectoryOffset = HeaderBytes;
		uint64 ContainerHashLow = ContainerHash.HashLow, ContainerHashHigh = ContainerHash.HashHigh;
		uint64 Reserved = 0;
		if (!Writer.Write(WriteMagic) || !Writer.Write(WriteVersion)
			|| !Writer.Write(WriteHeaderBytes) || !Writer.Write(WriteEntryBytes)
			|| !Writer.Write(Count) || !Writer.Write(DirectoryOffset)
			|| !Writer.Write(DataOffset) || !Writer.Write(ContainerHashLow)
			|| !Writer.Write(ContainerHashHigh) || !Writer.Write(Reserved))
			return Fail(OutError, "Authored bulk companion encoding failed.");
		for (size_t Index = 0; Index < Sorted.size(); ++Index)
		{
			const auto& Descriptor = Sorted[Index]->Descriptor;
			FGuid PayloadId = Descriptor.PayloadId, FormatId = Descriptor.FormatId;
			uint32 FormatVersion = Descriptor.FormatVersion, Flags = 0;
			uint64 Logical = Descriptor.LogicalByteCount, Stored = Descriptor.StoredByteCount;
			uint64 HashLow = Descriptor.ContentHash.HashLow, HashHigh = Descriptor.ContentHash.HashHigh;
			uint64 Offset = Ranges[Index].Offset, Zero = 0;
			if (!Writer.WriteGuid(PayloadId) || !Writer.WriteGuid(FormatId)
				|| !Writer.Write(FormatVersion) || !Writer.Write(Flags)
				|| !Writer.Write(Logical) || !Writer.Write(Stored)
				|| !Writer.Write(HashLow) || !Writer.Write(HashHigh)
				|| !Writer.Write(Offset) || !Writer.Write(Zero) || !Writer.Write(Zero))
				return Fail(OutError, "Authored bulk companion encoding failed.");
		}
		if (!Writer.PadTo(DataOffset))
			return Fail(OutError, "Authored bulk companion encoding failed.");
		for (size_t Index = 0; Index < Sorted.size(); ++Index)
		{
			if (!Writer.PadTo(Ranges[Index].Offset))
				return Fail(OutError, "Authored bulk companion encoding failed.");
			const auto Bytes = Sorted[Index]->Buffer.GetBytes();
			if (!Writer.WriteBytes({reinterpret_cast<const uint8*>(Bytes.data()), Bytes.size()}))
				return Fail(OutError, "Authored bulk companion encoding failed.");
		}
		std::vector<uint8> Candidate;
		if (Writer.Tell() != FileSize || !Writer.TryTake(Candidate))
			return Fail(OutError, "Authored bulk companion encoding failed.");
		if (!ValidateAuthoredBulkCompanion(Candidate, ContainerHash, OutError)) return false;
		OutBytes = std::move(Candidate);
		return true;
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
