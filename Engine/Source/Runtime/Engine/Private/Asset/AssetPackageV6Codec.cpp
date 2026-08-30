#include "AssetPackageV6Codec.h"

#include "Asset/Compatibility.h"
#include "Asset/EditorBulkDataStorage.h"
#include "Asset/PackageResource.h"
#include "Asset/PackageObjectStreamReader.h"
#include "Asset/PackageObjectStreamWriter.h"
#include "AssetRegistry/PackageHeader.h"
#include "Serialization/BinaryEnvelope.h"
#include "Serialization/BinaryFormat.h"
#include "PackageBulkDataWire.h"
#include "Misc/Paths.h"

namespace Durin::Asset::Private::DastV6
{
	namespace
	{
		constexpr uint32 PublicSummaryVersion = 1;
		constexpr uint32 ImportVersion = 1;
		constexpr uint32 PayloadDirectoryVersion = 1;
		constexpr uint32 PayloadEntryBytes = 80;
		constexpr FBinaryEnvelopeLimits EnvelopeLimits{MaximumHeaderBytes, MaximumFileBytes};

		auto Error(std::string Message) -> FAssetResult
		{
			return {EAssetError::CorruptFile, std::move(Message)};
		}

		auto Fail(std::string Message, std::string* OutError) -> bool
		{
			if (OutError) *OutError = std::move(Message);
			return false;
		}

		class FWriter
		{
		public:
			template<typename T>
			auto Fixed(T Value) -> void
			{
				for (size_t Index = 0; Index < sizeof(T); ++Index)
					Bytes.push_back(static_cast<std::byte>((Value >> (Index * 8)) & 0xff));
			}

			auto VarUInt(uint64 Value) -> void
			{
				do
				{
					uint8 Byte = Value & 0x7f;
					Value >>= 7;
					if (Value != 0) Byte |= 0x80;
					Bytes.push_back(static_cast<std::byte>(Byte));
				} while (Value != 0);
			}

			auto String(std::string_view Value) -> bool
			{
				if (Value.size() > PackageObjectStream::MaximumStringBytes) return false;
				VarUInt(Value.size());
				Append(std::as_bytes(std::span(Value.data(), Value.size())));
				return true;
			}

			auto Guid(const FGuid& Value) -> void
			{
				Fixed(Value.A); Fixed(Value.B); Fixed(Value.C); Fixed(Value.D);
			}

			auto Hash(const FXxHash128& Value) -> void
			{
				Fixed(Value.HashLow); Fixed(Value.HashHigh);
			}

			auto Append(std::span<const std::byte> Value) -> void
			{
				Bytes.insert(Bytes.end(), Value.begin(), Value.end());
			}

			auto View() const -> std::span<const std::byte> { return Bytes; }
			auto Take() -> std::vector<std::byte> { return std::move(Bytes); }

		private:
			std::vector<std::byte> Bytes;
		};

		class FReader
		{
		public:
			explicit FReader(std::span<const std::byte> InBytes) : Bytes(InBytes) {}

			template<typename T>
			auto Fixed(T& OutValue) -> bool
			{
				if (Remaining() < sizeof(T)) return false;
				T Value = 0;
				for (size_t Index = 0; Index < sizeof(T); ++Index)
					Value |= static_cast<T>(std::to_integer<uint8>(Bytes[Offset++])) << (Index * 8);
				OutValue = Value;
				return true;
			}

			auto VarUInt(uint64& OutValue) -> bool
			{
				uint64 Value = 0;
				for (uint32 Index = 0; Index < 10; ++Index)
				{
					uint8 Byte = 0;
					if (!Fixed(Byte) || (Index == 9 && (Byte & 0xfe) != 0)) return false;
					Value |= uint64(Byte & 0x7f) << (Index * 7);
					if ((Byte & 0x80) == 0)
					{
						if (Index != 0 && Byte == 0) return false;
						OutValue = Value;
						return true;
					}
				}
				return false;
			}

			auto String(std::string& OutValue, bool bAllowEmpty = true) -> bool
			{
				uint64 Size = 0;
				if (!VarUInt(Size) || Size > PackageObjectStream::MaximumStringBytes
					|| Size > Remaining() || (!bAllowEmpty && Size == 0)) return false;
				std::string Value(reinterpret_cast<const char*>(Bytes.data() + Offset),
					static_cast<size_t>(Size));
				Offset += Size;
				if (Value.find('\0') != std::string::npos) return false;
				OutValue = std::move(Value);
				return true;
			}

			auto Guid(FGuid& OutValue) -> bool
			{
				FGuid Value;
				if (!Fixed(Value.A) || !Fixed(Value.B) || !Fixed(Value.C) || !Fixed(Value.D))
					return false;
				OutValue = Value;
				return true;
			}

			auto Hash(FXxHash128& OutValue) -> bool
			{
				FXxHash128 Value;
				if (!Fixed(Value.HashLow) || !Fixed(Value.HashHigh)) return false;
				OutValue = Value;
				return true;
			}

			auto Remaining() const -> size_t { return Bytes.size() - Offset; }
			auto AtEnd() const -> bool { return Offset == Bytes.size(); }

	private:
		std::span<const std::byte> Bytes;
		size_t Offset = 0;
	};

		template<typename T>
		auto ReadAt(std::span<const std::byte> Bytes, uint64 Offset, T& OutValue) -> bool
		{
			return ReadLittleEndianAt(Bytes, Offset, OutValue);
		}

		template<typename T>
		auto WriteAt(std::span<std::byte> Bytes, uint64 Offset, T Value) -> void
		{
			for (size_t Index = 0; Index < sizeof(T); ++Index)
				Bytes[static_cast<size_t>(Offset) + Index]
					= static_cast<std::byte>((Value >> (Index * 8)) & 0xff);
		}

		auto GetRegistry() -> const FBinaryFormatRegistry&
		{
			static const FBinaryFormatRegistry Registry = [] {
				const std::array Descriptors{FBinaryFormatDescriptor{
					.FormatId = DastBinaryFormatId,
					.DebugName = std::string(DastBinaryFormatName),
					.MinimumFormatVersion = Version,
					.MaximumFormatVersion = AssetPackageV7FormatVersion,
					.SupportedRequiredFeatures = 0,
					.Limits = EnvelopeLimits}};
				FBinaryFormatRegistry Result;
				const bool bCreated = FBinaryFormatRegistry::Create(Descriptors, Result);
				require(bCreated);
				return Result;
			}();
			return Registry;
		}

