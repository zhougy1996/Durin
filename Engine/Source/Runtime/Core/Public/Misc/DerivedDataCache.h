#pragma once

#include "CoreAPI.h"
#include "Serialization/Archive.h"

namespace Durin::DerivedDataCache
{
	inline constexpr uint32 SerializationMarker = 0x01020304;
	inline constexpr uint64 MaximumCacheStringBytes = 1024ull * 1024ull;
	inline constexpr uint32 AssetRegistryMagic = 0x47455241; // AREG
	inline constexpr uint32 AssetRegistrySchemaVersion = 2;
	inline constexpr uint32 ThumbnailIndexMagic = 0x58444954; // TIDX
	inline constexpr uint32 ThumbnailIndexSchemaVersion = 2;

	// Identifies the binary cache family, schema, payload format, and byte order.
	struct FCacheHeader
	{
		uint32 Magic = 0;
		uint32 SchemaVersion = 0;
		uint32 FormatVersion = 0;
		uint32 Marker = SerializationMarker;
	};

	// Builds the canonical little-endian byte representation used by derived-data caches.
	class FWriter
	{
	public:
		CORE_API auto WriteU8(uint8 Value) -> void;
		CORE_API auto WriteU32(uint32 Value) -> void;
		CORE_API auto WriteU64(uint64 Value) -> void;
		CORE_API auto WriteI64(int64 Value) -> void;
		CORE_API auto WriteString(std::string_view Value) -> void;
		CORE_API auto WriteBytes(std::span<const uint8> Value) -> void;
		CORE_API auto WriteHeader(const FCacheHeader& Header) -> void;
		auto GetBytes() const -> const std::vector<uint8>& { return Bytes; }
		auto TakeBytes() -> std::vector<uint8> { return std::move(Bytes); }

	private:
		std::vector<uint8> Bytes;
	};

	// Reads canonical cache bytes without owning the source span.
	class FReader
	{
	public:
		explicit FReader(std::span<const uint8> InBytes)
			: Archive(InBytes, EArchivePurpose::DerivedDataPayload) {}

		CORE_API auto ReadU8(uint8& Value) -> bool;
		CORE_API auto ReadU32(uint32& Value) -> bool;
		CORE_API auto ReadU64(uint64& Value) -> bool;
		CORE_API auto ReadI64(int64& Value) -> bool;
		CORE_API auto ReadString(std::string& Value, uint64 MaximumBytes = MaximumCacheStringBytes) -> bool;
		CORE_API auto ReadBytes(std::vector<uint8>& Value, uint64 ByteCount, uint64 MaximumBytes) -> bool;
		CORE_API auto ReadAndValidateHeader(uint32 ExpectedMagic, uint32 ExpectedSchemaVersion, uint32 ExpectedFormatVersion, FCacheHeader* OutHeader = nullptr) -> bool;
		auto IsAtEnd() const -> bool { return !Archive.HasError() && Archive.GetRemainingPayloadBytes() == 0; }
		auto GetRemainingBytes() const -> size_t
		{
			return static_cast<size_t>(Archive.GetRemainingPayloadBytes());
		}

	private:
		FCanonicalMemoryReader Archive;
	};

	// Cache timestamps use signed nanoseconds in the platform file-clock domain and
	// fixed little-endian bytes. Conversion truncates finer filesystem precision;
	// cache headers isolate platforms with incompatible clock or serialization ABIs.
	CORE_API auto FileTimeToStableTicks(const std::filesystem::file_time_type& Time) -> int64;
	CORE_API auto StableTicksToFileTime(int64 Ticks) -> std::filesystem::file_time_type;
	CORE_API auto WriteFileAtomically(const std::filesystem::path& Destination, std::span<const uint8> Bytes, std::string* OutError = nullptr) -> bool;
} // namespace Durin::DerivedDataCache
