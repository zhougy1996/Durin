#include "AssetRegistry/PackageHeader.h"

#include "Hash/XxHash.h"
#include "Serialization/BinaryEnvelope.h"
#include "Serialization/BinaryFormat.h"

#include <fstream>

namespace Durin::Asset
{
	namespace
	{
		constexpr uint32 PublicSummaryVersion = 1;
		constexpr uint32 ImportVersion = 1;
		constexpr uint32 FormatHeaderBytes = 32;
		constexpr uint32 SectionEntryBytes = 48;
		constexpr uint32 RequiredSectionCount = 8;
		constexpr uint32 MaximumSectionCount = 64;
		constexpr uint64 MaximumHeaderBytes = 16ull * 1024ull * 1024ull;
		constexpr uint64 MaximumFileBytes = 1024ull * 1024ull * 1024ull;
		constexpr uint64 MaximumImportCount = 65'536;
		constexpr uint64 MaximumExportCount = 1'048'576;
		constexpr uint64 MaximumPayloadCount = 65'536;
		constexpr uint64 MaximumStringBytes = 1024ull * 1024ull;
		constexpr uint32 RequiredSectionFlag = 1;
		constexpr FBinaryEnvelopeLimits EnvelopeLimits{MaximumHeaderBytes, MaximumFileBytes};

		auto Error(EAssetError Code, std::string Message) -> FAssetResult
		{
			return {Code, std::move(Message)};
		}

		auto Fail(std::string Message, std::string* OutError) -> bool
		{
			if (OutError) *OutError = std::move(Message);
			return false;
		}

		auto EnvelopeError(const FBinaryEnvelopeDiagnostic& Diagnostic) -> FAssetResult
		{
			const bool bUnsupported =
				Diagnostic.Error == EBinaryEnvelopeError::UnknownFormat
				|| Diagnostic.Error == EBinaryEnvelopeError::UnsupportedFormatVersion
				|| Diagnostic.Error == EBinaryEnvelopeError::UnsupportedRequiredFeatures;
			return Error(bUnsupported ? EAssetError::UnsupportedVersion
				: EAssetError::CorruptFile, std::string(Diagnostic.Message));
		}

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
				if (!VarUInt(Size) || Size > MaximumStringBytes || Size > Remaining()
					|| (!bAllowEmpty && Size == 0)) return false;
				std::string Value(reinterpret_cast<const char*>(Bytes.data() + Offset),
					static_cast<size_t>(Size));
				Offset += static_cast<size_t>(Size);
				if (Value.find('\0') != std::string::npos) return false;
				OutValue = std::move(Value);
				return true;
			}

			auto Remaining() const -> size_t { return Bytes.size() - Offset; }
			auto AtEnd() const -> bool { return Offset == Bytes.size(); }

