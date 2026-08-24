#include "AssetPackageV6Codec.h"

#include "Asset/PackageObjectStreamReader.h"
#include "AssetPackageV5Codec.h"
#include "Serialization/BinaryFormat.h"

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
					.MaximumFormatVersion = Version,
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
			std::span<const PackageTrailer::FEntry> Entries,
			std::vector<std::byte>& OutBytes) -> bool
		{
			if (Entries.size() > MaximumPayloadCount) return false;
			FWriter Writer;
			Writer.Fixed(PayloadDirectoryVersion);
			Writer.Fixed(PayloadEntryBytes);
			Writer.Fixed(static_cast<uint64>(Entries.size()));
			for (const PackageTrailer::FEntry& Entry : Entries)
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

		auto DecodePublicSummary(std::span<const std::byte> Bytes,
			EAssetRegistryEntryKind EntryKind, FParsedPackage& Out, std::string* OutError) -> bool
		{
			FReader Reader(Bytes);
			uint32 SummaryVersion = 0;
			uint64 ImportCount = 0;
			uint64 PayloadCount = 0;
			uint64 Reserved = 0;
			if (!Reader.Fixed(SummaryVersion) || SummaryVersion != PublicSummaryVersion
				|| !Reader.Fixed(Out.MainExportIndex)
				|| !Reader.Fixed(ImportCount)
				|| !Reader.Fixed(Out.ExportCount)
				|| !Reader.Fixed(PayloadCount)
				|| !Reader.Fixed(Reserved) || Reserved != 0
				|| !Reader.String(Out.AssetClass, false)
				|| !Reader.String(Out.RedirectDestination)
				|| !Reader.AtEnd())
				return Fail("DAST v6 Public Summary is malformed.", OutError);
			if (Out.MainExportIndex != 1 || Out.ExportCount == 0
				|| Out.ExportCount > MaximumExportCount || ImportCount > MaximumImportCount
				|| PayloadCount > MaximumPayloadCount
				|| (EntryKind == EAssetRegistryEntryKind::Asset) != Out.RedirectDestination.empty())
				return Fail("DAST v6 Public Summary values are invalid.", OutError);
			Out.EntryKind = EntryKind;
			Out.ExpectedImportCount = ImportCount;
			Out.ExpectedPayloadCount = PayloadCount;
			Out.Imports.reserve(static_cast<size_t>(ImportCount));
			Out.PayloadEntries.reserve(static_cast<size_t>(PayloadCount));
			return true;
		}

		auto DecodeImports(std::span<const std::byte> Bytes,
			FParsedPackage& Out, std::string* OutError) -> bool
		{
			FReader Reader(Bytes);
			uint32 VersionValue = 0;
			uint32 Reserved = 0;
			uint64 Count = 0;
			if (!Reader.Fixed(VersionValue) || VersionValue != ImportVersion
				|| !Reader.Fixed(Reserved) || Reserved != 0
				|| !Reader.Fixed(Count) || Count != Out.ExpectedImportCount)
				return Fail("DAST v6 Import header is malformed.", OutError);
			std::string Previous;
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				std::string Import;
				if (!Reader.String(Import, false) || (!Previous.empty() && !(Previous < Import)))
					return Fail("DAST v6 Imports are invalid or noncanonical.", OutError);
				Previous = Import;
				Out.Imports.push_back(std::move(Import));
			}
			return Reader.AtEnd() || Fail("DAST v6 Import section has trailing bytes.", OutError);
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
				PackageTrailer::FEntry Entry;
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
				Entry.Placement = static_cast<PackageTrailer::EPlacement>(Placement);
				if (!Entry.PayloadId.IsValid()
					|| Entry.Placement != PackageTrailer::EPlacement::ExternalDabkV1
					|| Entry.LogicalByteCount != Entry.StoredByteCount
					|| Entry.ContentHash.IsZero() || Entry.ContainerHash.IsZero()
					|| (!Out.PayloadEntries.empty()
						&& !(Out.PayloadEntries.back().PayloadId < Entry.PayloadId)))
					return Fail("DAST v6 Payload Directory entry is invalid or noncanonical.", OutError);
				Out.PayloadEntries.push_back(Entry);
			}
			return Reader.AtEnd() || Fail("DAST v6 Payload Directory has trailing bytes.", OutError);
		}

		auto EncodeV5ObjectStream(
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
			Writer.Fixed(AssetPackageV5FormatVersion);
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

		auto BuildV5Package(const FParsedPackage& Package,
			std::vector<std::byte>& OutBytes) -> FAssetResult
		{
			std::vector<std::byte> ObjectStream;
			if (!EncodeV5ObjectStream(Package, ObjectStream))
				return Error("DAST v6 logical sections cannot form a bounded v5 adapter.");
			std::vector<std::byte> Trailer;
			std::string TrailerError;
			if (!PackageTrailer::Build(
				Package.PayloadEntries, ObjectStream.size(), Trailer, &TrailerError))
				return Error(std::move(TrailerError));
			ObjectStream.insert(ObjectStream.end(), Trailer.begin(), Trailer.end());
			if (FAssetResult Result = DastV5::GetCodec().Validate(ObjectStream); !Result)
				return Result;
			OutBytes = std::move(ObjectStream);
			return {};
		}

		auto BuildV6Package(
			const PackageObjectStream::FValidatedHeader& Header,
			const std::array<std::span<const std::byte>, 5>& V5Sections,
			std::span<const PackageTrailer::FEntry> PayloadEntries,
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
			if (!DecodePublicSummary(Result.RequiredSections[0],
				static_cast<EAssetRegistryEntryKind>(PackageKind), Result, OutError)
				|| !DecodeImports(Result.RequiredSections[1], Result, OutError)
				|| !DecodePayloadDirectory(Result.RequiredSections[7], Result, OutError))
				return false;
			OutPackage = std::move(Result);
			if (OutError) OutError->clear();
			return true;
		}

		auto ParseHeaderWire(
			std::span<const std::byte> Bytes,
			uint64 PackageSize,
			FParsedPackage& OutPackage,
			std::string* OutError) -> bool
		{
			FBinaryEnvelopePreamble Preamble;
			FBinaryEnvelopeDiagnostic EnvelopeDiagnostic;
			if (!ParseBinaryEnvelopePrefix(
				Bytes, PackageSize, EnvelopeLimits, Preamble, &EnvelopeDiagnostic))
				return Fail(std::string(EnvelopeDiagnostic.Message), OutError);
			if (Preamble.HeaderBytes > Bytes.size())
				return Fail("DAST v6 front matter is truncated.", OutError);
			const std::span<const std::byte> Front = Bytes.first(
				static_cast<size_t>(Preamble.HeaderBytes));
			FValidatedBinaryEnvelope Envelope;
			if (!ValidateBinaryEnvelopeHeader(
				Front, PackageSize, EnvelopeLimits, GetRegistry(), Envelope, &EnvelopeDiagnostic))
				return Fail(std::string(EnvelopeDiagnostic.Message), OutError);

			uint32 PackageKind = 0;
			uint32 PackageFlags = 0;
			uint64 DirectoryOffset = 0;
			uint32 SectionCount = 0;
			uint32 EntryBytes = 0;
			uint64 Reserved = 0;
			if (!ReadAt(Front, 64, PackageKind) || PackageKind > 1
				|| !ReadAt(Front, 68, PackageFlags) || PackageFlags != 0
				|| !ReadAt(Front, 72, DirectoryOffset)
				|| !ReadAt(Front, 80, SectionCount)
				|| !ReadAt(Front, 84, EntryBytes) || EntryBytes != SectionEntryBytes
				|| !ReadAt(Front, 88, Reserved) || Reserved != 0
				|| SectionCount < RequiredSectionCount || SectionCount > MaximumSectionCount
				|| DirectoryOffset != BinaryEnvelopePreambleBytes + FormatHeaderBytes)
				return Fail("DAST v6 format header is invalid or unsupported.", OutError);
			const uint64 DirectoryBytes = uint64(SectionCount) * SectionEntryBytes;
			if (DirectoryOffset > Preamble.HeaderBytes
				|| DirectoryBytes > Preamble.HeaderBytes - DirectoryOffset)
				return Fail("DAST v6 section directory exceeds HeaderBytes.", OutError);

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
				if (!ReadAt(Front, EntryOffset, Entry.Kind)
					|| !ReadAt(Front, EntryOffset + 4, Entry.Flags)
					|| !ReadAt(Front, EntryOffset + 8, Entry.Offset)
					|| !ReadAt(Front, EntryOffset + 16, Entry.Size)
					|| !ReadAt(Front, EntryOffset + 24, Entry.Hash.HashLow)
					|| !ReadAt(Front, EntryOffset + 32, Entry.Hash.HashHigh)
					|| !ReadAt(Front, EntryOffset + 40, EntryReserved) || EntryReserved != 0
					|| Entry.Kind <= PreviousKind || (Entry.Flags & ~RequiredSectionFlag) != 0
					|| Entry.Offset != ExpectedOffset || Entry.Offset > PackageSize
					|| Entry.Size > PackageSize - Entry.Offset)
					return Fail("DAST v6 section entry is invalid or noncanonical.", OutError);
				if (Index < RequiredSectionCount)
				{
					if (Entry.Kind != Index + 1 || Entry.Flags != RequiredSectionFlag)
						return Fail("DAST v6 required sections are missing or out of order.", OutError);
					Result.RequiredEntries[Index] = Entry;
					if (Index <= 1)
					{
						if (Entry.Offset > Front.size() || Entry.Size > Front.size() - Entry.Offset)
							return Fail("DAST v6 header section exceeds HeaderBytes.", OutError);
						const std::span<const std::byte> Section = Front.subspan(
							static_cast<size_t>(Entry.Offset), static_cast<size_t>(Entry.Size));
						if (FXxHash128::HashBuffer(Section) != Entry.Hash)
							return Fail("DAST v6 header section hash verification failed.", OutError);
						Result.RequiredSections[Index] = Section;
						if (Index == 1) ImportEnd = Entry.Offset + Entry.Size;
					}
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
			if (ExpectedOffset != PackageSize || ImportEnd != Preamble.HeaderBytes)
				return Fail("DAST v6 sections leave gaps, trailing bytes, or invalid HeaderBytes.", OutError);
			if (!DecodePublicSummary(Result.RequiredSections[0],
				static_cast<EAssetRegistryEntryKind>(PackageKind), Result, OutError)
				|| !DecodeImports(Result.RequiredSections[1], Result, OutError))
				return false;
			OutPackage = std::move(Result);
			if (OutError) OutError->clear();
			return true;
		}

		auto MakeV5(std::span<const std::byte> Bytes,
			std::vector<std::byte>& OutV5, bool bAllowUnknown) -> FAssetResult
		{
			FParsedPackage Parsed;
			std::string ParseError;
			if (!ParseWire(Bytes, Parsed, &ParseError)) return Error(std::move(ParseError));
			if (Parsed.bHasUnknownSkippableSections && !bAllowUnknown)
				return Error("DAST v6 mutation cannot preserve unknown skippable sections.");
			return BuildV5Package(Parsed, OutV5);
		}

		auto ReadHeader(std::span<const std::byte> Bytes, uint64 PackageSize,
			FAssetPackageHeader& OutHeader) -> FAssetResult
		{
			FParsedPackage Parsed;
			std::string ParseError;
			if (!ParseHeaderWire(Bytes, PackageSize, Parsed, &ParseError))
				return Error(std::move(ParseError));
			FAssetPackageHeader Header{
				.AssetClassName = Parsed.AssetClass,
				.EntryKind = Parsed.EntryKind,
				.FormatVersion = Version,
				.ObjectCount = Parsed.ExportCount,
				.BytesRead = Parsed.HeaderBytes};
			if (!Parsed.RedirectDestination.empty()
				&& !FAssetPath::TryCreate(Parsed.RedirectDestination, Header.RedirectDestination))
				return Error("DAST v6 redirect destination path is invalid.");
			for (const std::string& Import : Parsed.Imports)
			{
				FAssetPath Path;
				if (!FAssetPath::TryCreate(Import, Path))
					return Error("DAST v6 Import path is invalid.");
				Header.Dependencies.push_back(std::move(Path));
			}
			OutHeader = std::move(Header);
			return {};
		}

		auto Validate(std::span<const std::byte> Bytes) -> FAssetResult
		{
			std::vector<std::byte> V5;
			return MakeV5(Bytes, V5, true);
		}

		auto Inspect(std::span<const std::byte> Bytes,
			FAssetPackageInspection& OutInspection) -> FAssetResult
		{
			std::vector<std::byte> V5;
			if (FAssetResult Result = MakeV5(Bytes, V5, true); !Result) return Result;
			FAssetPackageInspection Inspection;
			if (FAssetResult Result = DastV5::GetCodec().Inspect(V5, Inspection); !Result) return Result;
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
			std::vector<std::byte> V5;
			if (FAssetResult Result = MakeV5(Bytes, V5, true); !Result) return Result;
			return DastV5::GetCodec().ExtractReferences(V5, Source, Out);
		}

		auto ProbeCompatibility(std::span<const std::byte> Bytes,
			const FAssetPath& Path, const FReflectionCompatibilityCatalog& Catalog,
			FAssetPackageCompatibilityRecord& OutRecord,
			FAssetCompatibilityProbeStats* OutStats) -> FAssetResult
		{
			std::vector<std::byte> V5;
			if (FAssetResult Result = MakeV5(Bytes, V5, true); !Result) return Result;
			FAssetResult Result = DastV5::GetCodec().ProbeCompatibility(
				V5, Path, Catalog, OutRecord, OutStats);
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
			std::vector<std::byte> V5;
			if (FAssetResult Result = MakeV5(Bytes, V5, true); !Result) return Result;
			return DastV5::GetCodec().Load(
				V5, Path, OutPackage, OutReport, OnSkeletonReady, OnSkeletonRollback);
		}

		auto Write(DPackage* Package, std::vector<std::byte>& OutBytes,
			EDefaultDeltaMode DeltaMode,
			const FAssetPackageSerializationOptions& Options) -> FAssetResult
		{
			std::vector<std::byte> V5;
			if (FAssetResult Result = DastV5::GetCodec().Write(Package, V5, DeltaMode, Options); !Result)
				return Result;
			return ConvertV5Package(V5, OutBytes);
		}

		auto RewriteReferences(std::span<const std::byte> Bytes,
			std::span<const FAssetRedirectorFixupMapping> Mappings,
			uint64 ExpectedCount, std::vector<std::byte>& OutBytes) -> FAssetResult
		{
			std::vector<std::byte> V5;
			if (FAssetResult Result = MakeV5(Bytes, V5, false); !Result) return Result;
			std::vector<std::byte> Rewritten;
			if (FAssetResult Result = DastV5::GetCodec().RewriteReferences(
				V5, Mappings, ExpectedCount, Rewritten); !Result) return Result;
			return ConvertV5Package(Rewritten, OutBytes);
		}

		auto Relocate(std::span<const std::byte> Bytes, const FAssetPath& Destination,
			std::vector<std::byte>& OutBytes) -> FAssetResult
		{
			std::vector<std::byte> V5;
			if (FAssetResult Result = MakeV5(Bytes, V5, false); !Result) return Result;
			std::vector<std::byte> Relocated;
			if (FAssetResult Result = DastV5::GetCodec().Relocate(V5, Destination, Relocated); !Result)
				return Result;
			return ConvertV5Package(Relocated, OutBytes);
		}

		auto WriteRedirector(const FAssetPath& Source, const FAssetPath& Destination,
			std::vector<std::byte>& OutBytes) -> FAssetResult
		{
			std::vector<std::byte> V5;
			if (FAssetResult Result = DastV5::GetCodec().WriteRedirector(Source, Destination, V5); !Result)
				return Result;
			return ConvertV5Package(V5, OutBytes);
		}
	}

	auto ParsePackage(std::span<const std::byte> Bytes,
		FParsedPackage& OutPackage, std::string* OutError) -> bool
	{
		FParsedPackage Parsed;
		if (!ParseWire(Bytes, Parsed, OutError)) return false;
		std::vector<std::byte> V5;
		if (FAssetResult Result = BuildV5Package(Parsed, V5); !Result)
			return Fail(Result.Message, OutError);
		OutPackage = std::move(Parsed);
		return true;
	}

	auto ConvertV5Package(std::span<const std::byte> V5Bytes,
		std::vector<std::byte>& OutV6Bytes) -> FAssetResult
	{
		if (FAssetResult Result = DastV5::GetCodec().Validate(V5Bytes); !Result) return Result;
		PackageTrailer::FInspection Trailer;
		std::string TrailerError;
		if (!PackageTrailer::Inspect(V5Bytes, Trailer, &TrailerError))
			return Error(std::move(TrailerError));
		const std::span<const std::byte> ObjectStream = V5Bytes.first(
			static_cast<size_t>(Trailer.ObjectStreamEnd));
		PackageObjectStream::FValidatedHeader Header;
		PackageObjectStream::FReaderDiagnostic Diagnostic;
		if (!PackageObjectStream::ReadHeader(
			ObjectStream, Header, {}, &Diagnostic, ObjectStream.size()))
			return Error(Diagnostic.Message);
		std::array<std::span<const std::byte>, 5> Sections;
		for (size_t Index = 0; Index < Sections.size(); ++Index)
			Sections[Index] = ObjectStream.subspan(
				Header.Sections[Index].Offset, Header.Sections[Index].Length);
		std::vector<std::byte> Candidate;
		if (FAssetResult Result = BuildV6Package(
			Header, Sections, Trailer.Entries, Candidate); !Result) return Result;
		FParsedPackage Parsed;
		std::string ParseError;
		if (!ParsePackage(Candidate, Parsed, &ParseError)) return Error(std::move(ParseError));
		OutV6Bytes = std::move(Candidate);
		return {};
	}

	auto ConvertV6PackageToV5(std::span<const std::byte> V6Bytes,
		std::vector<std::byte>& OutV5Bytes, bool bAllowUnknownSkippableSections) -> FAssetResult
	{
		return MakeV5(V6Bytes, OutV5Bytes, bAllowUnknownSkippableSections);
	}

	auto GetCodec() -> const FAssetPackageCodec&
	{
		static const FAssetPackageCodec Codec{
			.CodecId = "dast-v6-detached",
			.FormatId = DastBinaryFormatId,
			.FormatVersion = Version,
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