		auto EncodePublicSummary(
			const PackageObjectStream::FValidatedHeader& Header,
			uint64 PayloadCount,
			std::vector<std::byte>& OutBytes) -> bool
		{
			if (Header.ObjectCount == 0 || Header.ObjectCount > MaximumExportCount
				|| Header.Dependencies.size() > MaximumImportCount
				|| PayloadCount > MaximumPayloadCount) return false;
			FWriter Writer;
			Writer.Fixed(PublicSummaryVersion);
			Writer.Fixed(uint32{1});
			Writer.Fixed(static_cast<uint64>(Header.Dependencies.size()));
			Writer.Fixed(Header.ObjectCount);
			Writer.Fixed(PayloadCount);
			Writer.Fixed(uint64{0});
			if (!Writer.String(Header.AssetClass) || !Writer.String(Header.RedirectDestination))
				return false;
			OutBytes = Writer.Take();
			return true;
		}

		auto EncodePublicSummaryV7(
			const PackageObjectStream::FValidatedHeader& Header,
			uint64 PayloadCount,
			const FPackageBulkSegmentSummary& Segment,
			std::vector<std::byte>& OutBytes) -> bool
		{
			if (Header.ObjectCount == 0 || Header.ObjectCount > MaximumExportCount
				|| Header.Dependencies.size() > MaximumImportCount
				|| PayloadCount > MaximumPayloadCount
				|| Segment.Extent > PackageBulkDataMaximumSegmentBytes
				|| ((Segment.Extent == 0) != Segment.Digest.IsZero())) return false;
			FWriter Writer;
			Writer.Fixed(uint32{2});
			Writer.Fixed(uint32{1});
			Writer.Fixed(static_cast<uint64>(Header.Dependencies.size()));
			Writer.Fixed(Header.ObjectCount);
			Writer.Fixed(PayloadCount);
			Writer.Fixed(Segment.Extent);
			Writer.Hash(Segment.Digest);
			Writer.Fixed(uint32{0});
			Writer.Fixed(uint32{0});
			if (!Writer.String(Header.AssetClass) || !Writer.String(Header.RedirectDestination))
				return false;
			OutBytes = Writer.Take();
			return true;
		}

		auto EncodeImports(
			std::span<const std::string> Imports,
			std::vector<std::byte>& OutBytes) -> bool
		{
			if (Imports.size() > MaximumImportCount) return false;
			FWriter Writer;
			Writer.Fixed(ImportVersion);
			Writer.Fixed(uint32{0});
			Writer.Fixed(static_cast<uint64>(Imports.size()));
			for (const std::string& Import : Imports)
				if (!Writer.String(Import)) return false;
			OutBytes = Writer.Take();
			return true;
		}

		auto EncodePayloadDirectory(
			std::span<const FPayloadEntry> Entries,
			std::vector<std::byte>& OutBytes) -> bool
		{
			if (Entries.size() > MaximumPayloadCount) return false;
			FWriter Writer;
			Writer.Fixed(PayloadDirectoryVersion);
			Writer.Fixed(PayloadEntryBytes);
			Writer.Fixed(static_cast<uint64>(Entries.size()));
			for (const FPayloadEntry& Entry : Entries)
			{
				Writer.Guid(Entry.PayloadId);
				Writer.Fixed(static_cast<uint32>(Entry.Placement));
				Writer.Fixed(uint32{0});
				Writer.Fixed(Entry.LogicalByteCount);
				Writer.Fixed(Entry.StoredByteCount);
				Writer.Hash(Entry.ContentHash);
				Writer.Hash(Entry.ContainerHash);
				Writer.Fixed(uint64{0});
			}
			OutBytes = Writer.Take();
			return true;
		}

		auto DecodePayloadDirectory(std::span<const std::byte> Bytes,
			FParsedPackage& Out, std::string* OutError) -> bool
		{
			FReader Reader(Bytes);
			uint32 VersionValue = 0;
			uint32 EntryBytes = 0;
			uint64 Count = 0;
			if (!Reader.Fixed(VersionValue) || VersionValue != PayloadDirectoryVersion
				|| !Reader.Fixed(EntryBytes) || EntryBytes != PayloadEntryBytes
				|| !Reader.Fixed(Count) || Count != Out.ExpectedPayloadCount)
				return Fail("DAST v6 Payload Directory header is malformed.", OutError);
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				FPayloadEntry Entry;
				uint32 Placement = 0;
				uint32 Flags = 0;
				uint64 Reserved = 0;
				if (!Reader.Guid(Entry.PayloadId) || !Reader.Fixed(Placement)
					|| !Reader.Fixed(Flags) || Flags != 0
					|| !Reader.Fixed(Entry.LogicalByteCount)
					|| !Reader.Fixed(Entry.StoredByteCount)
					|| !Reader.Hash(Entry.ContentHash) || !Reader.Hash(Entry.ContainerHash)
					|| !Reader.Fixed(Reserved) || Reserved != 0)
					return Fail("DAST v6 Payload Directory entry is malformed.", OutError);
				Entry.Placement = static_cast<EPayloadPlacement>(Placement);
				if (!Entry.PayloadId.IsValid()
					|| Entry.Placement != EPayloadPlacement::ExternalDabkV1
					|| Entry.LogicalByteCount != Entry.StoredByteCount
					|| Entry.ContentHash.IsZero() || Entry.ContainerHash.IsZero()
					|| (!Out.PayloadEntries.empty()
						&& !(Out.PayloadEntries.back().PayloadId < Entry.PayloadId)))
					return Fail("DAST v6 Payload Directory entry is invalid or noncanonical.", OutError);
				Out.PayloadEntries.push_back(Entry);
			}
			return Reader.AtEnd() || Fail("DAST v6 Payload Directory has trailing bytes.", OutError);
		}

