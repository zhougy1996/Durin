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

		struct FHeader
		{
			uint32 Magic = 0;
			uint32 Version = 0;
			uint32 HeaderSize = 0;
			uint32 EntrySize = 0;
			uint64 EntryCount = 0;
			uint64 DirectoryOffset = 0;
			uint64 DataOffset = 0;
			FXxHash128 ContainerHash;
			uint64 Reserved = 0;
		};

		struct FWireEntry
		{
			FEntry Entry;
			uint32 Flags = 0;
			uint64 Reserved0 = 0;
			uint64 Reserved1 = 0;
		};

		auto Fail(std::string* OutError, std::string Message) -> bool
		{
			if (OutError) *OutError = std::move(Message);
			return false;
		}

		auto ReadHeader(BulkContainer::FBoundedReader& Reader, FHeader& OutHeader) -> bool
		{
			FHeader Header;
			Reader.Read(Header.Magic);
			Reader.Read(Header.Version);
			Reader.Read(Header.HeaderSize);
			Reader.Read(Header.EntrySize);
			Reader.Read(Header.EntryCount);
			Reader.Read(Header.DirectoryOffset);
			Reader.Read(Header.DataOffset);
			Reader.Read(Header.ContainerHash.HashLow);
			Reader.Read(Header.ContainerHash.HashHigh);
			Reader.Read(Header.Reserved);
			if (!Reader.IsValid()) return false;
			OutHeader = Header;
			return true;
		}

		auto WriteHeader(
			BulkContainer::FBoundedWriter& Writer,
			const FHeader& Header) -> bool
		{
			Writer.Write(Header.Magic);
			Writer.Write(Header.Version);
			Writer.Write(Header.HeaderSize);
			Writer.Write(Header.EntrySize);
			Writer.Write(Header.EntryCount);
			Writer.Write(Header.DirectoryOffset);
			Writer.Write(Header.DataOffset);
			Writer.Write(Header.ContainerHash.HashLow);
			Writer.Write(Header.ContainerHash.HashHigh);
			Writer.Write(Header.Reserved);
			return Writer.IsValid();
		}

		auto ReadEntry(
			BulkContainer::FBoundedReader& Reader,
			FXxHash128 ContainerHash,
			FWireEntry& OutEntry) -> bool
		{
			FWireEntry Candidate;
			uint64 HashLow = 0, HashHigh = 0;
			Reader.ReadGuid(Candidate.Entry.Descriptor.PayloadId);
			Reader.ReadGuid(Candidate.Entry.Descriptor.FormatId);
			Reader.Read(Candidate.Entry.Descriptor.FormatVersion);
			Reader.Read(Candidate.Flags);
			Reader.Read(Candidate.Entry.Descriptor.LogicalByteCount);
			Reader.Read(Candidate.Entry.Descriptor.StoredByteCount);
			Reader.Read(HashLow);
			Reader.Read(HashHigh);
			Reader.Read(Candidate.Entry.Offset);
			Reader.Read(Candidate.Reserved0);
			Reader.Read(Candidate.Reserved1);
			if (!Reader.IsValid()) return false;
			Candidate.Entry.Descriptor.ContentHash = {HashLow, HashHigh};
			Candidate.Entry.Descriptor.ContainerHash = ContainerHash;
			Candidate.Entry.Descriptor.StorageKind = EAuthoredBulkStorageKind::External;
			OutEntry = Candidate;
			return true;
		}

		auto WriteEntry(
			BulkContainer::FBoundedWriter& Writer,
			const FAuthoredBulkDataDescriptor& Descriptor,
			uint64 Offset) -> bool
		{
			Writer.WriteGuid(Descriptor.PayloadId);
			Writer.WriteGuid(Descriptor.FormatId);
			Writer.Write(Descriptor.FormatVersion);
			Writer.Write(uint32{0});
			Writer.Write(Descriptor.LogicalByteCount);
			Writer.Write(Descriptor.StoredByteCount);
			Writer.Write(Descriptor.ContentHash.HashLow);
			Writer.Write(Descriptor.ContentHash.HashHigh);
			Writer.Write(Offset);
			Writer.Write(uint64{0});
			Writer.Write(uint64{0});
			return Writer.IsValid();
		}

		auto IsValidHeaderIdentity(
			const FHeader& Header,
			FXxHash128 ExpectedContainerHash) -> bool
		{
			return Header.Magic == Magic && Header.Version == Version
				&& Header.HeaderSize == HeaderBytes && Header.EntrySize == EntryBytes
				&& Header.EntryCount <= MaximumEntries
				&& Header.DirectoryOffset == HeaderBytes && Header.Reserved == 0
				&& !Header.ContainerHash.IsZero()
				&& (ExpectedContainerHash.IsZero()
					|| Header.ContainerHash == ExpectedContainerHash);
		}

		auto IsValidEntry(const FWireEntry& WireEntry, uint64 DataOffset) -> bool
		{
			const FEntry& Entry = WireEntry.Entry;
			return Entry.Descriptor.PayloadId.IsValid()
				&& Entry.Descriptor.FormatId.IsValid()
				&& Entry.Descriptor.FormatVersion != 0 && WireEntry.Flags == 0
				&& Entry.Descriptor.LogicalByteCount == Entry.Descriptor.StoredByteCount
				&& Entry.Descriptor.StoredByteCount <= MaximumBytes
				&& Entry.Offset >= DataOffset && Entry.Offset % Alignment == 0
				&& WireEntry.Reserved0 == 0 && WireEntry.Reserved1 == 0;
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
			FHeader Header;
			if (!ReadHeader(Reader, Header))
				return Fail(OutError, "Authored bulk companion header is invalid.");
			uint64 DirectoryBytes = 0, DirectoryEnd = 0, ExpectedDataOffset = 0;
			if (!IsValidHeaderIdentity(Header, ExpectedContainerHash)
				|| !BulkContainer::TryMultiply(
					Header.EntryCount, EntryBytes, MaximumBytes, DirectoryBytes)
				|| !BulkContainer::TryAdd(HeaderBytes, DirectoryBytes, MaximumBytes, DirectoryEnd)
				|| !BulkContainer::TryAlignUp(DirectoryEnd, Alignment, MaximumBytes, ExpectedDataOffset)
				|| Header.DataOffset != ExpectedDataOffset
				|| Header.DataOffset > Bytes.size())
				return Fail(OutError, "Authored bulk companion header is invalid.");

			OutEntries.reserve(static_cast<size_t>(Header.EntryCount));
			std::vector<BulkContainer::FPayloadRange> Ranges;
			Ranges.reserve(static_cast<size_t>(Header.EntryCount));
			for (uint64 Index = 0; Index < Header.EntryCount; ++Index)
			{
				FWireEntry WireEntry;
				if (!ReadEntry(Reader, Header.ContainerHash, WireEntry))
					return Fail(OutError, "Authored bulk companion directory entry is invalid.");
				if (!IsValidEntry(WireEntry, Header.DataOffset))
					return Fail(OutError, "Authored bulk companion directory entry is invalid.");
				const FEntry& Entry = WireEntry.Entry;
				if (!OutEntries.empty() && !(OutEntries.back().Descriptor.PayloadId < Entry.Descriptor.PayloadId))
					return Fail(OutError, "Authored bulk companion payload ids are duplicate or noncanonical.");
				Ranges.push_back({Entry.Offset, Entry.Descriptor.StoredByteCount, Alignment});
				OutEntries.push_back(Entry);
			}
			BulkContainer::FFailure LayoutFailure;
			const BulkContainer::FLayoutPolicy LayoutPolicy{
				.MaximumCount = MaximumEntries,
				.MaximumPayloadBytes = MaximumBytes,
				.MaximumContainerBytes = MaximumBytes,
				.RequireCanonicalOffsets = true,
				.AllowTrailingZeroPadding = false};
			if (!BulkContainer::ValidateLayout(
				Bytes, Reader.Tell(), Header.DataOffset, Ranges, LayoutPolicy, &LayoutFailure))
			{
				if (LayoutFailure.Category == BulkContainer::EFailure::NonzeroPadding
					&& LayoutFailure.Offset < Header.DataOffset)
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
			OutDataOffset = Header.DataOffset;
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
			.MaximumCount = MaximumEntries,
			.MaximumPayloadBytes = MaximumBytes,
			.MaximumContainerBytes = MaximumBytes,
			.RequireCanonicalOffsets = true,
			.AllowTrailingZeroPadding = false};
		if (!BulkContainer::TryBuildLayout(
			DataOffset, LayoutItems, LayoutPolicy, Ranges, FileSize))
			return Fail(OutError, "Authored bulk companion exceeds the 1 GiB bound.");

		BulkContainer::FBoundedWriter Writer(MaximumBytes);
		const FHeader Header{
			.Magic = Magic,
			.Version = Version,
			.HeaderSize = HeaderBytes,
			.EntrySize = EntryBytes,
			.EntryCount = Sorted.size(),
			.DirectoryOffset = HeaderBytes,
			.DataOffset = DataOffset,
			.ContainerHash = ContainerHash,
			.Reserved = 0};
		if (!WriteHeader(Writer, Header))
			return Fail(OutError, "Authored bulk companion encoding failed.");
		for (size_t Index = 0; Index < Sorted.size(); ++Index)
		{
			const auto& Descriptor = Sorted[Index]->Descriptor;
			if (!WriteEntry(Writer, Descriptor, Ranges[Index].Offset))
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
