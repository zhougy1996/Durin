#pragma once

#include "AssetRegistry/ObjectStream.h"
#include "AssetRegistry/PackageHeader.h"
#include "Serialization/BinaryEnvelope.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::Testing::DastV7Fixture
{
	inline constexpr uint32 FormatHeaderBytes = 32;
	inline constexpr uint32 SectionEntryBytes = 48;
	inline constexpr uint32 RequiredSectionCount = 8;
	inline constexpr uint32 RequiredSectionFlag = 1;
	inline constexpr FBinaryEnvelopeLimits Limits{
		16ull * 1024ull * 1024ull, 1024ull * 1024ull * 1024ull};

	template<typename T>
	auto WriteAt(std::span<std::byte> Bytes, uint64 Offset, T Value) -> void
	{
		for (size_t Index = 0; Index < sizeof(T); ++Index)
			Bytes[static_cast<size_t>(Offset) + Index]
				= static_cast<std::byte>((Value >> (Index * 8)) & 0xff);
	}

	auto WriteString(FBinaryWriter& Writer, std::string_view Value) -> bool
	{
		if (Value.size() > Asset::PackageObjectStream::MaximumStringBytes) return false;
		Writer.WriteVarUInt(Value.size());
		Writer.WriteBytes(std::as_bytes(std::span(Value)));
		return !Writer.HasError();
	}

	inline auto BuildPackageFromObjectStream(
		std::span<const std::byte> ObjectStream,
		std::vector<std::byte>& OutBytes,
		std::string* OutError = nullptr) -> bool
	{
		using namespace Asset::PackageObjectStream;
		FValidatedHeader Header;
		FReaderDiagnostic Diagnostic;
		if (!ReadHeader(ObjectStream, Header, {}, &Diagnostic, ObjectStream.size()))
		{
			if (OutError) *OutError = Diagnostic.Message;
			return false;
		}
		std::array<std::vector<std::byte>, RequiredSectionCount> Sections;
		FBinaryWriter Summary;
		Summary.WriteU32(2);
		Summary.WriteU32(1);
		Summary.WriteU64(Header.Dependencies.size());
		Summary.WriteU64(Header.ObjectCount);
		Summary.WriteU64(0);
		Summary.WriteU64(0);
		Summary.WriteHash128({});
		Summary.WriteU32(0);
		Summary.WriteU32(0);
		if (!WriteString(Summary, Header.AssetClass)
			|| !WriteString(Summary, Header.RedirectDestination)) return false;
		Sections[0] = Summary.TakeBytes();

		FBinaryWriter Imports;
		Imports.WriteU32(1);
		Imports.WriteU32(0);
		Imports.WriteU64(Header.Dependencies.size());
		for (const std::string& Dependency : Header.Dependencies)
			if (!WriteString(Imports, Dependency)) return false;
		Sections[1] = Imports.TakeBytes();
		for (size_t Index = 0; Index < 5; ++Index)
		{
			const auto& Entry = Header.Sections[Index];
			Sections[Index + 2].assign(
				ObjectStream.begin() + Entry.Offset,
				ObjectStream.begin() + Entry.Offset + Entry.Length);
		}
		FBinaryWriter BulkDirectory;
		BulkDirectory.WriteU32(2);
		BulkDirectory.WriteU32(72);
		BulkDirectory.WriteU64(0);
		Sections[7] = BulkDirectory.TakeBytes();

		const uint64 DirectoryOffset = BinaryEnvelopePreambleBytes + FormatHeaderBytes;
		uint64 Offset = DirectoryOffset + RequiredSectionCount * SectionEntryBytes;
		struct FEntry { uint64 Offset = 0; uint64 Size = 0; FXxHash128 Hash; };
		std::array<FEntry, RequiredSectionCount> Entries;
		for (size_t Index = 0; Index < Sections.size(); ++Index)
		{
			Entries[Index] = {Offset, Sections[Index].size(),
				FXxHash128::HashBuffer(Sections[Index])};
			if (Sections[Index].size() > Limits.MaximumFileBytes - Offset) return false;
			Offset += Sections[Index].size();
		}
		const uint64 HeaderBytes = Entries[1].Offset + Entries[1].Size;
		if (HeaderBytes > Limits.MaximumHeaderBytes || Offset > Limits.MaximumFileBytes)
			return false;
		std::vector<std::byte> Bytes(static_cast<size_t>(Offset));
		const FBinaryEnvelopePreamble Preamble{
			.FormatId = Asset::DastBinaryFormatId,
			.FormatVersion = Asset::AssetPackageV7FormatVersion,
			.RequiredFeatures = 0,
			.HeaderBytes = HeaderBytes,
			.FileBytes = Offset};
		if (!EncodeBinaryEnvelopePreamble(Preamble, Bytes)) return false;
		WriteAt(Bytes, 64, static_cast<uint32>(Header.EntryKind));
		WriteAt(Bytes, 68, uint32{0});
		WriteAt(Bytes, 72, DirectoryOffset);
		WriteAt(Bytes, 80, RequiredSectionCount);
		WriteAt(Bytes, 84, SectionEntryBytes);
		WriteAt(Bytes, 88, uint64{0});
		for (size_t Index = 0; Index < Entries.size(); ++Index)
		{
			const uint64 EntryOffset = DirectoryOffset + Index * SectionEntryBytes;
			WriteAt(Bytes, EntryOffset, static_cast<uint32>(Index + 1));
			WriteAt(Bytes, EntryOffset + 4, RequiredSectionFlag);
			WriteAt(Bytes, EntryOffset + 8, Entries[Index].Offset);
			WriteAt(Bytes, EntryOffset + 16, Entries[Index].Size);
			WriteAt(Bytes, EntryOffset + 24, Entries[Index].Hash.HashLow);
			WriteAt(Bytes, EntryOffset + 32, Entries[Index].Hash.HashHigh);
			WriteAt(Bytes, EntryOffset + 40, uint64{0});
			std::ranges::copy(Sections[Index],
				Bytes.begin() + static_cast<ptrdiff_t>(Entries[Index].Offset));
		}
		if (!FinalizeBinaryEnvelopeHeader(
			std::span(Bytes).first(static_cast<size_t>(HeaderBytes)), Offset, Limits))
			return false;
		OutBytes = std::move(Bytes);
		if (OutError) OutError->clear();
		return true;
	}
}