		auto EncodeLogicalObjectStream(
			const FParsedPackage& Package,
			std::vector<std::byte>& OutBytes) -> bool
		{
			FWriter Summary;
			if (!Summary.String(Package.AssetClass)) return false;
			Summary.Fixed(static_cast<uint8>(Package.EntryKind));
			if (!Summary.String(Package.RedirectDestination)) return false;
			Summary.VarUInt(Package.Imports.size());
			for (const std::string& Import : Package.Imports)
				if (!Summary.String(Import)) return false;
			Summary.VarUInt(Package.ExportCount);
			if (Summary.View().size() > PackageObjectStream::MaximumSummaryBytes) return false;

			std::array<std::span<const std::byte>, 5> Sections{
				Package.RequiredSections[2], Package.RequiredSections[3],
				Package.RequiredSections[4], Package.RequiredSections[5],
				Package.RequiredSections[6]};
			uint64 Total = 13 + Summary.View().size() + Sections.size() * 9;
			for (auto Section : Sections) Total += Section.size();
			if (Total > PackageObjectStream::MaximumPackageBytes
				|| Total > std::numeric_limits<uint32>::max()) return false;

			FWriter Writer;
			Writer.Fixed(DastPackageMagic);
			Writer.Fixed(AssetPackageObjectStreamVersion);
			Writer.Fixed(static_cast<uint32>(Summary.View().size()));
			Writer.Fixed(static_cast<uint8>(Sections.size()));
			Writer.Append(Summary.View());
			uint32 Offset = static_cast<uint32>(13 + Summary.View().size() + Sections.size() * 9);
			for (size_t Index = 0; Index < Sections.size(); ++Index)
			{
				Writer.Fixed(static_cast<uint8>(Index + 1));
				Writer.Fixed(Offset);
				Writer.Fixed(static_cast<uint32>(Sections[Index].size()));
				Offset += static_cast<uint32>(Sections[Index].size());
			}
			for (auto Section : Sections) Writer.Append(Section);
			OutBytes = Writer.Take();
			return true;
		}

		auto MakePayloadEntries(const FAssetPackageInspection& Inspection,
			std::vector<FPayloadEntry>& OutEntries) -> FAssetResult
		{
			OutEntries.clear();
			std::vector<FEditorBulkDataStorageDescriptor> Descriptors;
			std::string DescriptorError;
			if (!InspectEditorBulkDataStorageDescriptors(
					Inspection, Descriptors, &DescriptorError))
				return Error(std::move(DescriptorError));
			for (const FEditorBulkDataStorageDescriptor& Descriptor : Descriptors)
			{
				if (Descriptor.StorageKind != EEditorBulkDataStorageKind::External) continue;
				OutEntries.push_back({
					.PayloadId = Descriptor.PayloadId,
					.Placement = EPayloadPlacement::ExternalDabkV1,
					.LogicalByteCount = Descriptor.LogicalByteCount,
					.StoredByteCount = Descriptor.StoredByteCount,
					.ContentHash = Descriptor.ContentHash,
					.ContainerHash = Descriptor.ContainerHash});
			}
			std::ranges::sort(OutEntries, {}, &FPayloadEntry::PayloadId);
			for (size_t Index = 1; Index < OutEntries.size(); ++Index)
				if (OutEntries[Index - 1].PayloadId == OutEntries[Index].PayloadId)
					return Error("DAST v6 object stream contains duplicate external payload ids.");
			return {};
		}

			auto PrepareObjectStream(const FParsedPackage& Package,
			std::vector<std::byte>& OutBytes,
			FAssetPackageInspection* OutInspection = nullptr) -> FAssetResult
		{
			if (!EncodeLogicalObjectStream(Package, OutBytes))
				return Error("DAST logical sections cannot form a bounded object stream.");
			FAssetPackageInspection Inspection;
			PackageObjectStream::FReaderDiagnostic Diagnostic;
			if (FAssetResult Result = PackageObjectStream::InspectPackage(
					OutBytes, Inspection, {}, &Diagnostic); !Result)
				return Error(std::format("{} (DAST v6 imports: {})", Result.Message,
					std::accumulate(Package.Imports.begin(), Package.Imports.end(),
						std::string{}, [](std::string Value, const std::string& Import) {
							if (!Value.empty()) Value += ", ";
							Value += Import;
							return Value;
						})));
			for (FAssetPackageObjectInspection& Object : Inspection.Objects)
				for (FAssetPackageField& Field : Object.Fields)
					Field.SourceFormatVersion = Package.FormatVersion;
			if (Package.FormatVersion == AssetPackageV6FormatVersion)
			{
				std::vector<FPayloadEntry> Expected;
				if (FAssetResult Result = MakePayloadEntries(Inspection, Expected); !Result)
					return Result;
				if (Expected != Package.PayloadEntries)
					return Error(
						"DAST v6 Payload Directory disagrees with object-stream bulk descriptors.");
			}
			else
			{
				std::vector<FEditorBulkDataStorageDescriptor> Descriptors;
				std::string DescriptorError;
				if (!InspectEditorBulkDataStorageDescriptors(
					Inspection, Descriptors, &DescriptorError))
					return Error(std::move(DescriptorError));
				if (Descriptors.size() != Package.BulkEntries.size())
					return Error("DAST v7 Payload Directory field count disagrees with the object stream.");
				for (size_t Index = 0; Index < Descriptors.size(); ++Index)
				{
					const auto& Descriptor = Descriptors[Index];
					const auto& Entry = Package.BulkEntries[Index];
					if (Entry.FieldIndex != Index + 1
						|| Entry.Placement != (Descriptor.StorageKind == EEditorBulkDataStorageKind::Inline
							? EPackageBulkDataPlacement::Inline : EPackageBulkDataPlacement::External)
						|| Entry.LogicalSize != Descriptor.LogicalByteCount
						|| Entry.StoredSize != Descriptor.StoredByteCount
						|| Entry.SegmentOffset != Descriptor.SegmentOffset
						|| Entry.Alignment != Descriptor.Alignment
						|| Entry.ContentId != Descriptor.ContentHash)
						return Error("DAST v7 Payload Directory disagrees with object-stream bulk metadata.");
				}
			}
			if (OutInspection) *OutInspection = std::move(Inspection);
			return {};
		}

