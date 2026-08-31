#include "Serialization/BinaryFormat.h"

namespace Durin
{
	auto FBinaryWriter::WriteI8(int8 Value) -> void { WriteInteger(Value); }
	auto FBinaryWriter::WriteI16(int16 Value) -> void { WriteInteger(Value); }
	auto FBinaryWriter::WriteU8(uint8 Value) -> void
	{
		WriteInteger(Value);
	}

	auto FBinaryWriter::WriteU16(uint16 Value) -> void
	{
		WriteInteger(Value);
	}

	auto FBinaryWriter::WriteU32(uint32 Value) -> void
	{
		WriteInteger(Value);
	}

	auto FBinaryWriter::WriteU64(uint64 Value) -> void
	{
		WriteInteger(Value);
	}

	auto FBinaryWriter::WriteI32(int32 Value) -> void
	{
		WriteInteger(Value);
	}

	auto FBinaryWriter::WriteI64(int64 Value) -> void
	{
		WriteInteger(Value);
	}

	auto FBinaryWriter::WriteFloat(float Value) -> void
	{
		WriteInteger(std::bit_cast<uint32>(Value));
	}

	auto FBinaryWriter::WriteDouble(double Value) -> void
	{
		WriteInteger(std::bit_cast<uint64>(Value));
	}

	auto FBinaryWriter::WriteVarUInt(uint64 Value) -> void
	{
		std::array<std::byte, 10> Encoded{};
		size_t Size = 0;
		do
		{
			uint8 Byte = static_cast<uint8>(Value & 0x7f);
			Value >>= 7;
			if (Value != 0) Byte |= 0x80;
			Encoded[Size++] = static_cast<std::byte>(Byte);
		} while (Value != 0);
		WriteBytes(std::span(Encoded).first(Size));
	}

	auto FBinaryWriter::WriteVarInt(int64 Value) -> void
	{
		WriteVarUInt((static_cast<uint64>(Value) << 1) ^ (0 - static_cast<uint64>(Value < 0)));
	}

	auto FBinaryWriter::WriteGuid(const FGuid& Value) -> void
	{
		if (!CanWrite(16, 16)) return;
		WriteU32(Value.A); WriteU32(Value.B); WriteU32(Value.C); WriteU32(Value.D);
	}

	auto FBinaryWriter::WriteHash128(const FXxHash128& Value) -> void
	{
		if (!CanWrite(16, 16)) return;
		WriteU64(Value.HashLow); WriteU64(Value.HashHigh);
	}

	auto FBinaryWriter::WriteString(std::string_view Value) -> void
	{
		if (static_cast<uint64>(Value.size()) > std::numeric_limits<uint64>::max() - sizeof(uint64))
		{
			bLimitError = true;
			return;
		}
		if (!CanWrite(sizeof(uint64) + static_cast<uint64>(Value.size()), Value.size())) return;
		uint64 Size = Value.size();
		Archive << Size;
		Archive.WriteBytes(std::as_bytes(std::span(Value)));
	}

	auto FBinaryWriter::WriteBytes(std::span<const std::byte> Value) -> void
	{
		if (!CanWrite(Value.size(), Value.size())) return;
		Archive.WriteBytes(Value);
	}

	auto FBinaryWriter::TakeBytes() -> FByteArray
	{
		bLimitError = false;
		return std::exchange(Bytes, {});
	}

	auto FBinaryWriter::WriteHeader(const FBinaryFormatHeader& Header) -> void
	{
		if (!CanWrite(16, 0)) return;
		WriteU32(Header.Magic);
		WriteU32(Header.SchemaVersion);
		WriteU32(Header.FormatVersion);
		WriteU32(Header.Marker);
	}

	auto FBinaryReader::ReadI8(int8& Value) -> bool { return ReadInteger(Value); }
	auto FBinaryReader::ReadI16(int16& Value) -> bool { return ReadInteger(Value); }
	auto FBinaryReader::ReadU8(uint8& Value) -> bool
	{
		return ReadInteger(Value);
	}

	auto FBinaryReader::ReadU16(uint16& Value) -> bool { return ReadInteger(Value); }

	auto FBinaryReader::ReadBytes(FByteArray& Value, uint64 ByteCount, uint64 MaximumBytes) -> bool
	{
		if (HasError() || ByteCount > MaximumBytes || ByteCount > Limits.MaximumFieldBytes
			|| ByteCount > GetRemainingBytes()
			|| ByteCount > static_cast<uint64>(FByteArray().max_size())) return false;
		FByteArray Loaded(static_cast<size_t>(ByteCount));
		if (ByteCount != 0) Archive.ReadBytes(Loaded);
		if (Archive.HasError()) return false;
		Value = std::move(Loaded);
		return true;
	}

