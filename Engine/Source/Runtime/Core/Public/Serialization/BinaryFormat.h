#pragma once

#include "CoreAPI.h"
#include "Serialization/Archive.h"

namespace Durin
{
	inline constexpr uint32 BinaryFormatMarker = 0x01020304;
	inline constexpr uint64 MaximumBinaryStringBytes = 1024ull * 1024ull;

	// Identifies a versioned binary family, schema, payload format, and byte order.
	struct FBinaryFormatHeader
	{
		uint32 Magic = 0;
		uint32 SchemaVersion = 0;
		uint32 FormatVersion = 0;
		uint32 Marker = BinaryFormatMarker;
	};

	// Builds a canonical little-endian binary representation.
	class FBinaryWriter
	{
	public:
		CORE_API auto WriteU8(uint8 Value) -> void;
		CORE_API auto WriteU32(uint32 Value) -> void;
		CORE_API auto WriteU64(uint64 Value) -> void;
		CORE_API auto WriteI64(int64 Value) -> void;
		CORE_API auto WriteString(std::string_view Value) -> void;
		CORE_API auto WriteBytes(std::span<const uint8> Value) -> void;
		CORE_API auto WriteBytes(std::span<const std::byte> Value) -> void;
		CORE_API auto WriteHeader(const FBinaryFormatHeader& Header) -> void;
		auto GetBytes() const -> const std::vector<uint8>& { return Bytes; }
		auto TakeBytes() -> std::vector<uint8> { return std::move(Bytes); }

	private:
		std::vector<uint8> Bytes;
	};

	// Reads canonical binary bytes without owning the source span.
	class FBinaryReader
	{
	public:
		explicit FBinaryReader(std::span<const uint8> InBytes)
			: Archive(InBytes, EArchivePurpose::DerivedDataPayload) {}

		CORE_API auto ReadU8(uint8& Value) -> bool;
		CORE_API auto ReadU32(uint32& Value) -> bool;
		CORE_API auto ReadU64(uint64& Value) -> bool;
		CORE_API auto ReadI64(int64& Value) -> bool;
		CORE_API auto ReadString(std::string& Value, uint64 MaximumBytes = MaximumBinaryStringBytes) -> bool;
		CORE_API auto ReadBytes(std::vector<uint8>& Value, uint64 ByteCount, uint64 MaximumBytes) -> bool;
		CORE_API auto ReadBytes(std::vector<std::byte>& Value, uint64 ByteCount, uint64 MaximumBytes) -> bool;
		CORE_API auto ReadAndValidateHeader(uint32 ExpectedMagic, uint32 ExpectedSchemaVersion,
			uint32 ExpectedFormatVersion, FBinaryFormatHeader* OutHeader = nullptr) -> bool;
		auto IsAtEnd() const -> bool { return !Archive.HasError() && Archive.GetRemainingPayloadBytes() == 0; }
		auto GetRemainingBytes() const -> size_t
		{
			return static_cast<size_t>(Archive.GetRemainingPayloadBytes());
		}

	private:
		FCanonicalMemoryReader Archive;
	};
} // namespace Durin