		auto BuildV6Package(
			const PackageObjectStream::FValidatedHeader& Header,
			const std::array<std::span<const std::byte>, 5>& V5Sections,
			std::span<const FPayloadEntry> PayloadEntries,
			std::vector<std::byte>& OutBytes) -> FAssetResult
		{
			std::array<std::vector<std::byte>, RequiredSectionCount> Owned;
			if (!EncodePublicSummary(Header, PayloadEntries.size(), Owned[0])
				|| !EncodeImports(Header.Dependencies, Owned[1])
				|| !EncodePayloadDirectory(PayloadEntries, Owned[7]))
				return Error("DAST v6 front matter or Payload Directory exceeds its bound.");
			for (size_t Index = 0; Index < V5Sections.size(); ++Index)
				Owned[Index + 2].assign(V5Sections[Index].begin(), V5Sections[Index].end());

			const uint64 DirectoryOffset = BinaryEnvelopePreambleBytes + FormatHeaderBytes;
			uint64 Offset = DirectoryOffset + RequiredSectionCount * SectionEntryBytes;
			std::array<FSectionEntry, RequiredSectionCount> Entries;
			for (size_t Index = 0; Index < Owned.size(); ++Index)
			{
				Entries[Index] = {
					.Kind = static_cast<uint32>(Index + 1),
					.Flags = RequiredSectionFlag,
					.Offset = Offset,
					.Size = Owned[Index].size(),
					.Hash = FXxHash128::HashBuffer(Owned[Index])};
				if (Owned[Index].size() > MaximumFileBytes - Offset)
					return Error("DAST v6 section extents exceed the file bound.");
				Offset += Owned[Index].size();
			}
			const uint64 HeaderBytes = Entries[1].Offset + Entries[1].Size;
			if (HeaderBytes > MaximumHeaderBytes || Offset > MaximumFileBytes)
				return Error("DAST v6 declared extents exceed configured limits.");

			std::vector<std::byte> Bytes(static_cast<size_t>(Offset));
			const FBinaryEnvelopePreamble Preamble{
				.FormatId = DastBinaryFormatId,
				.FormatVersion = Version,
				.RequiredFeatures = 0,
				.HeaderBytes = HeaderBytes,
				.FileBytes = Offset};
			if (!EncodeBinaryEnvelopePreamble(Preamble, Bytes))
				return Error("DAST v6 common preamble encoding failed.");
			WriteAt(Bytes, 64, static_cast<uint32>(Header.EntryKind));
			WriteAt(Bytes, 68, uint32{0});
			WriteAt(Bytes, 72, DirectoryOffset);
			WriteAt(Bytes, 80, RequiredSectionCount);
			WriteAt(Bytes, 84, SectionEntryBytes);
			WriteAt(Bytes, 88, uint64{0});
			for (size_t Index = 0; Index < Entries.size(); ++Index)
			{
				const uint64 EntryOffset = DirectoryOffset + Index * SectionEntryBytes;
				WriteAt(Bytes, EntryOffset, Entries[Index].Kind);
				WriteAt(Bytes, EntryOffset + 4, Entries[Index].Flags);
				WriteAt(Bytes, EntryOffset + 8, Entries[Index].Offset);
				WriteAt(Bytes, EntryOffset + 16, Entries[Index].Size);
				WriteAt(Bytes, EntryOffset + 24, Entries[Index].Hash.HashLow);
				WriteAt(Bytes, EntryOffset + 32, Entries[Index].Hash.HashHigh);
				WriteAt(Bytes, EntryOffset + 40, uint64{0});
				std::ranges::copy(Owned[Index], Bytes.begin() + Entries[Index].Offset);
			}
			if (!FinalizeBinaryEnvelopeHeader(std::span(Bytes).first(static_cast<size_t>(HeaderBytes)),
				Offset, EnvelopeLimits))
				return Error("DAST v6 common header finalization failed.");
			OutBytes = std::move(Bytes);
			return {};
		}

		auto BuildV7Package(
			const PackageObjectStream::FValidatedHeader& Header,
			const std::array<std::span<const std::byte>, 5>& Sections,
			std::span<const FPackageBulkDataEntry> BulkEntries,
			const FPackageBulkSegmentSummary& BulkSegment,
			std::vector<std::byte>& OutBytes) -> FAssetResult
		{
			std::array<std::vector<std::byte>, RequiredSectionCount> Owned;
			std::string DirectoryError;
			if (!EncodePublicSummaryV7(Header, BulkEntries.size(), BulkSegment, Owned[0])
				|| !EncodeImports(Header.Dependencies, Owned[1])
				|| !EncodePackageBulkDataDirectory(BulkEntries, Owned[7], &DirectoryError))
				return Error(DirectoryError.empty()
					? "DAST v7 front matter or Payload Directory exceeds its bound."
					: std::move(DirectoryError));
			for (size_t Index = 0; Index < Sections.size(); ++Index)
				Owned[Index + 2].assign(Sections[Index].begin(), Sections[Index].end());

			const uint64 DirectoryOffset = BinaryEnvelopePreambleBytes + FormatHeaderBytes;
			uint64 Offset = DirectoryOffset + RequiredSectionCount * SectionEntryBytes;
			std::array<FSectionEntry, RequiredSectionCount> Entries;
			for (size_t Index = 0; Index < Owned.size(); ++Index)
			{
				Entries[Index] = {.Kind = static_cast<uint32>(Index + 1),
					.Flags = RequiredSectionFlag, .Offset = Offset, .Size = Owned[Index].size(),
					.Hash = FXxHash128::HashBuffer(Owned[Index])};
				if (Owned[Index].size() > MaximumFileBytes - Offset)
					return Error("DAST v7 section extents exceed the file bound.");
				Offset += Owned[Index].size();
			}
			const uint64 HeaderBytes = Entries[1].Offset + Entries[1].Size;
			if (HeaderBytes > MaximumHeaderBytes || Offset > MaximumFileBytes)
				return Error("DAST v7 declared extents exceed configured limits.");
			std::vector<std::byte> Bytes(static_cast<size_t>(Offset));
			const FBinaryEnvelopePreamble Preamble{.FormatId = DastBinaryFormatId,
				.FormatVersion = AssetPackageV7FormatVersion, .RequiredFeatures = 0,
				.HeaderBytes = HeaderBytes, .FileBytes = Offset};
			if (!EncodeBinaryEnvelopePreamble(Preamble, Bytes))
				return Error("DAST v7 common preamble encoding failed.");
			WriteAt(Bytes, 64, static_cast<uint32>(Header.EntryKind));
			WriteAt(Bytes, 68, uint32{0});
			WriteAt(Bytes, 72, DirectoryOffset);
			WriteAt(Bytes, 80, RequiredSectionCount);
			WriteAt(Bytes, 84, SectionEntryBytes);
			WriteAt(Bytes, 88, uint64{0});
			for (size_t Index = 0; Index < Entries.size(); ++Index)
			{
				const uint64 EntryOffset = DirectoryOffset + Index * SectionEntryBytes;
				WriteAt(Bytes, EntryOffset, Entries[Index].Kind);
				WriteAt(Bytes, EntryOffset + 4, Entries[Index].Flags);
				WriteAt(Bytes, EntryOffset + 8, Entries[Index].Offset);
				WriteAt(Bytes, EntryOffset + 16, Entries[Index].Size);
				WriteAt(Bytes, EntryOffset + 24, Entries[Index].Hash.HashLow);
				WriteAt(Bytes, EntryOffset + 32, Entries[Index].Hash.HashHigh);
				WriteAt(Bytes, EntryOffset + 40, uint64{0});
				std::ranges::copy(Owned[Index], Bytes.begin() + Entries[Index].Offset);
			}
			if (!FinalizeBinaryEnvelopeHeader(std::span(Bytes).first(static_cast<size_t>(HeaderBytes)),
				Offset, EnvelopeLimits)) return Error("DAST v7 common header finalization failed.");
			OutBytes = std::move(Bytes);
			return {};
		}

