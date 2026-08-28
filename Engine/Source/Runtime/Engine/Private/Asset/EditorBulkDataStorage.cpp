#include "Asset/EditorBulkDataStorage.h"

#include "BulkContainerInfrastructure.h"
#include "Asset/PackageVersionPolicy.h"
#include "Misc/FileHelper.h"
#include "Serialization/Archive.h"
#include "Serialization/BinaryEnvelope.h"

namespace Durin::Asset
{
	namespace
	{
		constexpr uint64 Alignment = 16;
		constexpr uint64 MaximumEntries = 65536;
		constexpr uint64 MaximumBytes = 1024ull * 1024 * 1024;
		constexpr uint32 DurfVersion = 2;
		constexpr uint32 DurfFormatHeaderBytes = 64;
		constexpr uint32 DurfEntryBytes = 64;
		constexpr uint64 DurfFixedHeaderBytes = BinaryEnvelopePreambleBytes + DurfFormatHeaderBytes;
		constexpr FBinaryEnvelopeLimits DurfLimits{8ull * 1024 * 1024, MaximumBytes};

		struct FEntry
		{
			FEditorBulkDataStorageDescriptor Descriptor;
			uint64 Offset = 0;
		};

		struct FDurfHeader
		{
			uint32 HeaderSize = 0;
			uint32 EntrySize = 0;
			uint32 Flags = 0;
			uint32 Reserved0 = 0;
			uint64 EntryCount = 0;
			uint64 DirectoryOffset = 0;
			uint64 DataOffset = 0;
			FXxHash128 ContainerHash;
			uint64 Reserved1 = 0;
		};

		auto CollectDescriptors(
			DurinCodeGen::EPropertyGenFlags Kind,
			std::span<const std::byte> Payload,
			std::vector<FEditorBulkDataStorageDescriptor>& Out,
			uint32 Depth,
			std::string* OutError) -> bool
		{
			if (Depth > 64) return Fail("Authored bulk inspection exceeded the struct depth limit.", OutError);
			FCanonicalMemoryReader Reader(Payload, EArchivePurpose::BulkData);
			if (Kind == DurinCodeGen::EPropertyGenFlags::BulkData)
			{
				uint8 StorageKind = 0;
				FEditorBulkDataStorageDescriptor Descriptor;
				FGuid ReservedIdentity;
				uint32 ReservedVersion = 0;
				uint64 HashLow = 0, HashHigh = 0, ContainerLow = 0, ContainerHigh = 0;
				Reader << StorageKind << Descriptor.PayloadId << ReservedIdentity
					<< ReservedVersion << Descriptor.LogicalByteCount
					<< Descriptor.StoredByteCount << HashLow << HashHigh
					<< ContainerLow << ContainerHigh;
				Descriptor.ContentHash = {HashLow, HashHigh};
				Descriptor.ContainerHash = {ContainerLow, ContainerHigh};
				Descriptor.StorageKind = StorageKind == 0
					? EEditorBulkDataStorageKind::Inline : EEditorBulkDataStorageKind::External;
				if (Reader.HasError() || StorageKind > 1 || !Descriptor.PayloadId.IsValid()
					|| Descriptor.LogicalByteCount != Descriptor.StoredByteCount)
					return Fail("Inspected authored bulk descriptor is invalid.", OutError);
				if (Descriptor.StorageKind == EEditorBulkDataStorageKind::External)
				{
					if (Reader.Tell() != Payload.size() || Descriptor.ContainerHash.IsZero())
						return Fail("External authored bulk descriptor has trailing bytes or no container hash.", OutError);
				}
				else
				{
					std::vector<std::byte> InlineBytes;
					Reader.SerializeByteBlob(InlineBytes);
					if (Reader.HasError() || Reader.Tell() != Payload.size()
						|| !Descriptor.ContainerHash.IsZero()
						|| Descriptor.StoredByteCount != InlineBytes.size()
						|| FXxHash128::HashBuffer(InlineBytes) != Descriptor.ContentHash)
						return Fail("Inline authored bulk payload integrity is invalid.", OutError);
				}
				Out.push_back(Descriptor);
				return true;
			}
			if (Kind != DurinCodeGen::EPropertyGenFlags::Struct) return true;
			std::string StructName;
			uint64 FieldCount = 0;
			Reader << StructName << FieldCount;
			if (Reader.HasError() || FieldCount > 100000)
				return Fail("Inspected authored struct payload header is invalid.", OutError);
			for (uint64 Index = 0; Index < FieldCount; ++Index)
			{
				std::string DeclaringType, Name, Signature;
				uint8 FieldKind = 0;
				uint64 PayloadSize = 0;
				Reader << DeclaringType << Name << FieldKind << Signature << PayloadSize;
				if (Reader.HasError() || PayloadSize > Reader.GetRemainingPayloadBytes())
					return Fail("Inspected authored struct field is truncated.", OutError);
				std::vector<std::byte> FieldPayload(static_cast<size_t>(PayloadSize));
				if (PayloadSize != 0)
					Reader.SerializeRawBytes(std::as_writable_bytes(std::span(FieldPayload)));
				if (Reader.HasError() || !CollectDescriptors(
						static_cast<DurinCodeGen::EPropertyGenFlags>(FieldKind),
						FieldPayload, Out, Depth + 1, OutError)) return false;
			}
			if (Reader.Tell() != Payload.size())
				return Fail("Inspected authored struct payload contains trailing bytes.", OutError);
			return true;
		}

