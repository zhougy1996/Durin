#pragma once

namespace Durin::Asset::Build::Private
{
	inline auto AppendLittleEndianU32(std::vector<uint8>& Bytes, uint32 Value) -> void
	{
		for (uint32 Byte = 0; Byte < 4; ++Byte)
			Bytes.push_back(static_cast<uint8>(Value >> (Byte * 8)));
	}

	inline auto AppendLittleEndianU64(std::vector<uint8>& Bytes, uint64 Value) -> void
	{
		for (uint32 Byte = 0; Byte < 8; ++Byte)
			Bytes.push_back(static_cast<uint8>(Value >> (Byte * 8)));
	}

	inline auto ReadLittleEndianU32(
		std::span<const uint8> Bytes, size_t& Offset, uint32& Value) -> bool
	{
		if (Offset > Bytes.size() || Bytes.size() - Offset < 4) return false;
		Value = 0;
		for (uint32 Byte = 0; Byte < 4; ++Byte)
			Value |= uint32(Bytes[Offset++]) << (Byte * 8);
		return true;
	}

	inline auto ReadLittleEndianU64(
		std::span<const uint8> Bytes, size_t& Offset, uint64& Value) -> bool
	{
		if (Offset > Bytes.size() || Bytes.size() - Offset < 8) return false;
		Value = 0;
		for (uint32 Byte = 0; Byte < 8; ++Byte)
			Value |= uint64(Bytes[Offset++]) << (Byte * 8);
		return true;
	}
}