		auto ParseWire(
			std::span<const std::byte> Bytes,
			FParsedPackage& OutPackage,
			std::string* OutError) -> bool
		{
			FBinaryEnvelopePreamble Preamble;
			FBinaryEnvelopeDiagnostic EnvelopeDiagnostic;
			if (!ParseBinaryEnvelopePrefix(
				Bytes, Bytes.size(), EnvelopeLimits, Preamble, &EnvelopeDiagnostic))
				return Fail(std::string(EnvelopeDiagnostic.Message), OutError);
			if (Preamble.HeaderBytes > Bytes.size())
				return Fail("DAST v6 front matter is truncated.", OutError);
			FValidatedBinaryEnvelope Envelope;
			if (!ValidateBinaryEnvelopeHeader(
				Bytes.first(static_cast<size_t>(Preamble.HeaderBytes)), Bytes.size(),
				EnvelopeLimits, GetRegistry(), Envelope, &EnvelopeDiagnostic))
				return Fail(std::string(EnvelopeDiagnostic.Message), OutError);

			uint32 PackageKind = 0;
			uint32 PackageFlags = 0;
			uint64 DirectoryOffset = 0;
			uint32 SectionCount = 0;
			uint32 EntryBytes = 0;
			uint64 Reserved = 0;
			if (!ReadAt(Bytes, 64, PackageKind) || PackageKind > 1
				|| !ReadAt(Bytes, 68, PackageFlags) || PackageFlags != 0
				|| !ReadAt(Bytes, 72, DirectoryOffset)
				|| !ReadAt(Bytes, 80, SectionCount)
				|| !ReadAt(Bytes, 84, EntryBytes) || EntryBytes != SectionEntryBytes
				|| !ReadAt(Bytes, 88, Reserved) || Reserved != 0
				|| SectionCount < RequiredSectionCount || SectionCount > MaximumSectionCount
				|| DirectoryOffset != BinaryEnvelopePreambleBytes + FormatHeaderBytes)
				return Fail("DAST v6 format header is invalid or unsupported.", OutError);
			const uint64 DirectoryBytes = uint64(SectionCount) * SectionEntryBytes;
			if (DirectoryOffset > Preamble.HeaderBytes
				|| DirectoryBytes > Preamble.HeaderBytes - DirectoryOffset)
				return Fail("DAST v6 section directory exceeds HeaderBytes.", OutError);

			FParsedPackage Result;
			Result.FormatVersion = Preamble.FormatVersion;
			Result.HeaderBytes = Preamble.HeaderBytes;
			uint64 ExpectedOffset = DirectoryOffset + DirectoryBytes;
			uint32 PreviousKind = 0;
			uint64 ImportEnd = 0;
			for (uint32 Index = 0; Index < SectionCount; ++Index)
			{
				const uint64 EntryOffset = DirectoryOffset + uint64(Index) * SectionEntryBytes;
				FSectionEntry Entry;
				uint64 EntryReserved = 0;
				if (!ReadAt(Bytes, EntryOffset, Entry.Kind)
					|| !ReadAt(Bytes, EntryOffset + 4, Entry.Flags)
					|| !ReadAt(Bytes, EntryOffset + 8, Entry.Offset)
					|| !ReadAt(Bytes, EntryOffset + 16, Entry.Size)
					|| !ReadAt(Bytes, EntryOffset + 24, Entry.Hash.HashLow)
					|| !ReadAt(Bytes, EntryOffset + 32, Entry.Hash.HashHigh)
					|| !ReadAt(Bytes, EntryOffset + 40, EntryReserved) || EntryReserved != 0
					|| Entry.Kind <= PreviousKind || (Entry.Flags & ~RequiredSectionFlag) != 0
					|| Entry.Offset != ExpectedOffset || Entry.Offset > Bytes.size()
					|| Entry.Size > Bytes.size() - Entry.Offset)
					return Fail("DAST v6 section entry is invalid or noncanonical.", OutError);
				const std::span<const std::byte> Section = Bytes.subspan(
					static_cast<size_t>(Entry.Offset), static_cast<size_t>(Entry.Size));
				if (FXxHash128::HashBuffer(Section) != Entry.Hash)
					return Fail("DAST v6 section hash verification failed.", OutError);
				if (Index < RequiredSectionCount)
				{
					if (Entry.Kind != Index + 1 || Entry.Flags != RequiredSectionFlag)
						return Fail("DAST v6 required sections are missing or out of order.", OutError);
					Result.RequiredEntries[Index] = Entry;
					Result.RequiredSections[Index] = Section;
					if (Index == 1) ImportEnd = Entry.Offset + Entry.Size;
				}
				else
				{
					if ((Entry.Flags & RequiredSectionFlag) != 0)
						return Fail("DAST v6 contains an unknown required section.", OutError);
					Result.bHasUnknownSkippableSections = true;
				}
				ExpectedOffset += Entry.Size;
				PreviousKind = Entry.Kind;
			}
			if (ExpectedOffset != Bytes.size() || ImportEnd != Preamble.HeaderBytes)
				return Fail("DAST v6 sections leave gaps, trailing bytes, or invalid HeaderBytes.", OutError);
			Dast::FPublicSummary Summary;
			if (!Dast::DecodePublicSummary(Result.RequiredSections[0],
				Result.RequiredSections[1], static_cast<EAssetRegistryEntryKind>(PackageKind),
				Summary, OutError))
				return false;
			Result.EntryKind = Summary.EntryKind;
			Result.MainExportIndex = Summary.MainExportIndex;
			Result.AssetClass = std::move(Summary.AssetClass);
			Result.RedirectDestination = std::move(Summary.RedirectDestination);
			Result.Imports = std::move(Summary.Imports);
			Result.ExportCount = Summary.ExportCount;
			Result.BulkSegment = {
				.Extent = Summary.BulkSegmentExtent,
				.Digest = Summary.BulkSegmentDigest};
			Result.ExpectedImportCount = Result.Imports.size();
			Result.ExpectedPayloadCount = Summary.PayloadCount;
			if (Result.FormatVersion == AssetPackageV6FormatVersion)
			{
				Result.PayloadEntries.reserve(static_cast<size_t>(Summary.PayloadCount));
				if (!DecodePayloadDirectory(Result.RequiredSections[7], Result, OutError))
					return false;
			}
			else
			{
				if (!DecodePackageBulkDataDirectory(
					Result.RequiredSections[7], Result.BulkEntries, OutError)
					|| Result.BulkEntries.size() != Summary.PayloadCount
					|| !ValidatePackageBulkDataMetadata(
						Result.BulkSegment, Result.BulkEntries, OutError)) return false;
			}
			OutPackage = std::move(Result);
			if (OutError) OutError->clear();
			return true;
		}