		auto GetDurfRegistry() -> const FBinaryFormatRegistry&
		{
			static const FBinaryFormatRegistry Registry = [] {
				const std::array Descriptors{FBinaryFormatDescriptor{
					.FormatId = DabkBinaryFormatId,
					.DebugName = std::string(DabkBinaryFormatName),
					.MinimumFormatVersion = DurfVersion,
					.MaximumFormatVersion = DurfVersion,
					.SupportedRequiredFeatures = 0,
					.Limits = DurfLimits}};
				FBinaryFormatRegistry Result;
				require(FBinaryFormatRegistry::Create(Descriptors, Result));
				return Result;
			}();
			return Registry;
		}

		auto ReadDurfHeader(BulkContainer::FBoundedReader& Reader, FDurfHeader& OutHeader) -> bool
		{
			FDurfHeader Header;
			Reader.Read(Header.HeaderSize);
			Reader.Read(Header.EntrySize);
			Reader.Read(Header.Flags);
			Reader.Read(Header.Reserved0);
			Reader.Read(Header.EntryCount);
			Reader.Read(Header.DirectoryOffset);
			Reader.Read(Header.DataOffset);
			Reader.Read(Header.ContainerHash.HashLow);
			Reader.Read(Header.ContainerHash.HashHigh);
			Reader.Read(Header.Reserved1);
			if (!Reader.IsValid()) return false;
			OutHeader = Header;
			return true;
		}

		auto WriteDurfHeader(BulkContainer::FBoundedWriter& Writer, const FDurfHeader& Header) -> bool
		{
			Writer.Write(Header.HeaderSize);
			Writer.Write(Header.EntrySize);
			Writer.Write(Header.Flags);
			Writer.Write(Header.Reserved0);
			Writer.Write(Header.EntryCount);
			Writer.Write(Header.DirectoryOffset);
			Writer.Write(Header.DataOffset);
			Writer.Write(Header.ContainerHash.HashLow);
			Writer.Write(Header.ContainerHash.HashHigh);
			Writer.Write(Header.Reserved1);
			return Writer.IsValid();
		}