		private:
			std::span<const std::byte> Bytes;
			size_t Offset = 0;
		};

		auto GetRegistry() -> const FBinaryFormatRegistry&
		{
			static const FBinaryFormatRegistry Registry = [] {
				const std::array Descriptors{FBinaryFormatDescriptor{
					.FormatId = DastBinaryFormatId,
					.DebugName = std::string(DastBinaryFormatName),
					.MinimumFormatVersion = AssetPackageV6FormatVersion,
					.MaximumFormatVersion = AssetPackageV6FormatVersion,
					.SupportedRequiredFeatures = 0,
					.Limits = EnvelopeLimits}};
				FBinaryFormatRegistry Result;
				const bool bCreated = FBinaryFormatRegistry::Create(Descriptors, Result);
				require(bCreated);
				return Result;
			}();
			return Registry;
		}
	}

	namespace Dast
	{
		auto DecodePublicSummary(std::span<const std::byte> PublicSummaryBytes,
			std::span<const std::byte> ImportBytes, EAssetRegistryEntryKind EntryKind,
			FPublicSummary& OutSummary, std::string* OutError) -> bool
		{
			FPublicSummary Result;
			FReader Summary(PublicSummaryBytes);
			uint32 SummaryVersion = 0;
			uint64 ImportCount = 0;
			uint64 Reserved = 0;
			if (!Summary.Fixed(SummaryVersion) || SummaryVersion != PublicSummaryVersion
				|| !Summary.Fixed(Result.MainExportIndex) || !Summary.Fixed(ImportCount)
				|| !Summary.Fixed(Result.ExportCount) || !Summary.Fixed(Result.PayloadCount)
				|| !Summary.Fixed(Reserved) || Reserved != 0
				|| !Summary.String(Result.AssetClass, false)
				|| !Summary.String(Result.RedirectDestination) || !Summary.AtEnd())
				return Fail("DAST v6 Public Summary is malformed.", OutError);
			if (Result.MainExportIndex != 1 || Result.ExportCount == 0
				|| Result.ExportCount > MaximumExportCount || ImportCount > MaximumImportCount
				|| Result.PayloadCount > MaximumPayloadCount
				|| (EntryKind == EAssetRegistryEntryKind::Asset)
					!= Result.RedirectDestination.empty())
				return Fail("DAST v6 Public Summary values are invalid.", OutError);
			Result.EntryKind = EntryKind;
			Result.Imports.reserve(static_cast<size_t>(ImportCount));

			FReader Imports(ImportBytes);
			uint32 ImportsVersion = 0;
			uint32 ImportsReserved = 0;
			uint64 Count = 0;
			if (!Imports.Fixed(ImportsVersion) || ImportsVersion != ImportVersion
				|| !Imports.Fixed(ImportsReserved) || ImportsReserved != 0
				|| !Imports.Fixed(Count) || Count != ImportCount)
				return Fail("DAST v6 Import header is malformed.", OutError);
			std::string Previous;
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				std::string Import;
				if (!Imports.String(Import, false) || (!Previous.empty() && !(Previous < Import)))
					return Fail("DAST v6 Imports are invalid or noncanonical.", OutError);
				Previous = Import;
				Result.Imports.push_back(std::move(Import));
			}
			if (!Imports.AtEnd())
				return Fail("DAST v6 Import section has trailing bytes.", OutError);
			OutSummary = std::move(Result);
			if (OutError) OutError->clear();
			return true;
		}
	}

	auto ReadAssetPackageHeaderBytes(std::span<const std::byte> FrontMatter,
		uint64 PhysicalFileBytes, FAssetPackageHeader& OutHeader) -> FAssetResult
	{
		OutHeader = {};
		FBinaryEnvelopePreamble Preamble;
		FBinaryEnvelopeDiagnostic Diagnostic;
		if (!ParseBinaryEnvelopePrefix(FrontMatter, PhysicalFileBytes, EnvelopeLimits,
			Preamble, &Diagnostic))
			return EnvelopeError(Diagnostic);
		if (Preamble.HeaderBytes > FrontMatter.size())
			return Error(EAssetError::CorruptFile, "DAST v6 front matter is truncated.");
		const auto Front = FrontMatter.first(static_cast<size_t>(Preamble.HeaderBytes));
		FValidatedBinaryEnvelope Envelope;
		if (!ValidateBinaryEnvelopeHeader(Front, PhysicalFileBytes, EnvelopeLimits,
			GetRegistry(), Envelope, &Diagnostic))
			return EnvelopeError(Diagnostic);

		uint32 PackageKind = 0, PackageFlags = 0, SectionCount = 0, EntryBytes = 0;
		uint64 DirectoryOffset = 0, Reserved = 0;
		if (!ReadLittleEndianAt(Front, 64, PackageKind) || PackageKind > 1
			|| !ReadLittleEndianAt(Front, 68, PackageFlags) || PackageFlags != 0
			|| !ReadLittleEndianAt(Front, 72, DirectoryOffset)
			|| !ReadLittleEndianAt(Front, 80, SectionCount)
			|| !ReadLittleEndianAt(Front, 84, EntryBytes) || EntryBytes != SectionEntryBytes
			|| !ReadLittleEndianAt(Front, 88, Reserved) || Reserved != 0
			|| SectionCount < RequiredSectionCount || SectionCount > MaximumSectionCount
			|| DirectoryOffset != BinaryEnvelopePreambleBytes + FormatHeaderBytes)
			return Error(EAssetError::CorruptFile,
				"DAST v6 format header is invalid or unsupported.");
		const uint64 DirectoryBytes = uint64(SectionCount) * SectionEntryBytes;
		if (DirectoryOffset > Preamble.HeaderBytes
			|| DirectoryBytes > Preamble.HeaderBytes - DirectoryOffset)
			return Error(EAssetError::CorruptFile,
				"DAST v6 section directory exceeds HeaderBytes.");

		uint64 ExpectedOffset = DirectoryOffset + DirectoryBytes;
		uint64 ImportEnd = 0;
		uint32 PreviousKind = 0;
		std::array<std::span<const std::byte>, 2> HeaderSections;
		for (uint32 Index = 0; Index < SectionCount; ++Index)
		{
			const uint64 At = DirectoryOffset + uint64(Index) * SectionEntryBytes;
			uint32 Kind = 0, Flags = 0;
			uint64 Offset = 0, Size = 0, HashLow = 0, HashHigh = 0, EntryReserved = 0;
			if (!ReadLittleEndianAt(Front, At, Kind)
				|| !ReadLittleEndianAt(Front, At + 4, Flags)
				|| !ReadLittleEndianAt(Front, At + 8, Offset)
				|| !ReadLittleEndianAt(Front, At + 16, Size)
				|| !ReadLittleEndianAt(Front, At + 24, HashLow)
				|| !ReadLittleEndianAt(Front, At + 32, HashHigh)
				|| !ReadLittleEndianAt(Front, At + 40, EntryReserved) || EntryReserved != 0
				|| Kind <= PreviousKind || (Flags & ~RequiredSectionFlag) != 0
				|| Offset != ExpectedOffset || Offset > PhysicalFileBytes
				|| Size > PhysicalFileBytes - Offset)
				return Error(EAssetError::CorruptFile,
					"DAST v6 section entry is invalid or noncanonical.");
			if (Index < RequiredSectionCount)
			{
				if (Kind != Index + 1 || Flags != RequiredSectionFlag)
					return Error(EAssetError::CorruptFile,
						"DAST v6 required sections are missing or out of order.");
				if (Index <= 1)
				{
					if (Offset > Front.size() || Size > Front.size() - Offset)
						return Error(EAssetError::CorruptFile,
							"DAST v6 header section exceeds HeaderBytes.");
					HeaderSections[Index] = Front.subspan(
						static_cast<size_t>(Offset), static_cast<size_t>(Size));
					if (FXxHash128::HashBuffer(HeaderSections[Index]) != FXxHash128{HashLow, HashHigh})
						return Error(EAssetError::CorruptFile,
							"DAST v6 header section hash verification failed.");
					if (Index == 1) ImportEnd = Offset + Size;
				}
			}
			else if ((Flags & RequiredSectionFlag) != 0)
				return Error(EAssetError::CorruptFile,
					"DAST v6 contains an unknown required section.");
			ExpectedOffset += Size;
			PreviousKind = Kind;
		}
		if (ExpectedOffset != PhysicalFileBytes || ImportEnd != Preamble.HeaderBytes)
			return Error(EAssetError::CorruptFile,
				"DAST v6 sections leave gaps, trailing bytes, or invalid HeaderBytes.");

		Dast::FPublicSummary Summary;
		std::string ParseError;
		if (!Dast::DecodePublicSummary(HeaderSections[0], HeaderSections[1],
			static_cast<EAssetRegistryEntryKind>(PackageKind), Summary, &ParseError))
			return Error(EAssetError::CorruptFile, std::move(ParseError));
		FAssetPackageHeader Header{
			.AssetClassName = std::move(Summary.AssetClass),
			.EntryKind = Summary.EntryKind,
			.FormatVersion = AssetPackageV6FormatVersion,
			.ObjectCount = Summary.ExportCount,
			.BytesRead = Preamble.HeaderBytes};
		if (!Summary.RedirectDestination.empty()
			&& !FAssetPath::TryCreate(Summary.RedirectDestination, Header.RedirectDestination))
			return Error(EAssetError::CorruptFile,
				"DAST v6 redirect destination path is invalid.");
		for (const std::string& Import : Summary.Imports)
		{
			FAssetPath Path;
			if (!FAssetPath::TryCreate(Import, Path))
				return Error(EAssetError::CorruptFile, "DAST v6 Import path is invalid.");
			Header.Dependencies.push_back(std::move(Path));
		}
		OutHeader = std::move(Header);
		return {};
	}

	auto ReadAssetPackageHeader(std::string_view PhysicalPath,
		FAssetPackageHeader& OutHeader) -> FAssetResult
	{
		OutHeader = {};
		std::ifstream Stream(std::filesystem::path(PhysicalPath), std::ios::binary | std::ios::ate);
		if (!Stream) return Error(EAssetError::IoError,
			std::format("Failed to open asset package {}.", PhysicalPath));
		const auto End = Stream.tellg();
		if (End < 0) return Error(EAssetError::IoError,
			std::format("Failed to size asset package {}.", PhysicalPath));
		const uint64 FileSize = static_cast<uint64>(End);
		if (FileSize > MaximumFileBytes)
			return Error(EAssetError::CorruptFile,
				"Asset package exceeds the supported byte bound.");
		Stream.seekg(0);
		const uint64 InitialSize = std::min<uint64>(FileSize, BinaryEnvelopePreambleBytes);
		std::vector<std::byte> Bytes(static_cast<size_t>(InitialSize));
		if (InitialSize != 0)
		{
			Stream.read(reinterpret_cast<char*>(Bytes.data()), static_cast<std::streamsize>(InitialSize));
			if (!Stream) return Error(EAssetError::IoError,
				std::format("Failed to read asset package {}.", PhysicalPath));
		}
		uint32 Magic = 0;
		if (Bytes.size() >= sizeof(Magic))
			std::memcpy(&Magic, Bytes.data(), sizeof(Magic));
		if (Magic == DastPackageMagic)
		{
			if (Bytes.size() < sizeof(uint32) * 2)
				return Error(EAssetError::CorruptFile, "Truncated asset header.");
			uint32 LegacyVersion = 0;
			std::memcpy(&LegacyVersion, Bytes.data() + sizeof(Magic), sizeof(LegacyVersion));
			return Error(EAssetError::UnsupportedVersion,
				std::format("Unsupported legacy DAST prefix version {}.", LegacyVersion));
		}
		uint64 HeaderBytes = InitialSize;
		if (Bytes.size() >= BinaryEnvelopePreambleBytes)
		{
			uint64 Declared = 0;
			if (!ReadLittleEndianAt(Bytes, 32, Declared) || Declared < BinaryEnvelopePreambleBytes
				|| Declared > MaximumHeaderBytes || Declared > FileSize)
				return Error(EAssetError::CorruptFile,
					"Asset package declares an invalid front-matter extent.");
			HeaderBytes = Declared;
			if (Declared > Bytes.size())
			{
				const size_t Previous = Bytes.size();
				Bytes.resize(static_cast<size_t>(Declared));
				Stream.read(reinterpret_cast<char*>(Bytes.data() + Previous),
					static_cast<std::streamsize>(Declared - Previous));
				if (!Stream) return Error(EAssetError::IoError,
					std::format("Failed to read asset package {}.", PhysicalPath));
			}
		}
		FAssetResult Result = ReadAssetPackageHeaderBytes(Bytes, FileSize, OutHeader);
		if (Result) OutHeader.FileBytesRead = HeaderBytes;
		return Result;
	}
}
