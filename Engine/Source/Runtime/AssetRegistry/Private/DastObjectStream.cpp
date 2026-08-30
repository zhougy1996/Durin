#include "AssetRegistry/ObjectStream.h"

#include "Hash/XxHash.h"
#include "Serialization/BinaryEnvelope.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::Asset::PackageObjectStream
{
	namespace
	{
		constexpr uint32 FormatHeaderBytes = 32;
		constexpr uint32 SectionEntryBytes = 48;
		constexpr uint32 DastRequiredSectionCount = 8;
		constexpr uint32 MaximumSectionCount = 64;
		constexpr uint64 DastMaximumHeaderBytes = 16ull * 1024ull * 1024ull;
		constexpr uint64 DastMaximumFileBytes = 1024ull * 1024ull * 1024ull;
		constexpr uint32 RequiredSectionFlag = 1;
		constexpr FBinaryEnvelopeLimits EnvelopeLimits{
			DastMaximumHeaderBytes, DastMaximumFileBytes};

		auto Error(std::string Message) -> FAssetResult
		{
			return {EAssetError::CorruptFile, std::move(Message)};
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
				require(FBinaryFormatRegistry::Create(Descriptors, Result));
				return Result;
			}();
			return Registry;
		}

		class FReader
		{
		public:
			explicit FReader(std::span<const std::byte> InBytes) : Reader(InBytes) {}

			template<typename T>
			auto Fixed(T& Out) -> bool
			{
				return Reader.ReadInteger(Out);
			}

			auto Guid(FGuid& Out) -> bool
			{
				return Reader.ReadGuid(Out);
			}

			auto Hash(FXxHash128& Out) -> bool
			{
				return Reader.ReadHash128(Out);
			}

			auto AtEnd() const -> bool { return Reader.IsAtEnd(); }

		private:
			FBinaryReader Reader;
		};

		class FWriter
		{
		public:
			template<typename T>
			auto Fixed(T Value) -> void
			{
				Writer.WriteInteger(Value);
			}

			auto VarUInt(uint64 Value) -> void
			{
				Writer.WriteVarUInt(Value);
			}

			auto String(std::string_view Value) -> bool
			{
				if (Value.size() > MaximumStringBytes) return false;
				VarUInt(Value.size());
				const auto Raw = std::as_bytes(std::span(Value));
				Writer.WriteBytes(Raw);
				return true;
			}

			auto Append(std::span<const std::byte> Value) -> void
			{
				Writer.WriteBytes(Value);
			}

			auto Take() -> std::vector<std::byte> { return Writer.TakeBytes(); }
			auto Size() const -> size_t { return static_cast<size_t>(Writer.Tell()); }

		private:
			FBinaryWriter Writer;
		};

	}

	auto ExtractDastObjectStream(std::span<const std::byte> PackageBytes,
		std::vector<std::byte>& OutObjectStream) -> FAssetResult
	{
		OutObjectStream.clear();
		FBinaryEnvelopePreamble Preamble;
		FBinaryEnvelopeDiagnostic Diagnostic;
		if (!ParseBinaryEnvelopePrefix(PackageBytes, PackageBytes.size(),
			EnvelopeLimits, Preamble, &Diagnostic))
			return Error(std::string(Diagnostic.Message));
		if (Preamble.HeaderBytes > PackageBytes.size())
			return Error("DAST v7 front matter is truncated.");
		FValidatedBinaryEnvelope Envelope;
		if (!ValidateBinaryEnvelopeHeader(
			PackageBytes.first(static_cast<size_t>(Preamble.HeaderBytes)),
			PackageBytes.size(), EnvelopeLimits, GetRegistry(), Envelope, &Diagnostic))
			return Error(std::string(Diagnostic.Message));

		uint32 PackageKind = 0;
		uint32 PackageFlags = 0;
		uint64 DirectoryOffset = 0;
		uint32 SectionCount = 0;
		uint32 EntryBytes = 0;
		uint64 Reserved = 0;
		if (!ReadLittleEndianAt(PackageBytes, 64, PackageKind) || PackageKind > 1
			|| !ReadLittleEndianAt(PackageBytes, 68, PackageFlags) || PackageFlags != 0
			|| !ReadLittleEndianAt(PackageBytes, 72, DirectoryOffset)
			|| !ReadLittleEndianAt(PackageBytes, 80, SectionCount)
			|| !ReadLittleEndianAt(PackageBytes, 84, EntryBytes)
			|| EntryBytes != SectionEntryBytes
			|| !ReadLittleEndianAt(PackageBytes, 88, Reserved) || Reserved != 0
			|| SectionCount < DastRequiredSectionCount
			|| SectionCount > MaximumSectionCount
			|| DirectoryOffset != BinaryEnvelopePreambleBytes + FormatHeaderBytes)
			return Error("DAST v7 format header is invalid or unsupported.");
		const uint64 DirectoryBytes = uint64(SectionCount) * SectionEntryBytes;
		if (DirectoryOffset > Preamble.HeaderBytes
			|| DirectoryBytes > Preamble.HeaderBytes - DirectoryOffset)
			return Error("DAST v7 section directory exceeds HeaderBytes.");

		std::array<std::span<const std::byte>, DastRequiredSectionCount> Sections;
		uint64 ExpectedOffset = DirectoryOffset + DirectoryBytes;
		uint64 ImportEnd = 0;
		uint32 PreviousKind = 0;
		for (uint32 Index = 0; Index < SectionCount; ++Index)
		{
			const uint64 At = DirectoryOffset + uint64(Index) * SectionEntryBytes;
			uint32 Kind = 0;
			uint32 Flags = 0;
			uint64 Offset = 0;
			uint64 Size = 0;
			FXxHash128 Hash;
			uint64 EntryReserved = 0;
			if (!ReadLittleEndianAt(PackageBytes, At, Kind)
				|| !ReadLittleEndianAt(PackageBytes, At + 4, Flags)
				|| !ReadLittleEndianAt(PackageBytes, At + 8, Offset)
				|| !ReadLittleEndianAt(PackageBytes, At + 16, Size)
				|| !ReadLittleEndianAt(PackageBytes, At + 24, Hash.HashLow)
				|| !ReadLittleEndianAt(PackageBytes, At + 32, Hash.HashHigh)
				|| !ReadLittleEndianAt(PackageBytes, At + 40, EntryReserved)
				|| EntryReserved != 0 || Kind <= PreviousKind
				|| (Flags & ~RequiredSectionFlag) != 0 || Offset != ExpectedOffset
				|| Offset > PackageBytes.size() || Size > PackageBytes.size() - Offset)
				return Error("DAST v7 section entry is invalid or noncanonical.");
			const auto Section = PackageBytes.subspan(
				static_cast<size_t>(Offset), static_cast<size_t>(Size));
			if (FXxHash128::HashBuffer(Section) != Hash)
				return Error("DAST v7 section hash verification failed.");
			if (Index < DastRequiredSectionCount)
			{
				if (Kind != Index + 1 || Flags != RequiredSectionFlag)
					return Error("DAST v7 required sections are missing or out of order.");
				Sections[Index] = Section;
				if (Index == 1) ImportEnd = Offset + Size;
			}
			else if ((Flags & RequiredSectionFlag) != 0)
				return Error("DAST v7 contains an unknown required section.");
			ExpectedOffset += Size;
			PreviousKind = Kind;
		}
		if (ExpectedOffset != PackageBytes.size() || ImportEnd != Preamble.HeaderBytes)
			return Error("DAST v7 sections leave gaps, trailing bytes, or invalid HeaderBytes.");

		Dast::FPublicSummary Summary;
		std::string ParseError;
		if (!Dast::DecodePublicSummary(Sections[0], Sections[1],
			static_cast<EAssetRegistryEntryKind>(PackageKind), Summary, &ParseError))
			return Error(std::move(ParseError));
		FWriter SummaryWriter;
		if (!SummaryWriter.String(Summary.AssetClass))
			return Error("DAST v7 logical summary exceeds its bound.");
		SummaryWriter.Fixed(static_cast<uint8>(Summary.EntryKind));
		if (!SummaryWriter.String(Summary.RedirectDestination))
			return Error("DAST v7 logical redirect exceeds its bound.");
		SummaryWriter.VarUInt(Summary.Imports.size());
		for (const std::string& Import : Summary.Imports)
			if (!SummaryWriter.String(Import))
				return Error("DAST v7 logical import exceeds its bound.");
		SummaryWriter.VarUInt(Summary.ExportCount);
		if (SummaryWriter.Size() > MaximumSummaryBytes)
			return Error("DAST v7 logical summary exceeds its bound.");

		const std::array LogicalSections{
			Sections[2], Sections[3], Sections[4], Sections[5], Sections[6]};
		uint64 Total = 13 + SummaryWriter.Size() + LogicalSections.size() * 9;
		for (const auto Section : LogicalSections) Total += Section.size();
		if (Total > MaximumPackageBytes || Total > std::numeric_limits<uint32>::max())
			return Error("DAST v7 logical object stream exceeds its bound.");
		const uint32 LogicalSummaryBytes = static_cast<uint32>(SummaryWriter.Size());
		FWriter Writer;
		Writer.Fixed(Magic);
		Writer.Fixed(Version);
		Writer.Fixed(LogicalSummaryBytes);
		Writer.Fixed(static_cast<uint8>(LogicalSections.size()));
		Writer.Append(SummaryWriter.Take());
		uint32 Offset = static_cast<uint32>(13 + LogicalSummaryBytes
			+ LogicalSections.size() * 9);
		for (size_t Index = 0; Index < LogicalSections.size(); ++Index)
		{
			Writer.Fixed(static_cast<uint8>(Index + 1));
			Writer.Fixed(Offset);
			Writer.Fixed(static_cast<uint32>(LogicalSections[Index].size()));
			Offset += static_cast<uint32>(LogicalSections[Index].size());
		}
		for (const auto Section : LogicalSections) Writer.Append(Section);
		OutObjectStream = Writer.Take();
		return {};
	}

	auto ExtractAssetPackageReferences(std::span<const std::byte> PackageBytes,
		const FAssetPath& SourcePackage,
		std::vector<FAssetReferenceEdge>& OutReferences,
		FAssetPackageFingerprint* OutFingerprint) -> FAssetResult
	{
		std::vector<std::byte> ObjectStream;
		if (FAssetResult Result = ExtractDastObjectStream(PackageBytes, ObjectStream); !Result)
			return Result;
		FBinaryEnvelopePreamble PackagePreamble;
		FBinaryEnvelopeDiagnostic PackageDiagnostic;
		if (!ParseBinaryEnvelopePrefix(PackageBytes, PackageBytes.size(), EnvelopeLimits,
				PackagePreamble, &PackageDiagnostic))
			return Error(std::string(PackageDiagnostic.Message));
		FReaderDiagnostic Diagnostic;
		if (FAssetResult Result = ExtractReferences(
			ObjectStream, SourcePackage, OutReferences, {}, &Diagnostic); !Result)
			return Result;
		std::erase_if(OutReferences, [&](const FAssetReferenceEdge& Reference) {
			return Reference.Kind == EAssetReferenceKind::HardObject
				&& Reference.TargetPath == SourcePackage;
		});
		const FAssetPackageFingerprint Fingerprint{
			.FileSize = PackageBytes.size(),
			.ContentHash = FXxHash128::HashBuffer(PackageBytes),
			.ReaderVersion = PackagePreamble.FormatVersion};
		for (FAssetReferenceEdge& Reference : OutReferences)
			Reference.SourceFingerprint = Fingerprint;
		if (OutFingerprint)
			*OutFingerprint = Fingerprint;
		return {};
	}
}