		auto ReadDurfEntry(BulkContainer::FBoundedReader& Reader, FXxHash128 ContainerHash,
			FEntry& OutEntry) -> bool
		{
			FEntry Entry;
			uint32 Flags = 0, Reserved = 0;
			Reader.ReadGuid(Entry.Descriptor.PayloadId);
			Reader.Read(Entry.Descriptor.LogicalByteCount);
			Reader.Read(Entry.Descriptor.StoredByteCount);
			Reader.Read(Entry.Descriptor.ContentHash.HashLow);
			Reader.Read(Entry.Descriptor.ContentHash.HashHigh);
			Reader.Read(Entry.Offset);
			Reader.Read(Flags);
			Reader.Read(Reserved);
			if (!Reader.IsValid() || Flags != 0 || Reserved != 0) return false;
			Entry.Descriptor.ContainerHash = ContainerHash;
			Entry.Descriptor.StorageKind = EEditorBulkDataStorageKind::External;
			OutEntry = Entry;
			return true;
		}

		auto WriteDurfEntry(BulkContainer::FBoundedWriter& Writer,
			const FEditorBulkDataStorageDescriptor& Descriptor, uint64 Offset) -> bool
		{
			Writer.WriteGuid(Descriptor.PayloadId);
			Writer.Write(Descriptor.LogicalByteCount);
			Writer.Write(Descriptor.StoredByteCount);
			Writer.Write(Descriptor.ContentHash.HashLow);
			Writer.Write(Descriptor.ContentHash.HashHigh);
			Writer.Write(Offset);
			Writer.Write(uint32{0});
			Writer.Write(uint32{0});
			return Writer.IsValid();
		}