	auto FBinaryReader::ReadRegion(
		std::span<const std::byte>& Value, uint64 ByteCount, uint64 MaximumBytes) -> bool
	{
		Value = {};
		if (HasError() || ByteCount > MaximumBytes || ByteCount > Limits.MaximumFieldBytes) return false;
		return Archive.ReadRegion(ByteCount, Value);
	}

	auto FBinaryReader::ReadU32(uint32& Value) -> bool { return ReadInteger(Value); }
	auto FBinaryReader::ReadU64(uint64& Value) -> bool { return ReadInteger(Value); }
	auto FBinaryReader::ReadI32(int32& Value) -> bool { return ReadInteger(Value); }
	auto FBinaryReader::ReadI64(int64& Value) -> bool { return ReadInteger(Value); }
	auto FBinaryReader::ReadFloat(float& Value) -> bool
	{
		uint32 Bits = 0;
		if (!ReadU32(Bits)) return false;
		Value = std::bit_cast<float>(Bits);
		return true;
	}
	auto FBinaryReader::ReadDouble(double& Value) -> bool
	{
		uint64 Bits = 0;
		if (!ReadU64(Bits)) return false;
		Value = std::bit_cast<double>(Bits);
		return true;
	}

	auto FBinaryReader::ReadVarUInt(uint64& Value) -> bool
	{
		uint64 Decoded = 0;
		for (uint32 Index = 0; Index < 10; ++Index)
		{
			if (static_cast<uint64>(Index) + 1 > Limits.MaximumFieldBytes) return false;
			uint8 Byte = 0;
			if (!ReadU8(Byte) || (Index == 9 && (Byte & 0xfe) != 0)) return false;
			Decoded |= static_cast<uint64>(Byte & 0x7f) << (Index * 7);
			if ((Byte & 0x80) == 0)
			{
				if (Index != 0 && (Byte & 0x7f) == 0) return false;
				Value = Decoded;
				return true;
			}
		}
		return false;
	}

	auto FBinaryReader::ReadVarInt(int64& Value) -> bool
	{
		uint64 Encoded = 0;
		if (!ReadVarUInt(Encoded)) return false;
		const uint64 Bits = (Encoded >> 1) ^ (0 - (Encoded & 1));
		Value = std::bit_cast<int64>(Bits);
		return true;
	}

	auto FBinaryReader::ReadGuid(FGuid& Value) -> bool
	{
		if (16 > Limits.MaximumFieldBytes || 16 > GetRemainingBytes()) return false;
		FGuid Candidate;
		if (!ReadU32(Candidate.A) || !ReadU32(Candidate.B)
			|| !ReadU32(Candidate.C) || !ReadU32(Candidate.D)) return false;
		Value = Candidate;
		return true;
	}

	auto FBinaryReader::ReadHash128(FXxHash128& Value) -> bool
	{
		if (16 > Limits.MaximumFieldBytes || 16 > GetRemainingBytes()) return false;
		FXxHash128 Candidate;
		if (!ReadU64(Candidate.HashLow) || !ReadU64(Candidate.HashHigh)) return false;
		Value = Candidate;
		return true;
	}

	auto FBinaryReader::ReadString(std::string& Value, uint64 MaximumBytes) -> bool
	{
		uint64 Size = 0;
		if (!ReadU64(Size) || Size > MaximumBytes || Size > Limits.MaximumFieldBytes
			|| Size > GetRemainingBytes() || Size > static_cast<uint64>(std::string().max_size())) return false;
		std::span<const std::byte> Encoded;
		if (!Archive.ReadRegion(Size, Encoded)) return false;
		Value.assign(reinterpret_cast<const char*>(Encoded.data()), Encoded.size());
		return true;
	}

	auto FBinaryReader::ReadAndValidateHeader(uint32 ExpectedMagic, uint32 ExpectedSchemaVersion,
		uint32 ExpectedFormatVersion, FBinaryFormatHeader* OutHeader) -> bool
	{
		FBinaryFormatHeader Header;
		if (!ReadU32(Header.Magic) || !ReadU32(Header.SchemaVersion)
			|| !ReadU32(Header.FormatVersion) || !ReadU32(Header.Marker)) return false;
		if (OutHeader) *OutHeader = Header;
		return Header.Magic == ExpectedMagic && Header.SchemaVersion == ExpectedSchemaVersion
			&& Header.FormatVersion == ExpectedFormatVersion && Header.Marker == BinaryFormatMarker;
	}
} // namespace Durin
