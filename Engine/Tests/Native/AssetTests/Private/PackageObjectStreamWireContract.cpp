#include "PackageObjectStreamWireContract.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace Durin::Testing::PackageObjectStream
{
	namespace
	{
		auto Fail(std::string& OutError, std::string_view Message) -> bool
		{
			OutError = Message;
			return false;
		}

		template<typename T>
		auto WriteLittleEndian(Durin::FByteArray& Bytes, T Value) -> void
		{
			for (uint32 Index = 0; Index < sizeof(T); ++Index)
				Bytes.push_back(static_cast<std::byte>(Value >> (Index * 8)));
		}

		template<typename T>
		auto ReadLittleEndian(
			std::span<const std::byte> Bytes,
			uint64& Offset,
			T& OutValue,
			std::string& OutError) -> bool
		{
			if (Bytes.size() - Offset < sizeof(T))
				return Fail(OutError, "truncated fixed-width value");
			T Value = 0;
			for (uint32 Index = 0; Index < sizeof(T); ++Index)
				Value |= std::to_integer<T>(Bytes[Offset++]) << (Index * 8);
			OutValue = Value;
			return true;
		}

		auto IsCanonicalDependencyList(const std::vector<std::string>& Dependencies) -> bool
		{
			return std::adjacent_find(Dependencies.begin(), Dependencies.end(),
				[](const std::string& Left, const std::string& Right)
				{
					return !std::lexicographical_compare(
						Left.begin(), Left.end(), Right.begin(), Right.end(),
						[](char A, char B) { return uint8(A) < uint8(B); });
				}) == Dependencies.end();
		}

		auto ValidateSummary(const FPublicSummary& Summary, std::string& OutError) -> bool
		{
			if (Summary.AssetClass.empty())
				return Fail(OutError, "asset class must be nonempty");
			if (Summary.EntryKind > 1)
				return Fail(OutError, "invalid entry kind");
			if ((Summary.EntryKind == 0) != Summary.RedirectDestination.empty())
				return Fail(OutError, "redirect destination does not match entry kind");
			if (Summary.Dependencies.size() > MaximumDependencies)
				return Fail(OutError, "dependency count exceeds bound");
			if (!IsCanonicalDependencyList(Summary.Dependencies))
				return Fail(OutError, "dependencies are not sorted and unique");
			for (const std::string& Dependency : Summary.Dependencies)
				if (Dependency.empty())
					return Fail(OutError, "dependency must be nonempty");
			if (Summary.ObjectCount > MaximumObjects)
				return Fail(OutError, "object count exceeds bound");
			return true;
		}
	}

	auto IsValidUtf8(std::string_view Value) -> bool
	{
		for (uint64 Index = 0; Index < Value.size();)
		{
			const uint8 Lead = uint8(Value[Index++]);
			if (Lead == 0)
				return false;
			if (Lead <= 0x7f)
				continue;
			uint32 CodePoint = 0;
			uint32 Continuations = 0;
			uint32 Minimum = 0;
			if (Lead >= 0xc2 && Lead <= 0xdf)
			{
				CodePoint = Lead & 0x1f;
				Continuations = 1;
				Minimum = 0x80;
			}
			else if (Lead >= 0xe0 && Lead <= 0xef)
			{
				CodePoint = Lead & 0x0f;
				Continuations = 2;
				Minimum = 0x800;
			}
			else if (Lead >= 0xf0 && Lead <= 0xf4)
			{
				CodePoint = Lead & 0x07;
				Continuations = 3;
				Minimum = 0x10000;
			}
			else
				return false;

			if (Value.size() - Index < Continuations)
				return false;
			for (uint32 Continuation = 0; Continuation < Continuations; ++Continuation)
			{
				const uint8 Byte = uint8(Value[Index++]);
				if ((Byte & 0xc0) != 0x80)
					return false;
				CodePoint = (CodePoint << 6) | (Byte & 0x3f);
			}
			if (CodePoint < Minimum || CodePoint > 0x10ffff
				|| (CodePoint >= 0xd800 && CodePoint <= 0xdfff))
				return false;
		}
		return true;
	}

	auto FWireWriter::WriteU8(uint8 Value) -> void { Data.push_back(static_cast<std::byte>(Value)); }
	auto FWireWriter::WriteU16(uint16 Value) -> void { WriteLittleEndian(Data, Value); }
	auto FWireWriter::WriteU32(uint32 Value) -> void { WriteLittleEndian(Data, Value); }
	auto FWireWriter::WriteU64(uint64 Value) -> void { WriteLittleEndian(Data, Value); }
	auto FWireWriter::WriteF32(float Value) -> void { WriteU32(std::bit_cast<uint32>(Value)); }
	auto FWireWriter::WriteF64(double Value) -> void { WriteU64(std::bit_cast<uint64>(Value)); }

	auto FWireWriter::WriteVarUInt(uint64 Value) -> void
	{
		do
		{
			uint8 Byte = Value & 0x7f;
			Value >>= 7;
			if (Value != 0)
				Byte |= 0x80;
			Data.push_back(static_cast<std::byte>(Byte));
		} while (Value != 0);
	}

	auto FWireWriter::WriteVarInt(int64 Value) -> void
	{
		const uint64 Encoded = Value >= 0
			? uint64(Value) * 2
			: uint64(-(Value + 1)) * 2 + 1;
		WriteVarUInt(Encoded);
	}

	auto FWireWriter::WriteString(std::string_view Value, std::string& OutError) -> bool
	{
		if (Value.size() > MaximumStringBytes)
			return Fail(OutError, "string exceeds bound");
		if (!IsValidUtf8(Value))
			return Fail(OutError, "invalid UTF-8");
		WriteVarUInt(Value.size());
		WriteBytes(std::as_bytes(std::span{Value}));
		return true;
	}

	auto FWireWriter::WriteBytes(std::span<const std::byte> Value) -> void
	{
		Data.insert(Data.end(), Value.begin(), Value.end());
	}

	auto FWireReader::ReadU8(uint8& OutValue, std::string& OutError) -> bool
	{
		if (Remaining() == 0)
			return Fail(OutError, "truncated byte");
		OutValue = std::to_integer<uint8>(Bytes[Offset++]);
		return true;
	}

	auto FWireReader::ReadU16(uint16& OutValue, std::string& OutError) -> bool
	{
		return ReadLittleEndian(Bytes, Offset, OutValue, OutError);
	}

	auto FWireReader::ReadU32(uint32& OutValue, std::string& OutError) -> bool
	{
		return ReadLittleEndian(Bytes, Offset, OutValue, OutError);
	}

	auto FWireReader::ReadU64(uint64& OutValue, std::string& OutError) -> bool
	{
		return ReadLittleEndian(Bytes, Offset, OutValue, OutError);
	}

	auto FWireReader::ReadF32(float& OutValue, std::string& OutError) -> bool
	{
		uint32 Bits = 0;
		if (!ReadU32(Bits, OutError))
			return false;
		OutValue = std::bit_cast<float>(Bits);
		return true;
	}

	auto FWireReader::ReadF64(double& OutValue, std::string& OutError) -> bool
	{
		uint64 Bits = 0;
		if (!ReadU64(Bits, OutError))
			return false;
		OutValue = std::bit_cast<double>(Bits);
		return true;
	}

	auto FWireReader::ReadVarUInt(uint64& OutValue, std::string& OutError) -> bool
	{
		uint64 Value = 0;
		for (uint32 Index = 0; Index < 10; ++Index)
		{
			uint8 Byte = 0;
			if (!ReadU8(Byte, OutError))
				return false;
			if (Index == 9 && (Byte & 0xfe) != 0)
				return Fail(OutError, "VarUInt overflow");
			Value |= uint64(Byte & 0x7f) << (Index * 7);
			if ((Byte & 0x80) == 0)
			{
				if (Index != 0 && Byte == 0)
					return Fail(OutError, "nonminimal VarUInt");
				OutValue = Value;
				return true;
			}
		}
		return Fail(OutError, "VarUInt exceeds ten bytes");
	}

	auto FWireReader::ReadVarInt(int64& OutValue, std::string& OutError) -> bool
	{
		uint64 Encoded = 0;
		if (!ReadVarUInt(Encoded, OutError))
			return false;
		if ((Encoded & 1) == 0)
			OutValue = int64(Encoded >> 1);
		else if (Encoded == std::numeric_limits<uint64>::max())
			OutValue = std::numeric_limits<int64>::min();
		else
			OutValue = -int64((Encoded >> 1) + 1);
		return true;
	}

	auto FWireReader::ReadString(std::string& OutValue, std::string& OutError) -> bool
	{
		uint64 Length = 0;
		if (!ReadVarUInt(Length, OutError))
			return false;
		if (Length > MaximumStringBytes)
			return Fail(OutError, "string exceeds bound");
		std::span<const std::byte> Value;
		if (!ReadBytes(Length, Value, OutError))
			return false;
		std::string Result(reinterpret_cast<const char*>(Value.data()), Value.size());
		if (!IsValidUtf8(Result))
			return Fail(OutError, "invalid UTF-8");
		OutValue = std::move(Result);
		return true;
	}

	auto FWireReader::ReadBytes(
		uint64 Count,
		std::span<const std::byte>& OutValue,
		std::string& OutError) -> bool
	{
		if (Count > Remaining())
			return Fail(OutError, "truncated byte range");
		OutValue = Bytes.subspan(Offset, Count);
		Offset += Count;
		return true;
	}

	auto FWireReader::RequireEnd(std::string& OutError) const -> bool
	{
		return Remaining() == 0 || Fail(OutError, "unconsumed bytes");
	}

	auto EncodePublicSummary(
		const FPublicSummary& Summary,
		Durin::FByteArray& OutBytes,
		std::string& OutError) -> bool
	{
		if (!ValidateSummary(Summary, OutError))
			return false;
		FWireWriter Writer;
		if (!Writer.WriteString(Summary.AssetClass, OutError))
			return false;
		Writer.WriteU8(Summary.EntryKind);
		if (!Writer.WriteString(Summary.RedirectDestination, OutError))
			return false;
		Writer.WriteVarUInt(Summary.Dependencies.size());
		for (const std::string& Dependency : Summary.Dependencies)
			if (!Writer.WriteString(Dependency, OutError))
				return false;
		Writer.WriteVarUInt(Summary.ObjectCount);
		if (Writer.Bytes().size() > MaximumSummaryBytes)
			return Fail(OutError, "public summary exceeds bound");
		OutBytes = Writer.TakeBytes();
		return true;
	}

	auto EncodeEnvelope(
		const FPublicSummary& Summary,
		const std::array<Durin::FByteArray, SectionCount>& Sections,
		Durin::FByteArray& OutBytes,
		std::string& OutError) -> bool
	{
		Durin::FByteArray SummaryBytes;
		if (!EncodePublicSummary(Summary, SummaryBytes, OutError))
			return false;
		uint64 Total = 13 + SummaryBytes.size() + SectionCount * 9;
		for (const Durin::FByteArray& Section : Sections)
		{
			if (Section.size() > std::numeric_limits<uint32>::max() - Total)
				return Fail(OutError, "section extent overflows uint32");
			Total += Section.size();
		}
		if (Total > MaximumPackageBytes)
			return Fail(OutError, "package exceeds bound");

		FWireWriter Writer;
		Writer.WriteU32(Magic);
		Writer.WriteU32(Version);
		Writer.WriteU32(uint32(SummaryBytes.size()));
		Writer.WriteU8(SectionCount);
		Writer.WriteBytes(SummaryBytes);
		uint32 Offset = uint32(13 + SummaryBytes.size() + SectionCount * 9);
		for (uint8 Index = 0; Index < SectionCount; ++Index)
		{
			Writer.WriteU8(Index + 1);
			Writer.WriteU32(Offset);
			Writer.WriteU32(uint32(Sections[Index].size()));
			Offset += uint32(Sections[Index].size());
		}
		for (const Durin::FByteArray& Section : Sections)
			Writer.WriteBytes(Section);
		OutBytes = Writer.TakeBytes();
		return true;
	}

	auto DecodeHeader(
		std::span<const std::byte> Bytes,
		FValidatedHeader& OutHeader,
		std::string& OutError) -> bool
	{
		if (Bytes.size() > MaximumPackageBytes)
			return Fail(OutError, "package exceeds bound");
		FWireReader Reader(Bytes);
		uint32 ReadMagic = 0;
		uint32 ReadVersion = 0;
		uint32 SummaryLength = 0;
		uint8 ReadSectionCount = 0;
		if (!Reader.ReadU32(ReadMagic, OutError) || !Reader.ReadU32(ReadVersion, OutError)
			|| !Reader.ReadU32(SummaryLength, OutError) || !Reader.ReadU8(ReadSectionCount, OutError))
			return false;
		if (ReadMagic != Magic)
			return Fail(OutError, "invalid DAST magic");
		if (ReadVersion != Version)
			return Fail(OutError, "unsupported DAST version");
		if (SummaryLength > MaximumSummaryBytes)
			return Fail(OutError, "public summary exceeds bound");
		if (ReadSectionCount != SectionCount)
			return Fail(OutError, "v4 requires exactly five sections");
		std::span<const std::byte> SummaryBytes;
		if (!Reader.ReadBytes(SummaryLength, SummaryBytes, OutError))
			return false;

		FValidatedHeader Result;
		FWireReader SummaryReader(SummaryBytes);
		if (!SummaryReader.ReadString(Result.Summary.AssetClass, OutError)
			|| !SummaryReader.ReadU8(Result.Summary.EntryKind, OutError)
			|| !SummaryReader.ReadString(Result.Summary.RedirectDestination, OutError))
			return false;
		uint64 DependencyCount = 0;
		if (!SummaryReader.ReadVarUInt(DependencyCount, OutError))
			return false;
		if (DependencyCount > MaximumDependencies)
			return Fail(OutError, "dependency count exceeds bound");
		Result.Summary.Dependencies.reserve(DependencyCount);
		for (uint64 Index = 0; Index < DependencyCount; ++Index)
		{
			std::string Dependency;
			if (!SummaryReader.ReadString(Dependency, OutError))
				return false;
			Result.Summary.Dependencies.push_back(std::move(Dependency));
		}
		if (!SummaryReader.ReadVarUInt(Result.Summary.ObjectCount, OutError)
			|| !SummaryReader.RequireEnd(OutError) || !ValidateSummary(Result.Summary, OutError))
			return false;

		const uint64 FirstSectionOffset = 13ull + SummaryLength + SectionCount * 9ull;
		uint64 ExpectedOffset = FirstSectionOffset;
		for (uint8 Index = 0; Index < SectionCount; ++Index)
		{
			uint8 Kind = 0;
			if (!Reader.ReadU8(Kind, OutError)
				|| !Reader.ReadU32(Result.Sections[Index].Offset, OutError)
				|| !Reader.ReadU32(Result.Sections[Index].Length, OutError))
				return false;
			if (Kind != Index + 1)
				return Fail(OutError, Kind < 1 || Kind > SectionCount
					? "unknown required section" : "duplicate or out-of-order section");
			Result.Sections[Index].Kind = ESectionKind(Kind);
			if (Result.Sections[Index].Offset != ExpectedOffset)
				return Fail(OutError, "section extent is overlapping, gapped, or out of order");
			const uint64 End = uint64(Result.Sections[Index].Offset) + Result.Sections[Index].Length;
			if (End > std::numeric_limits<uint32>::max() || End > Bytes.size())
				return Fail(OutError, "section extent overflow or truncation");
			ExpectedOffset = End;
		}
		if (ExpectedOffset != Bytes.size())
			return Fail(OutError, "trailing bytes after final section");
		OutHeader = std::move(Result);
		return true;
	}
}