		auto ParseV2(std::span<const std::byte> Bytes, FXxHash128 ExpectedContainerHash,
			std::vector<FEntry>& OutEntries, uint64& OutDataOffset,
			std::string* OutError) -> bool
		{
			OutEntries.clear();
			OutDataOffset = 0;
			if (Bytes.size() < DurfFixedHeaderBytes || Bytes.size() > MaximumBytes)
				return Fail("Authored bulk companion size is outside the supported bound.", OutError);
			FBinaryEnvelopePreamble Preamble;
			FBinaryEnvelopeDiagnostic Diagnostic;
			if (!ParseBinaryEnvelopePrefix(Bytes.first(BinaryEnvelopePreambleBytes), Bytes.size(),
					DurfLimits, Preamble, &Diagnostic)
				|| Preamble.HeaderBytes > Bytes.size())
				return Fail(std::string(Diagnostic.Message), OutError);
			FValidatedBinaryEnvelope Envelope;
			if (!ValidateBinaryEnvelopeHeader(Bytes.first(static_cast<size_t>(Preamble.HeaderBytes)),
					Bytes.size(), DurfLimits, GetDurfRegistry(), Envelope, &Diagnostic))
				return Fail(std::string(Diagnostic.Message), OutError);

			BulkContainer::FBoundedReader Reader(Envelope.FormatHeaderBytes, DurfLimits.MaximumHeaderBytes);
			FDurfHeader Header;
			if (!ReadDurfHeader(Reader, Header))
				return Fail("Authored bulk DURF header is truncated.", OutError);
			uint64 DirectoryBytes = 0, DirectoryEnd = 0, ExpectedDataOffset = 0;
			if (Header.HeaderSize != DurfFormatHeaderBytes || Header.EntrySize != DurfEntryBytes
				|| Header.Flags != 0 || Header.Reserved0 != 0 || Header.Reserved1 != 0
				|| Header.EntryCount == 0 || Header.EntryCount > MaximumEntries
				|| Header.DirectoryOffset != DurfFixedHeaderBytes || Header.ContainerHash.IsZero()
				|| (!ExpectedContainerHash.IsZero() && Header.ContainerHash != ExpectedContainerHash)
				|| !BulkContainer::TryMultiply(Header.EntryCount, DurfEntryBytes, MaximumBytes, DirectoryBytes)
				|| !BulkContainer::TryAdd(DurfFixedHeaderBytes, DirectoryBytes, MaximumBytes, DirectoryEnd)
				|| !BulkContainer::TryAlignUp(DirectoryEnd, Alignment, MaximumBytes, ExpectedDataOffset)
				|| Header.DataOffset != ExpectedDataOffset || Header.DataOffset != Preamble.HeaderBytes)
				return Fail("Authored bulk DURF header is invalid.", OutError);

			OutEntries.reserve(static_cast<size_t>(Header.EntryCount));
			std::vector<BulkContainer::FPayloadRange> Ranges;
			Ranges.reserve(static_cast<size_t>(Header.EntryCount));
			for (uint64 Index = 0; Index < Header.EntryCount; ++Index)
			{
				FEntry Entry;
				if (!ReadDurfEntry(Reader, Header.ContainerHash, Entry)
					|| !Entry.Descriptor.PayloadId.IsValid()
					|| Entry.Descriptor.LogicalByteCount != Entry.Descriptor.StoredByteCount
					|| Entry.Descriptor.StoredByteCount > MaximumBytes
					|| Entry.Offset < Header.DataOffset || Entry.Offset % Alignment != 0
					|| (!OutEntries.empty() && !(OutEntries.back().Descriptor.PayloadId < Entry.Descriptor.PayloadId)))
					return Fail("Authored bulk DURF directory entry is invalid.", OutError);
				Ranges.push_back({Entry.Offset, Entry.Descriptor.StoredByteCount, Alignment});
				OutEntries.push_back(Entry);
			}
			const BulkContainer::FLayoutPolicy LayoutPolicy{
				.MaximumCount = MaximumEntries, .MaximumPayloadBytes = MaximumBytes,
				.MaximumContainerBytes = MaximumBytes, .RequireCanonicalOffsets = true,
				.AllowTrailingZeroPadding = false};
			BulkContainer::FFailure LayoutFailure;
			if (!BulkContainer::ValidateLayout(Bytes, DirectoryEnd, Header.DataOffset,
					Ranges, LayoutPolicy, &LayoutFailure))
				return Fail("Authored bulk DURF layout is invalid.", OutError);
			for (const FEntry& Entry : OutEntries)
			{
				std::span<const std::byte> Payload;
				if (!BulkContainer::TryProjectRange(Bytes, Entry.Offset,
						Entry.Descriptor.StoredByteCount, Payload)
					|| FXxHash128::HashBuffer(Payload) != Entry.Descriptor.ContentHash)
					return Fail("Authored bulk payload content hash verification failed.", OutError);
			}
			OutDataOffset = Header.DataOffset;
			if (OutError) OutError->clear();
			return true;
		}

		auto Parse(std::span<const std::byte> Bytes, FXxHash128 ExpectedContainerHash,
			std::vector<FEntry>& OutEntries, uint64& OutDataOffset,
			std::string* OutError) -> bool
		{
			return ParseV2(Bytes, ExpectedContainerHash, OutEntries, OutDataOffset, OutError);
		}
	}

	auto ResolveEditorBulkDataCompanionPath(
		const std::filesystem::path& PackagePath,
		std::filesystem::path& OutPath, std::string* OutError) -> bool
	{
		OutPath.clear();
		if (PackagePath.extension() != ".dasset")
			return Fail("Authored bulk companion requires a .dasset path.", OutError);
		OutPath = PackagePath.parent_path()
			/ std::format("{}{}", PackagePath.stem().string(),
				EditorBulkDataCompanionSuffix);
		if (OutError) OutError->clear();
		return true;
	}

