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
		CORE_API auto WriteU16(uint16 Value) -> void;
		CORE_API auto WriteU32(uint32 Value) -> void;
		CORE_API auto WriteU64(uint64 Value) -> void;
		CORE_API auto WriteI32(int32 Value) -> void;
		CORE_API auto WriteI64(int64 Value) -> void;
		CORE_API auto WriteFloat(float Value) -> void;
		CORE_API auto WriteString(std::string_view Value) -> void;
		CORE_API auto WriteBytes(std::span<const std::byte> Value) -> void;
		CORE_API auto WriteHeader(const FBinaryFormatHeader& Header) -> void;
		auto GetBytes() const -> const std::vector<std::byte>& { return Bytes; }
		auto TakeBytes() -> std::vector<std::byte> { return std::move(Bytes); }

	private:
		std::vector<std::byte> Bytes;
	};

	// Reads canonical binary bytes without owning the source span.
	class FBinaryReader
	{
	public:
		explicit FBinaryReader(std::span<const std::byte> InBytes)
			: Archive(InBytes, EArchivePurpose::DerivedDataPayload) {}

		CORE_API auto ReadU8(uint8& Value) -> bool;
		CORE_API auto ReadU16(uint16& Value) -> bool;
		CORE_API auto ReadU32(uint32& Value) -> bool;
		CORE_API auto ReadU64(uint64& Value) -> bool;
		CORE_API auto ReadI32(int32& Value) -> bool;
		CORE_API auto ReadI64(int64& Value) -> bool;
		CORE_API auto ReadFloat(float& Value) -> bool;
		CORE_API auto ReadString(std::string& Value, uint64 MaximumBytes = MaximumBinaryStringBytes) -> bool;
		CORE_API auto ReadBytes(std::vector<std::byte>& Value, uint64 ByteCount, uint64 MaximumBytes) -> bool;
		// Returns a non-owning region whose lifetime is bounded by the reader source.
		CORE_API auto ReadRegion(
			std::span<const std::byte>& Value, uint64 ByteCount, uint64 MaximumBytes) -> bool;
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

	// Reads one fixed-width unsigned little-endian value without advancing a reader.
	template<typename T>
	requires std::is_unsigned_v<T>
	[[nodiscard]] auto ReadLittleEndianAt(
		std::span<const std::byte> Bytes, uint64 Offset, T& OutValue) -> bool
	{
		if (Offset > static_cast<uint64>(Bytes.size())) return false;
		const size_t LocalOffset = static_cast<size_t>(Offset);
		if (sizeof(T) > Bytes.size() - LocalOffset) return false;

		T Value = 0;
		for (size_t Index = 0; Index < sizeof(T); ++Index)
			Value |= static_cast<T>(std::to_integer<uint8>(Bytes[LocalOffset + Index])) << (Index * 8);
		OutValue = Value;
		return true;
	}
} // namespace Durin