		auto MakeObjectStream(std::span<const std::byte> Bytes,
			std::vector<std::byte>& OutObjectStream, bool bAllowUnknown,
			FParsedPackage* OutParsed = nullptr,
			FAssetPackageInspection* OutInspection = nullptr) -> FAssetResult
		{
			FParsedPackage Parsed;
			std::string ParseError;
			if (!ParseWire(Bytes, Parsed, &ParseError)) return Error(std::move(ParseError));
			if (Parsed.bHasUnknownSkippableSections && !bAllowUnknown)
				return Error("DAST v6 mutation cannot preserve unknown skippable sections.");
			if (FAssetResult Result = PrepareObjectStream(
					Parsed, OutObjectStream, OutInspection); !Result)
				return Result;
			if (OutParsed) *OutParsed = std::move(Parsed);
			return {};
		}

		auto BuildV6FromObjectStream(std::span<const std::byte> ObjectStream,
			std::vector<std::byte>& OutBytes) -> FAssetResult
		{
			PackageObjectStream::FValidatedHeader Header;
			PackageObjectStream::FReaderDiagnostic Diagnostic;
			if (!PackageObjectStream::ReadHeader(
					ObjectStream, Header, {}, &Diagnostic, ObjectStream.size()))
				return Error(Diagnostic.Message);
			FAssetPackageInspection Inspection;
			if (FAssetResult Result = PackageObjectStream::InspectPackage(
					ObjectStream, Inspection, {}, &Diagnostic); !Result)
				return Result;
			std::vector<FPayloadEntry> Entries;
			if (FAssetResult Result = MakePayloadEntries(Inspection, Entries); !Result)
				return Result;
			std::array<std::span<const std::byte>, 5> Sections;
			for (size_t Index = 0; Index < Sections.size(); ++Index)
				Sections[Index] = ObjectStream.subspan(
					Header.Sections[Index].Offset, Header.Sections[Index].Length);
			return BuildV6Package(Header, Sections, Entries, OutBytes);
		}

		auto BuildV7FromObjectStream(std::span<const std::byte> ObjectStream,
			std::span<const FPackageBulkDataEntry> Entries,
			const FPackageBulkSegmentSummary& Segment,
			std::vector<std::byte>& OutBytes) -> FAssetResult
		{
			PackageObjectStream::FValidatedHeader Header;
			PackageObjectStream::FReaderDiagnostic Diagnostic;
			if (!PackageObjectStream::ReadHeader(
					ObjectStream, Header, {}, &Diagnostic, ObjectStream.size()))
				return Error(Diagnostic.Message);
			std::array<std::span<const std::byte>, 5> Sections;
			for (size_t Index = 0; Index < Sections.size(); ++Index)
				Sections[Index] = ObjectStream.subspan(
					Header.Sections[Index].Offset, Header.Sections[Index].Length);
			return BuildV7Package(Header, Sections, Entries, Segment, OutBytes);
		}

		auto ReadHeader(std::span<const std::byte> Bytes, uint64 PackageSize,
			FAssetPackageHeader& OutHeader) -> FAssetResult
		{
			return ReadAssetPackageHeaderBytes(Bytes, PackageSize, OutHeader);
		}

		auto Validate(std::span<const std::byte> Bytes) -> FAssetResult
		{
			std::vector<std::byte> ObjectStream;
			return MakeObjectStream(Bytes, ObjectStream, true);
		}

		auto Inspect(std::span<const std::byte> Bytes,
			FAssetPackageInspection& OutInspection) -> FAssetResult
		{
			FAssetPackageInspection Inspection;
			std::vector<std::byte> ObjectStream;
			if (FAssetResult Result = MakeObjectStream(
					Bytes, ObjectStream, true, nullptr, &Inspection); !Result)
				return Result;
			Inspection.Header.FormatVersion = Version;
			Inspection.Fingerprint = {
				.FileSize = Bytes.size(),
				.ContentHash = FXxHash128::HashBuffer(Bytes),
				.ReaderVersion = Version};
			OutInspection = std::move(Inspection);
			return {};
		}

		auto ExtractReferences(std::span<const std::byte> Bytes,
			const FAssetPath& Source, std::vector<FAssetReferenceEdge>& Out) -> FAssetResult
		{
			std::vector<std::byte> ObjectStream;
			if (FAssetResult Result = MakeObjectStream(Bytes, ObjectStream, true); !Result)
				return Result;
			PackageObjectStream::FReaderDiagnostic Diagnostic;
			return PackageObjectStream::ExtractReferences(
				ObjectStream, Source, Out, {}, &Diagnostic);
		}

		auto ProbeCompatibility(std::span<const std::byte> Bytes,
			const FAssetPath& Path, const FReflectionCompatibilityCatalog& Catalog,
			FAssetPackageCompatibilityRecord& OutRecord,
			FAssetCompatibilityProbeStats* OutStats) -> FAssetResult
		{
			std::vector<std::byte> ObjectStream;
			if (FAssetResult Result = MakeObjectStream(Bytes, ObjectStream, true); !Result)
				return Result;
			PackageObjectStream::FReaderDiagnostic Diagnostic;
			FAssetResult Result = PackageObjectStream::ProbeCompatibility(
				ObjectStream, Path, Catalog, OutRecord, OutStats, {}, &Diagnostic);
			if (Result)
			{
				OutRecord.FormatVersion = Version;
				OutRecord.Fingerprint = {
					.FileSize = Bytes.size(), .ContentHash = FXxHash128::HashBuffer(Bytes),
					.ReaderVersion = Version};
			}
			return Result;
		}