	auto BuildEditorBulkDataCompanion(
		std::span<const FEditorBulkDataStoragePayload> Payloads, FXxHash128 ContainerHash,
		std::vector<std::byte>& OutBytes, std::string* OutError) -> bool
	{
		OutBytes.clear();
		if (Payloads.empty() || Payloads.size() > MaximumEntries || ContainerHash.IsZero())
			return Fail("Authored bulk companion requires a bounded nonempty payload set and container hash.", OutError);
		std::vector<const FEditorBulkDataStoragePayload*> Sorted;
		if (!BulkContainer::TryMakeSortedProjection<FEditorBulkDataStoragePayload>(
			Payloads, [](const FEditorBulkDataStoragePayload& Payload) {
				return Payload.Descriptor.PayloadId;
			}, Sorted))
			return Fail("Authored bulk companion contains duplicate payload ids.", OutError);

		uint64 DirectoryBytes = 0, DirectoryEnd = 0, DataOffset = 0;
		if (!BulkContainer::TryMultiply(Sorted.size(), DurfEntryBytes, MaximumBytes, DirectoryBytes)
			|| !BulkContainer::TryAdd(DurfFixedHeaderBytes, DirectoryBytes, MaximumBytes, DirectoryEnd)
			|| !BulkContainer::TryAlignUp(DirectoryEnd, Alignment, MaximumBytes, DataOffset))
			return Fail("Authored bulk companion exceeds the 1 GiB bound.", OutError);
		std::vector<BulkContainer::FLayoutItem> LayoutItems;
		LayoutItems.reserve(Sorted.size());
		for (const FEditorBulkDataStoragePayload* Payload : Sorted)
		{
			const auto& Descriptor = Payload->Descriptor;
			if (!Descriptor.PayloadId.IsValid()
				|| Descriptor.StorageKind != EEditorBulkDataStorageKind::External
				|| Descriptor.ContainerHash != ContainerHash
				|| Descriptor.LogicalByteCount != Payload->Buffer.GetSize()
				|| Descriptor.StoredByteCount != Payload->Buffer.GetSize()
				|| FXxHash128::HashBuffer(Payload->Buffer.GetBytes()) != Descriptor.ContentHash)
				return Fail("Authored bulk companion input descriptor or bytes are invalid.", OutError);
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
			return Fail("Authored bulk companion exceeds the 1 GiB bound.", OutError);

		BulkContainer::FBoundedWriter Writer(MaximumBytes);
		const std::array<std::byte, BinaryEnvelopePreambleBytes> EmptyPreamble{};
		const FDurfHeader Header{
			.HeaderSize = DurfFormatHeaderBytes,
			.EntrySize = DurfEntryBytes,
			.Flags = 0,
			.Reserved0 = 0,
			.EntryCount = Sorted.size(),
			.DirectoryOffset = DurfFixedHeaderBytes,
			.DataOffset = DataOffset,
			.ContainerHash = ContainerHash,
			.Reserved1 = 0};
		if (!Writer.Write(EmptyPreamble) || !WriteDurfHeader(Writer, Header))
			return Fail("Authored bulk companion encoding failed.", OutError);
		for (size_t Index = 0; Index < Sorted.size(); ++Index)
		{
			const auto& Descriptor = Sorted[Index]->Descriptor;
			if (!WriteDurfEntry(Writer, Descriptor, Ranges[Index].Offset))
				return Fail("Authored bulk companion encoding failed.", OutError);
		}
		if (!Writer.PadTo(DataOffset))
			return Fail("Authored bulk companion encoding failed.", OutError);
		for (size_t Index = 0; Index < Sorted.size(); ++Index)
		{
			if (!Writer.PadTo(Ranges[Index].Offset))
				return Fail("Authored bulk companion encoding failed.", OutError);
			const auto Bytes = Sorted[Index]->Buffer.GetBytes();
			if (!Writer.Write(Bytes))
				return Fail("Authored bulk companion encoding failed.", OutError);
		}
		std::vector<std::byte> Candidate;
		if (Writer.Tell() != FileSize || !Writer.TryTake(Candidate))
			return Fail("Authored bulk companion encoding failed.", OutError);
		const FBinaryEnvelopePreamble Preamble{
			.FormatId = DabkBinaryFormatId, .FormatVersion = DurfVersion,
			.RequiredFeatures = 0, .HeaderBytes = DataOffset, .FileBytes = FileSize};
		FBinaryEnvelopeDiagnostic Diagnostic;
		if (!EncodeBinaryEnvelopePreamble(Preamble, Candidate, &Diagnostic)
			|| !FinalizeBinaryEnvelopeHeader(std::span(Candidate).first(static_cast<size_t>(DataOffset)),
				FileSize, DurfLimits, &Diagnostic))
			return Fail(std::string(Diagnostic.Message), OutError);
		if (!ValidateEditorBulkDataCompanion(Candidate, ContainerHash, OutError)) return false;
		OutBytes = std::move(Candidate);
		return true;
	}

	auto ValidateEditorBulkDataCompanion(
		std::span<const std::byte> Bytes, FXxHash128 ExpectedContainerHash,
		std::string* OutError) -> bool
	{
		std::vector<FEntry> Entries;
		uint64 DataOffset = 0;
		return Parse(Bytes, ExpectedContainerHash, Entries, DataOffset, OutError);
	}

	auto ReadEditorBulkDataStoragePayload(
		const std::filesystem::path& CompanionPath,
		const FEditorBulkDataStorageDescriptor& Descriptor,
		FSharedByteBuffer& OutBuffer, std::string* OutError) -> bool
	{
		OutBuffer = {};
		std::vector<std::byte> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, CompanionPath))
			return Fail("Authored bulk companion is missing or unreadable.", OutError);
		std::vector<FEntry> Entries;
		uint64 DataOffset = 0;
		if (!Parse(Bytes, Descriptor.ContainerHash, Entries, DataOffset, OutError)) return false;
		const auto It = std::ranges::find(Entries, Descriptor.PayloadId,
			[](const FEntry& Entry) { return Entry.Descriptor.PayloadId; });
		if (It == Entries.end() || It->Descriptor != Descriptor)
			return Fail("Authored bulk companion descriptor does not match the package reference.", OutError);
		OutBuffer = FSharedByteBuffer::Copy(std::as_bytes(std::span(Bytes)).subspan(
			static_cast<size_t>(It->Offset), static_cast<size_t>(It->Descriptor.StoredByteCount)));
		if (OutError) OutError->clear();
		return true;
	}

	auto LoadEditorBulkDataStoragePayload(
		const std::filesystem::path& CompanionPath,
		const FEditorBulkDataStorageDescriptor& Descriptor,
		FSharedByteBuffer& OutBuffer, std::string* OutError) -> bool
	{
		std::string FinalError;
		if (ReadEditorBulkDataStoragePayload(
				CompanionPath, Descriptor, OutBuffer, &FinalError))
		{
			std::filesystem::path BackupPath = CompanionPath;
			BackupPath += EditorBulkDataCompanionBackupSuffix;
			std::error_code ErrorCode;
			std::filesystem::remove(BackupPath, ErrorCode);
			if (ErrorCode)
			{
				OutBuffer = {};
				return Fail(std::format(
					"Authored bulk backup cleanup failed: {}", ErrorCode.message()), OutError);
			}
			if (OutError) OutError->clear();
			return true;
		}

		std::filesystem::path BackupPath = CompanionPath;
		BackupPath += EditorBulkDataCompanionBackupSuffix;
		FSharedByteBuffer BackupBuffer;
		std::string BackupError;
		if (!ReadEditorBulkDataStoragePayload(
				BackupPath, Descriptor, BackupBuffer, &BackupError))
		{
			OutBuffer = {};
			return Fail(std::format(
				"Authored bulk companion and backup do not match the published package. Final: {} Backup: {}",
				FinalError, BackupError), OutError);
		}

		std::vector<std::byte> BackupBytes;
		if (!FFileHelper::LoadFileToArray(BackupBytes, BackupPath))
		{
			OutBuffer = {};
			return Fail("Authored bulk backup became unreadable during recovery.", OutError);
		}
		FFileHelper::FAtomicFileError PublicationError;
		if (!FFileHelper::SaveArrayToFileAtomically(
				BackupBytes, CompanionPath, &PublicationError))
		{
			OutBuffer = {};
			return Fail(std::format("Authored bulk backup recovery failed: {}",
				PublicationError.ToString()), OutError);
		}
		std::error_code ErrorCode;
		if (!std::filesystem::remove(BackupPath, ErrorCode) || ErrorCode)
		{
			OutBuffer = {};
			return Fail(std::format(
				"Authored bulk backup cleanup failed after recovery: {}", ErrorCode.message()), OutError);
		}
		OutBuffer = std::move(BackupBuffer);
		if (OutError) OutError->clear();
		return true;
	}

	auto InspectEditorBulkDataCompanionPaths(
		const std::filesystem::path& PackagePath,
		const FAssetPackageInspection& Inspection,
		std::vector<std::filesystem::path>& OutPaths,
		std::string* OutError) -> bool
	{
		OutPaths.clear();
		std::vector<FEditorBulkDataStorageDescriptor> Descriptors;
		if (!InspectEditorBulkDataStorageDescriptors(
				Inspection, Descriptors, OutError)) return false;
		for (const FEditorBulkDataStorageDescriptor& Descriptor : Descriptors)
		{
			if (Descriptor.StorageKind != EEditorBulkDataStorageKind::External) continue;
			std::filesystem::path Path;
			if (!ResolveEditorBulkDataCompanionPath(PackagePath, Path, OutError)) return false;
			if (OutPaths.empty() || OutPaths.back() != Path) OutPaths.push_back(std::move(Path));
		}
		if (OutError) OutError->clear();
		return true;
	}

	auto InspectEditorBulkDataStorageDescriptors(
		const FAssetPackageInspection& Inspection,
		std::vector<FEditorBulkDataStorageDescriptor>& OutDescriptors,
		std::string* OutError) -> bool
	{
		OutDescriptors.clear();
		for (const FAssetPackageObjectInspection& Object : Inspection.Objects)
			for (const FAssetPackageField& Field : Object.Fields)
				if (!CollectDescriptors(
						Field.Kind, Field.Payload, OutDescriptors, 0, OutError))
					return false;
		if (OutError) OutError->clear();
		return true;
	}

	auto InspectOrphanedEditorBulkDataCompanionPaths(
		const std::filesystem::path& PackagePath,
		const FAssetPackageInspection& Inspection,
		std::vector<std::filesystem::path>& OutPaths,
		std::string* OutError) -> bool
	{
		OutPaths.clear();
		std::vector<std::filesystem::path> Referenced;
		if (!InspectEditorBulkDataCompanionPaths(
				PackagePath, Inspection, Referenced, OutError)) return false;
		const std::filesystem::path Parent = PackagePath.parent_path();
		const std::string Stem = PackagePath.stem().string();
		const std::string StableName = Stem + std::string(EditorBulkDataCompanionSuffix);
		std::error_code ErrorCode;
		for (std::filesystem::directory_iterator It(Parent, ErrorCode), End;
			!ErrorCode && It != End; It.increment(ErrorCode))
		{
			const std::filesystem::path Candidate = It->path();
			const std::string Name = Candidate.filename().string();
			if (!It->is_regular_file(ErrorCode) || ErrorCode
				|| Name != StableName)
			{
				ErrorCode.clear();
				continue;
			}
			if (std::ranges::find(Referenced, Candidate) == Referenced.end())
				OutPaths.push_back(Candidate);
		}
		if (ErrorCode)
			return Fail("Editor bulk companion directory could not be inspected.", OutError);
		std::ranges::sort(OutPaths);
		if (OutError) OutError->clear();
		return true;
	}
}
