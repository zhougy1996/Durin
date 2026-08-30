#include "AssetPackageV7Codec.h"
#include "AssetPackageByteSource.h"

#include "Asset/Compatibility.h"
#include "Asset/Cook.h"
#include "Asset/EditorBulkDataStorage.h"
#include "Asset/Load.h"
#include "Asset/PackageResource.h"
#include "Asset/PackageObjectStreamReader.h"
#include "Asset/PackageObjectStreamWriter.h"
#include "AssetRegistry/PackageHeader.h"
#include "Serialization/BinaryEnvelope.h"
#include "Serialization/BinaryFormat.h"
#include "PackageBulkDataWire.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace Durin::Asset::Private::DastV7
{
	namespace
	{
		constexpr uint32 ImportVersion = 1;
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
					.MinimumFormatVersion = AssetPackageV7FormatVersion,
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
					return Error(std::format("{} (DAST v7 imports: {})", Result.Message,
					std::accumulate(Package.Imports.begin(), Package.Imports.end(),
						std::string{}, [](std::string Value, const std::string& Import) {
							if (!Value.empty()) Value += ", ";
							Value += Import;
							return Value;
						})));
			for (FAssetPackageObjectInspection& Object : Inspection.Objects)
				for (FAssetPackageField& Field : Object.Fields)
					Field.SourceFormatVersion = AssetPackageV7FormatVersion;
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
			if (OutInspection) *OutInspection = std::move(Inspection);
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
				return Fail("DAST v7 front matter is truncated.", OutError);
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
				return Fail("DAST v7 format header is invalid or unsupported.", OutError);
			const uint64 DirectoryBytes = uint64(SectionCount) * SectionEntryBytes;
			if (DirectoryOffset > Preamble.HeaderBytes
				|| DirectoryBytes > Preamble.HeaderBytes - DirectoryOffset)
				return Fail("DAST v7 section directory exceeds HeaderBytes.", OutError);

			FParsedPackage Result;
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
					return Fail("DAST v7 section entry is invalid or noncanonical.", OutError);
				const std::span<const std::byte> Section = Bytes.subspan(
					static_cast<size_t>(Entry.Offset), static_cast<size_t>(Entry.Size));
				if (FXxHash128::HashBuffer(Section) != Entry.Hash)
					return Fail("DAST v7 section hash verification failed.", OutError);
				if (Index < RequiredSectionCount)
				{
					if (Entry.Kind != Index + 1 || Entry.Flags != RequiredSectionFlag)
						return Fail("DAST v7 required sections are missing or out of order.", OutError);
					Result.RequiredEntries[Index] = Entry;
					Result.RequiredSections[Index] = Section;
					if (Index == 1) ImportEnd = Entry.Offset + Entry.Size;
				}
				else
				{
					if ((Entry.Flags & RequiredSectionFlag) != 0)
						return Fail("DAST v7 contains an unknown required section.", OutError);
					Result.bHasUnknownSkippableSections = true;
				}
				ExpectedOffset += Entry.Size;
				PreviousKind = Entry.Kind;
			}
			if (ExpectedOffset != Bytes.size() || ImportEnd != Preamble.HeaderBytes)
				return Fail("DAST v7 sections leave gaps, trailing bytes, or invalid HeaderBytes.", OutError);
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
			if (!DecodePackageBulkDataDirectory(
					Result.RequiredSections[7], Result.BulkEntries, OutError)
				|| Result.BulkEntries.size() != Summary.PayloadCount
				|| !ValidatePackageBulkDataMetadata(
					Result.BulkSegment, Result.BulkEntries, OutError)) return false;
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
				return Error("DAST v7 mutation cannot preserve unknown skippable sections.");
			if (FAssetResult Result = PrepareObjectStream(
					Parsed, OutObjectStream, OutInspection); !Result)
				return Result;
			if (OutParsed) *OutParsed = std::move(Parsed);
			return {};
		}

		auto ReadCompatibilityRange(IAssetPackageByteSource& Source, uint64 Offset,
			std::span<std::byte> Output,
			const FAssetCompatibilityCancellationCheck& IsCancelled,
			std::string& OutError) -> bool
		{
			constexpr size_t ScratchReadBytes = 64 * 1024;
			for (size_t Complete = 0; Complete < Output.size();)
			{
				if (IsCancelled && IsCancelled())
					return Fail("Asset compatibility inspection was cancelled.", &OutError);
				const size_t Count = std::min(ScratchReadBytes, Output.size() - Complete);
				if (!Source.ReadAt(Offset + Complete, Output.subspan(Complete, Count), &OutError))
					return false;
				Complete += Count;
			}
			if (Output.empty() && IsCancelled && IsCancelled())
				return Fail("Asset compatibility inspection was cancelled.", &OutError);
			return true;
		}

		auto ParseCompatibilityMetadata(IAssetPackageByteSource& Source,
			std::array<std::vector<std::byte>, RequiredSectionCount>& Owned,
			FParsedPackage& OutPackage,
			const FAssetCompatibilityCancellationCheck& IsCancelled,
			std::string& OutError) -> bool
		{
			const size_t PrefixSize = static_cast<size_t>(std::min<uint64>(
				Source.GetSize(), BinaryEnvelopePreambleBytes));
			std::vector<std::byte> Prefix(PrefixSize);
			if (!ReadCompatibilityRange(Source, 0, Prefix, IsCancelled, OutError)) return false;
			FBinaryEnvelopePreamble Preamble;
			FBinaryEnvelopeDiagnostic EnvelopeDiagnostic;
			if (!ParseBinaryEnvelopePrefix(Prefix, Source.GetSize(), EnvelopeLimits,
				Preamble, &EnvelopeDiagnostic))
				return Fail(std::string(EnvelopeDiagnostic.Message), &OutError);
			if (Preamble.HeaderBytes > std::numeric_limits<size_t>::max())
				return Fail("DAST v7 front matter exceeds the addressable range.", &OutError);
			std::vector<std::byte> Header(static_cast<size_t>(Preamble.HeaderBytes));
			if (!ReadCompatibilityRange(Source, 0, Header, IsCancelled, OutError)) return false;
			FValidatedBinaryEnvelope Envelope;
			if (!ValidateBinaryEnvelopeHeader(Header, Source.GetSize(), EnvelopeLimits,
				GetRegistry(), Envelope, &EnvelopeDiagnostic))
				return Fail(std::string(EnvelopeDiagnostic.Message), &OutError);

			uint32 PackageKind = 0, PackageFlags = 0, SectionCount = 0, EntryBytes = 0;
			uint64 DirectoryOffset = 0, Reserved = 0;
			if (!ReadAt(Header, 64, PackageKind) || PackageKind > 1
				|| !ReadAt(Header, 68, PackageFlags) || PackageFlags != 0
				|| !ReadAt(Header, 72, DirectoryOffset)
				|| !ReadAt(Header, 80, SectionCount)
				|| !ReadAt(Header, 84, EntryBytes) || EntryBytes != SectionEntryBytes
				|| !ReadAt(Header, 88, Reserved) || Reserved != 0
				|| SectionCount < RequiredSectionCount || SectionCount > MaximumSectionCount
				|| DirectoryOffset != BinaryEnvelopePreambleBytes + FormatHeaderBytes)
				return Fail("DAST v7 format header is invalid or unsupported.", &OutError);
			const uint64 DirectoryBytes = uint64(SectionCount) * SectionEntryBytes;
			if (DirectoryOffset > Preamble.HeaderBytes
				|| DirectoryBytes > Preamble.HeaderBytes - DirectoryOffset)
				return Fail("DAST v7 section directory exceeds HeaderBytes.", &OutError);

			FParsedPackage Result;
			Result.HeaderBytes = Preamble.HeaderBytes;
			uint64 ExpectedOffset = DirectoryOffset + DirectoryBytes;
			uint32 PreviousKind = 0;
			uint64 ImportEnd = 0;
			for (uint32 Index = 0; Index < SectionCount; ++Index)
			{
				const uint64 EntryOffset = DirectoryOffset + uint64(Index) * SectionEntryBytes;
				FSectionEntry Entry; uint64 EntryReserved = 0;
				if (!ReadAt(Header, EntryOffset, Entry.Kind)
					|| !ReadAt(Header, EntryOffset + 4, Entry.Flags)
					|| !ReadAt(Header, EntryOffset + 8, Entry.Offset)
					|| !ReadAt(Header, EntryOffset + 16, Entry.Size)
					|| !ReadAt(Header, EntryOffset + 24, Entry.Hash.HashLow)
					|| !ReadAt(Header, EntryOffset + 32, Entry.Hash.HashHigh)
					|| !ReadAt(Header, EntryOffset + 40, EntryReserved) || EntryReserved != 0
					|| Entry.Kind <= PreviousKind || (Entry.Flags & ~RequiredSectionFlag) != 0
					|| Entry.Offset != ExpectedOffset || Entry.Offset > Source.GetSize()
					|| Entry.Size > Source.GetSize() - Entry.Offset)
					return Fail("DAST v7 section entry is invalid or noncanonical.", &OutError);
				if (Index < RequiredSectionCount)
				{
					if (Entry.Kind != Index + 1 || Entry.Flags != RequiredSectionFlag)
						return Fail("DAST v7 required sections are missing or out of order.", &OutError);
					Result.RequiredEntries[Index] = Entry;
					if (Index == 1) ImportEnd = Entry.Offset + Entry.Size;
				}
				else
				{
					if ((Entry.Flags & RequiredSectionFlag) != 0)
						return Fail("DAST v7 contains an unknown required section.", &OutError);
					Result.bHasUnknownSkippableSections = true;
				}
				ExpectedOffset += Entry.Size;
				PreviousKind = Entry.Kind;
			}
			if (ExpectedOffset != Source.GetSize() || ImportEnd != Preamble.HeaderBytes)
				return Fail("DAST v7 sections leave gaps, trailing bytes, or invalid HeaderBytes.", &OutError);

			for (size_t Index : {size_t{0}, size_t{1}, size_t{2}, size_t{3}, size_t{4}, size_t{5}, size_t{7}})
			{
				const FSectionEntry& Entry = Result.RequiredEntries[Index];
				if (Entry.Size > std::numeric_limits<size_t>::max())
					return Fail("DAST v7 metadata section exceeds the addressable range.", &OutError);
				Owned[Index].resize(static_cast<size_t>(Entry.Size));
				if (Entry.Offset + Entry.Size <= Header.size())
					std::ranges::copy(std::span(Header).subspan(static_cast<size_t>(Entry.Offset),
						static_cast<size_t>(Entry.Size)), Owned[Index].begin());
				else if (!ReadCompatibilityRange(Source, Entry.Offset, Owned[Index], IsCancelled, OutError))
					return false;
				if (FXxHash128::HashBuffer(Owned[Index]) != Entry.Hash)
					return Fail("DAST v7 metadata section hash verification failed.", &OutError);
				Result.RequiredSections[Index] = Owned[Index];
			}

			Dast::FPublicSummary Summary;
			if (!Dast::DecodePublicSummary(Result.RequiredSections[0], Result.RequiredSections[1],
				static_cast<EAssetRegistryEntryKind>(PackageKind), Summary, &OutError)) return false;
			Result.EntryKind = Summary.EntryKind;
			Result.MainExportIndex = Summary.MainExportIndex;
			Result.AssetClass = std::move(Summary.AssetClass);
			Result.RedirectDestination = std::move(Summary.RedirectDestination);
			Result.Imports = std::move(Summary.Imports);
			Result.ExportCount = Summary.ExportCount;
			Result.BulkSegment = {.Extent = Summary.BulkSegmentExtent,
				.Digest = Summary.BulkSegmentDigest};
			if (!DecodePackageBulkDataDirectory(Result.RequiredSections[7], Result.BulkEntries, &OutError)
				|| Result.BulkEntries.size() != Summary.PayloadCount
				|| !ValidatePackageBulkDataMetadata(Result.BulkSegment, Result.BulkEntries, &OutError))
				return false;
			OutPackage = std::move(Result);
			return true;
		}

		auto BuildCompatibilityObjectStream(const FParsedPackage& Package,
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
			FWriter Values;
			Values.VarUInt(Package.ExportCount);
			for (uint64 Index = 0; Index < Package.ExportCount; ++Index)
			{
				Values.VarUInt(1);
				Values.VarUInt(0);
			}
			const std::array<std::span<const std::byte>, 5> Sections{
				Package.RequiredSections[2], Package.RequiredSections[3],
				Package.RequiredSections[4], Package.RequiredSections[5], Values.View()};
			uint64 Total = 13 + Summary.View().size() + Sections.size() * 9;
			for (const auto Section : Sections) Total += Section.size();
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
				Writer.Fixed(static_cast<uint8>(Index + 1)); Writer.Fixed(Offset);
				Writer.Fixed(static_cast<uint32>(Sections[Index].size()));
				Offset += static_cast<uint32>(Sections[Index].size());
			}
			for (const auto Section : Sections) Writer.Append(Section);
			OutBytes = Writer.Take();
			return true;
		}

		auto ScanCompatibilityValueDescriptors(IAssetPackageByteSource& Source,
			const FSectionEntry& Section, PackageObjectStream::FDecodedPackage& Package,
			FAssetCompatibilityProbeStats* OutStats,
			const FAssetCompatibilityCancellationCheck& IsCancelled,
			std::string& OutError) -> bool
		{
			uint64 Offset = Section.Offset;
			const uint64 End = Section.Offset + Section.Size;
			auto ReadByte = [&](uint64 Bound, uint8& Out) -> bool {
				if (Offset >= Bound) return Fail(std::format(
					"DAST v7 value descriptor is truncated at physical offset {}.", Offset), &OutError);
				std::byte Byte{};
				if (!ReadCompatibilityRange(Source, Offset, std::span(&Byte, 1), IsCancelled, OutError)) return false;
				Out = std::to_integer<uint8>(Byte); ++Offset; return true;
			};
			auto VarUInt = [&](uint64 Bound, uint64& Out) -> bool {
				const uint64 Start = Offset; uint64 Value = 0;
				for (uint32 Index = 0; Index < 10; ++Index)
				{
					uint8 Byte = 0; if (!ReadByte(Bound, Byte)) return false;
					const uint8 Payload = Byte & 0x7f;
					if (Index == 9 && Payload > 1)
						return Fail(std::format("DAST v7 VarUInt overflows at physical offset {}.", Start), &OutError);
					Value |= uint64(Payload) << (Index * 7);
					if ((Byte & 0x80) == 0)
					{
						if (Index > 0 && Payload == 0)
							return Fail(std::format("DAST v7 VarUInt is noncanonical at physical offset {}.", Start), &OutError);
						Out = Value; return true;
					}
				}
				return Fail(std::format("DAST v7 VarUInt exceeds ten bytes at physical offset {}.", Start), &OutError);
			};
			uint64 ObjectCount = 0;
			if (!VarUInt(End, ObjectCount) || ObjectCount != Package.Objects.size())
				return Fail("DAST v7 value-section object count is invalid.", &OutError);
			Package.ObjectValues.clear();
			Package.ObjectValues.reserve(static_cast<size_t>(ObjectCount));
			for (uint64 ObjectIndex = 0; ObjectIndex < ObjectCount; ++ObjectIndex)
			{
				if (IsCancelled && IsCancelled())
					return Fail("Asset compatibility inspection was cancelled.", &OutError);
				uint64 BlockSize = 0;
				if (!VarUInt(End, BlockSize) || BlockSize > End - Offset)
					return Fail(std::format("DAST v7 object value block is invalid at physical offset {}.", Offset), &OutError);
				const uint64 BlockEnd = Offset + BlockSize;
				uint64 OverrideCount = 0;
				if (!VarUInt(BlockEnd, OverrideCount)
					|| OverrideCount > PackageObjectStream::MaximumTableEntries)
					return Fail("DAST v7 override count exceeds the configured bound.", &OutError);
				PackageObjectStream::FDecodedObjectValues Values;
				uint64 PreviousSchema = 0, PreviousField = 0;
				for (uint64 Index = 0; Index < OverrideCount; ++Index)
				{
					if (IsCancelled && IsCancelled())
						return Fail("Asset compatibility inspection was cancelled.", &OutError);
					PackageObjectStream::FDecodedOverride Override;
					uint8 Provenance = 0;
					if (!VarUInt(BlockEnd, Override.SchemaId) || !VarUInt(BlockEnd, Override.FieldId)
						|| !ReadByte(BlockEnd, Provenance) || Provenance > 2)
						return Fail("DAST v7 override descriptor is invalid.", &OutError);
					Override.Provenance = Provenance;
					if (Override.SchemaId < PreviousSchema
						|| (Override.SchemaId == PreviousSchema && Override.FieldId <= PreviousField)
						|| Override.SchemaId == 0 || Override.SchemaId > Package.Schemas.size()
						|| Override.FieldId == 0
						|| Override.FieldId > Package.Schemas[static_cast<size_t>(Override.SchemaId - 1)].Fields.size())
						return Fail("DAST v7 overrides are unordered or reference an invalid field.", &OutError);
					uint64 PayloadSize = 0;
					if (!VarUInt(BlockEnd, PayloadSize) || PayloadSize > BlockEnd - Offset)
						return Fail(std::format("DAST v7 payload extent is invalid at physical offset {}.", Offset), &OutError);
					Override.PayloadOffset = Offset;
					Override.PayloadSize = PayloadSize;
					Offset += PayloadSize;
					if (OutStats) OutStats->PayloadBytesSkipped += PayloadSize;
					PreviousSchema = Override.SchemaId; PreviousField = Override.FieldId;
					Values.Overrides.push_back(std::move(Override));
				}
				if (Offset != BlockEnd)
					return Fail(std::format("DAST v7 object value block has trailing bytes at physical offset {}.", Offset), &OutError);
				Package.ObjectValues.push_back(std::move(Values));
			}
			return Offset == End || Fail(std::format(
				"DAST v7 value section has trailing bytes at physical offset {}.", Offset), &OutError);
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
			FParsedPackage Parsed;
			std::string ParseError;
			if (!ParseWire(Bytes, Parsed, &ParseError)) return Error(std::move(ParseError));
			Inspection.Header.FormatVersion = AssetPackageV7FormatVersion;
			Inspection.Header.BulkSegmentExtent = Parsed.BulkSegment.Extent;
			Inspection.Header.BulkSegmentDigest = Parsed.BulkSegment.Digest;
			Inspection.Fingerprint = {
				.FileSize = Bytes.size(),
				.ContentHash = FXxHash128::HashBuffer(Bytes),
				.ReaderVersion = AssetPackageV7FormatVersion};
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

		auto ProbeCompatibility(IAssetPackageByteSource& Source,
			const FAssetPath& Path, const FReflectionCompatibilityCatalog& Catalog,
			FAssetPackageCompatibilityRecord& OutRecord,
			FAssetCompatibilityProbeStats* OutStats,
			const FAssetCompatibilityCancellationCheck& IsCancelled) -> FAssetResult
		{
			if (IsCancelled && IsCancelled())
				return {EAssetError::IoError, "Asset compatibility inspection was cancelled."};
			std::array<std::vector<std::byte>, RequiredSectionCount> Owned;
			FParsedPackage Parsed;
			std::string ReadError;
			if (!ParseCompatibilityMetadata(Source, Owned, Parsed, IsCancelled, ReadError))
				return Error(std::move(ReadError));
			std::vector<std::byte> MetadataStream;
			if (!BuildCompatibilityObjectStream(Parsed, MetadataStream))
				return Error("DAST v7 metadata cannot form a bounded compatibility stream.");
			PackageObjectStream::FReaderDiagnostic Diagnostic;
			PackageObjectStream::FDecodedPackage Package;
			if (!PackageObjectStream::DecodePackageDescriptors(
				MetadataStream, Package, {}, &Diagnostic))
				return Error(Diagnostic.Message);
			if (!ScanCompatibilityValueDescriptors(Source, Parsed.RequiredEntries[6],
				Package, OutStats, IsCancelled, ReadError)) return Error(std::move(ReadError));

			const bool bNeedsPayloadValues = std::ranges::any_of(
				Catalog.GetDeprecatedPropertyRoutes(), [&](const auto& Route) {
					return std::ranges::any_of(Package.Schemas, [&](const auto& Schema) {
						return Schema.QualifiedName == Route.DeclaringType;
					});
				});
			FAssetResult Result;
			if (bNeedsPayloadValues)
			{
				if (Source.GetSize() > std::numeric_limits<size_t>::max())
					return Error("DAST v7 package exceeds the addressable range.");
				std::vector<std::byte> Bytes(static_cast<size_t>(Source.GetSize()));
				if (!ReadCompatibilityRange(Source, 0, Bytes, IsCancelled, ReadError))
					return Error(std::move(ReadError));
				std::vector<std::byte> ObjectStream;
				if (Result = MakeObjectStream(Bytes, ObjectStream, true); !Result) return Result;
				FAssetCompatibilityProbeStats FallbackStats;
				Result = PackageObjectStream::ProbeCompatibility(ObjectStream, Path, Catalog,
					OutRecord, &FallbackStats, {}, &Diagnostic);
				if (OutStats)
				{
					OutStats->PayloadBytesSkipped = 0;
					OutStats->PeakMetadataBytes = std::max<uint64>(OutStats->PeakMetadataBytes,
						Bytes.size() + ObjectStream.size());
				}
			}
			else Result = PackageObjectStream::ProbeDecodedCompatibility(std::move(Package),
				Source.GetSize(), false, Path, Catalog, OutRecord, &Diagnostic);
			if (Result)
			{
				OutRecord.FormatVersion = AssetPackageV7FormatVersion;
				OutRecord.Fingerprint.FileSize = Source.GetSize();
				OutRecord.Fingerprint.ReaderVersion = AssetPackageV7FormatVersion;
				if (OutStats && !bNeedsPayloadValues)
				{
					uint64 LiveBytes = MetadataStream.size();
					for (const auto& Section : Owned) LiveBytes += Section.size();
					OutStats->PeakMetadataBytes = std::max(OutStats->PeakMetadataBytes, LiveBytes);
				}
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
			FParsedPackage Parsed;
			if (FAssetResult Result = MakeObjectStream(Bytes, ObjectStream, true, &Parsed); !Result)
				return Result;
			FPackageResourceHandle Resource;
			const FAssetRuntimeConfiguration& Runtime = GetAssetRuntimeConfiguration();
			std::filesystem::path PackagePath;
			bool bCookedFieldPackage = false;
			FArchiveTarget CookTarget;
			if (Runtime.IsCooked())
			{
				std::string ResolveError;
				if (!ResolveCookedPackagePath(
					Runtime.GetCookRoot(), Path.GetView(), PackagePath, &ResolveError))
					return Error(ResolveError.empty()
						? "Cooked DAST v7 package path cannot resolve its bulk segment."
						: std::move(ResolveError));
				std::vector<std::byte> ManifestBytes;
				FCookManifest Manifest;
				if (!FFileHelper::LoadFileToArray(
						ManifestBytes, Runtime.GetCookRoot() / "CookManifest.bin")
					|| !DecodeCookManifest(ManifestBytes, Manifest, &ResolveError))
					return Error(ResolveError.empty()
						? "Cooked DAST v7 manifest cannot be loaded." : std::move(ResolveError));
				const std::string RelativePackage = PackagePath.lexically_relative(
					Runtime.GetCookRoot()).generic_string();
				bCookedFieldPackage = std::ranges::any_of(Manifest.Entries,
					[&](const FCookManifestEntry& Entry) {
						return Entry.Kind == ECookManifestEntryKind::CookedPackage
							&& Entry.RelativePath == RelativePackage
							&& (Entry.Flags & CookManifestEntryCookedFieldProjection) != 0;
					});
				CookTarget.Platform = Manifest.TargetPlatform == ECookTargetPlatform::Win64
					? "Win64" : std::string{};
				CookTarget.Profile = Manifest.TargetProfile == ECookTargetProfile::Game
					? "Game" : "EditorValidation";
			}
			else
			{
				const PathUtilities::FAssetPathResult Resolved = PathUtilities::ResolveAssetPath(
					Path.GetView(), PathUtilities::EPathExistence::AllowMissing);
				if (!Resolved) return Error("DAST v7 package path cannot resolve its bulk segment.");
				PackagePath = Resolved.PhysicalPath;
				PackagePath += ".dasset";
			}
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
					.SourceFormatVersion = AssetPackageV7FormatVersion,
					.bCooked = bCookedFieldPackage,
					.Target = std::move(CookTarget)}, {}, &Diagnostic);
			if (!Result)
			{
				if (Resource) GetPackageResourceManager().RetirePackage(Path.ToString());
				return Result;
			}
			OutPackage = Loaded.Release();
			return {};
		}

		auto Write(DPackage* Package, std::vector<std::byte>& OutBytes,
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

		auto RewriteReferences(std::span<const std::byte> Bytes,
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

		auto Relocate(std::span<const std::byte> Bytes, const FAssetPath& Destination,
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

		auto WriteRedirector(const FAssetPath& Source, const FAssetPath& Destination,
			std::vector<std::byte>& OutBytes) -> FAssetResult
		{
			std::vector<std::byte> ObjectStream;
			if (FAssetResult Result = PackageObjectStream::WriteRedirectorPackage(
					Source, Destination, ObjectStream); !Result) return Result;
			return BuildV7FromObjectStream(ObjectStream, {}, {}, OutBytes);
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
		std::vector<std::byte>& OutBytes) -> FAssetResult
	{
		return BuildV7FromObjectStream(ObjectStreamBytes, {}, {}, OutBytes);
	}

	auto ExtractObjectStream(std::span<const std::byte> Bytes,
		std::vector<std::byte>& OutObjectStream) -> FAssetResult
	{
		return MakeObjectStream(Bytes, OutObjectStream, true);
	}

	auto GetCodec() -> const FAssetPackageCodec&
	{
		static const FAssetPackageCodec Codec{
			.CodecId = "dast-v7",
			.FormatVersion = AssetPackageV7FormatVersion,
			.bCanRead = true,
			.bCanWrite = true,
			.bCanMutate = true,
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

}