		auto Load(std::span<const std::byte> Bytes, const FAssetPath& Path,
			DPackage*& OutPackage, FAssetLoadReport* OutReport,
			const std::function<FAssetResult(DPackage*)>& OnSkeletonReady,
			const std::function<void(DPackage*)>& OnSkeletonRollback) -> FAssetResult
		{
			OutPackage = nullptr;
			std::vector<std::byte> ObjectStream;
			if (FAssetResult Result = MakeObjectStream(Bytes, ObjectStream, true); !Result)
				return Result;
			PackageObjectStream::FLoadedAssetPackage Loaded;
			PackageObjectStream::FReaderDiagnostic Diagnostic;
			FAssetResult Result = PackageObjectStream::LoadAssetPackage(
				ObjectStream, Path, Loaded, OutReport,
				{.OnSkeletonReady = OnSkeletonReady,
					.OnSkeletonRollback = OnSkeletonRollback}, {}, &Diagnostic);
			if (!Result) return Result;
			OutPackage = Loaded.Release();
			return {};
		}

		auto Write(DPackage* Package, std::vector<std::byte>& OutBytes,
			EDefaultDeltaMode DeltaMode,
			const FAssetPackageSerializationOptions& Options) -> FAssetResult
		{
			std::vector<std::byte> ObjectStream;
			PackageObjectStream::FWriterDiagnostic Diagnostic;
			if (FAssetResult Result = PackageObjectStream::WriteAssetPackage(
					Package, ObjectStream,
					{.DeltaMode = DeltaMode, .Serialization = Options}, &Diagnostic); !Result)
				return Result;
			return BuildV6FromObjectStream(ObjectStream, OutBytes);
		}

		auto InspectV7(std::span<const std::byte> Bytes,
			FAssetPackageInspection& OutInspection) -> FAssetResult
		{
			FAssetResult Result = Inspect(Bytes, OutInspection);
			if (Result)
			{
				FParsedPackage Parsed;
				std::string ParseError;
				if (!ParseWire(Bytes, Parsed, &ParseError)) return Error(std::move(ParseError));
				OutInspection.Header.FormatVersion = AssetPackageV7FormatVersion;
				OutInspection.Header.BulkSegmentExtent = Parsed.BulkSegment.Extent;
				OutInspection.Header.BulkSegmentDigest = Parsed.BulkSegment.Digest;
				OutInspection.Fingerprint.ReaderVersion = AssetPackageV7FormatVersion;
			}
			return Result;
		}

		auto ProbeCompatibilityV7(std::span<const std::byte> Bytes,
			const FAssetPath& Path, const FReflectionCompatibilityCatalog& Catalog,
			FAssetPackageCompatibilityRecord& OutRecord,
			FAssetCompatibilityProbeStats* OutStats) -> FAssetResult
		{
			FAssetResult Result = ProbeCompatibility(Bytes, Path, Catalog, OutRecord, OutStats);
			if (Result)
			{
				OutRecord.FormatVersion = AssetPackageV7FormatVersion;
				OutRecord.Fingerprint.ReaderVersion = AssetPackageV7FormatVersion;
			}
			return Result;
		}

		auto LoadV7(std::span<const std::byte> Bytes, const FAssetPath& Path,
			DPackage*& OutPackage, FAssetLoadReport* OutReport,
			const std::function<FAssetResult(DPackage*)>& OnSkeletonReady,
			const std::function<void(DPackage*)>& OnSkeletonRollback) -> FAssetResult
		{
			OutPackage = nullptr;
			std::vector<std::byte> ObjectStream;
			FParsedPackage Parsed;
			if (FAssetResult Result = MakeObjectStream(Bytes, ObjectStream, true, &Parsed); !Result)
				return Result;
			FPackageResourceHandle Resource;
			const PathUtilities::FAssetPathResult Resolved = PathUtilities::ResolveAssetPath(
				Path.GetView(), PathUtilities::EPathExistence::AllowMissing);
			if (!Resolved) return Error("DAST v7 package path cannot resolve its bulk segment.");
			std::filesystem::path PackagePath = Resolved.PhysicalPath;
			PackagePath += ".dasset";
			std::filesystem::path LegacyPath = PackagePath;
			LegacyPath.replace_extension(".dabulk");
			std::filesystem::path SegmentPath = PackagePath;
			SegmentPath.replace_extension(".dbulk");
			std::error_code FileError;
			if (std::filesystem::exists(LegacyPath, FileError))
				return Error("DAST v7 package conflicts with a legacy DABK companion.");
			if (Parsed.BulkSegment.Extent != 0)
			{
				std::string ResourceError;
				if (!GetPackageResourceManager().RegisterLoosePackage(
						Path.ToString(), PackagePath, Parsed.BulkSegment, Parsed.BulkEntries,
						Resource, &ResourceError)) return Error(std::move(ResourceError));
			}
			PackageObjectStream::FLoadedAssetPackage Loaded;
			PackageObjectStream::FReaderDiagnostic Diagnostic;
			FAssetResult Result = PackageObjectStream::LoadAssetPackage(
				ObjectStream, Path, Loaded, OutReport,
				{.OnSkeletonReady = OnSkeletonReady, .OnSkeletonRollback = OnSkeletonRollback,
					.SourceFormatVersion = AssetPackageV7FormatVersion}, {}, &Diagnostic);
			if (!Result)
			{
				if (Resource) GetPackageResourceManager().RetirePackage(Path.ToString());
				return Result;
			}
			OutPackage = Loaded.Release();
			return {};
		}

		auto WriteV7(DPackage* Package, std::vector<std::byte>& OutBytes,
			EDefaultDeltaMode DeltaMode,
			const FAssetPackageSerializationOptions& Options) -> FAssetResult
		{
			std::vector<FEditorBulkDataStoragePayload> Payloads;
			FAssetPackageSerializationOptions EffectiveOptions = Options;
			EffectiveOptions.EditorBulkDataStoragePayloads = &Payloads;
			std::vector<std::byte> ObjectStream;
			PackageObjectStream::FWriterDiagnostic Diagnostic;
			if (FAssetResult Result = PackageObjectStream::WriteAssetPackage(
					Package, ObjectStream,
					{.DeltaMode = DeltaMode, .Serialization = EffectiveOptions}, &Diagnostic); !Result)
				return Result;
			if (Options.EditorBulkDataStoragePayloads)
				*Options.EditorBulkDataStoragePayloads = Payloads;
			std::vector<std::byte> Segment;
			FPackageBulkSegmentSummary Summary;
			std::vector<FPackageBulkDataEntry> Entries;
			std::string SegmentError;
			if (!BuildPackageBulkDataSegment(Payloads, Segment, Summary, Entries, &SegmentError))
				return Error(std::move(SegmentError));
			return BuildV7FromObjectStream(ObjectStream, Entries, Summary, OutBytes);
		}

		auto RewriteReferencesV7(std::span<const std::byte> Bytes,
			std::span<const FAssetRedirectorFixupMapping> Mappings,
			uint64 ExpectedCount, std::vector<std::byte>& OutBytes) -> FAssetResult
		{
			std::vector<std::byte> ObjectStream;
			FParsedPackage Parsed;
			if (FAssetResult Result = MakeObjectStream(Bytes, ObjectStream, false, &Parsed); !Result)
				return Result;
			std::vector<std::byte> Rewritten;
			if (FAssetResult Result = PackageObjectStream::RewriteReferences(
					ObjectStream, Mappings, ExpectedCount, Rewritten); !Result) return Result;
			return BuildV7FromObjectStream(Rewritten, Parsed.BulkEntries, Parsed.BulkSegment, OutBytes);
		}

		auto RelocateV7(std::span<const std::byte> Bytes, const FAssetPath& Destination,
			std::vector<std::byte>& OutBytes) -> FAssetResult
		{
			std::vector<std::byte> ObjectStream;
			FParsedPackage Parsed;
			if (FAssetResult Result = MakeObjectStream(Bytes, ObjectStream, false, &Parsed); !Result)
				return Result;
			std::vector<std::byte> Relocated;
			if (FAssetResult Result = PackageObjectStream::RelocatePackage(
					ObjectStream, Destination, Relocated); !Result) return Result;
			return BuildV7FromObjectStream(Relocated, Parsed.BulkEntries, Parsed.BulkSegment, OutBytes);
		}

		auto WriteRedirectorV7(const FAssetPath& Source, const FAssetPath& Destination,
			std::vector<std::byte>& OutBytes) -> FAssetResult
		{
			std::vector<std::byte> ObjectStream;
			if (FAssetResult Result = PackageObjectStream::WriteRedirectorPackage(
					Source, Destination, ObjectStream); !Result) return Result;
			return BuildV7FromObjectStream(ObjectStream, {}, {}, OutBytes);
		}

		auto RewriteReferences(std::span<const std::byte> Bytes,
			std::span<const FAssetRedirectorFixupMapping> Mappings,
			uint64 ExpectedCount, std::vector<std::byte>& OutBytes) -> FAssetResult
		{
			std::vector<std::byte> ObjectStream;
			if (FAssetResult Result = MakeObjectStream(Bytes, ObjectStream, false); !Result)
				return Result;
			std::vector<std::byte> Rewritten;
			if (FAssetResult Result = PackageObjectStream::RewriteReferences(
					ObjectStream, Mappings, ExpectedCount, Rewritten); !Result)
				return Result;
			return BuildV6FromObjectStream(Rewritten, OutBytes);
		}

		auto Relocate(std::span<const std::byte> Bytes, const FAssetPath& Destination,
			std::vector<std::byte>& OutBytes) -> FAssetResult
		{
			std::vector<std::byte> ObjectStream;
			if (FAssetResult Result = MakeObjectStream(Bytes, ObjectStream, false); !Result)
				return Result;
			std::vector<std::byte> Relocated;
			if (FAssetResult Result = PackageObjectStream::RelocatePackage(
					ObjectStream, Destination, Relocated); !Result)
				return Result;
			return BuildV6FromObjectStream(Relocated, OutBytes);
		}

		auto WriteRedirector(const FAssetPath& Source, const FAssetPath& Destination,
			std::vector<std::byte>& OutBytes) -> FAssetResult
		{
			std::vector<std::byte> ObjectStream;
			if (FAssetResult Result = PackageObjectStream::WriteRedirectorPackage(
					Source, Destination, ObjectStream); !Result)
				return Result;
			return BuildV6FromObjectStream(ObjectStream, OutBytes);
		}
	}

	auto ParsePackage(std::span<const std::byte> Bytes,
		FParsedPackage& OutPackage, std::string* OutError) -> bool
	{
		FParsedPackage Parsed;
		if (!ParseWire(Bytes, Parsed, OutError)) return false;
		std::vector<std::byte> ObjectStream;
		if (FAssetResult Result = PrepareObjectStream(Parsed, ObjectStream); !Result)
			return Fail(Result.Message, OutError);
		OutPackage = std::move(Parsed);
		return true;
	}

	auto BuildPackageFromObjectStream(std::span<const std::byte> ObjectStreamBytes,
		std::vector<std::byte>& OutV6Bytes) -> FAssetResult
	{
		return BuildV6FromObjectStream(ObjectStreamBytes, OutV6Bytes);
	}

	auto ExtractObjectStream(std::span<const std::byte> V6Bytes,
		std::vector<std::byte>& OutObjectStream) -> FAssetResult
	{
		return MakeObjectStream(V6Bytes, OutObjectStream, true);
	}

	auto GetCodec() -> const FAssetPackageCodec&
	{
		static const FAssetPackageCodec Codec{
			.CodecId = "dast-v6",
			.FormatVersion = Version,
			.bCanRead = true,
			.bCanWrite = false,
			.bCanMutate = false,
			.ReadHeader = &ReadHeader,
			.Validate = &Validate,
			.Inspect = &Inspect,
			.ExtractReferences = &ExtractReferences,
			.ProbeCompatibility = &ProbeCompatibility,
			.Load = &Load,
			.Write = &Write,
			.RewriteReferences = &RewriteReferences,
			.Relocate = &Relocate,
			.WriteRedirector = &WriteRedirector};
		return Codec;
	}

	auto GetV7Codec() -> const FAssetPackageCodec&
	{
		static const FAssetPackageCodec Codec{
			.CodecId = "dast-v7",
			.FormatVersion = AssetPackageV7FormatVersion,
			.bCanRead = true,
			.bCanWrite = true,
			.bCanMutate = true,
			.ReadHeader = &ReadHeader,
			.Validate = &Validate,
			.Inspect = &InspectV7,
			.ExtractReferences = &ExtractReferences,
			.ProbeCompatibility = &ProbeCompatibilityV7,
			.Load = &LoadV7,
			.Write = &WriteV7,
			.RewriteReferences = &RewriteReferencesV7,
			.Relocate = &RelocateV7,
			.WriteRedirector = &WriteRedirectorV7};
		return Codec;
	}
}
