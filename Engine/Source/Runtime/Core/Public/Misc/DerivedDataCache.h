#pragma once

#include "CoreAPI.h"

namespace Durin::DerivedDataCache
{
	inline constexpr uint32 SerializationMarker = 0x01020304;
	inline constexpr uint64 MaximumCacheStringBytes = 1024ull * 1024ull;
	inline constexpr uint32 AssetRegistryMagic = 0x47455241; // AREG
	inline constexpr uint32 AssetRegistrySchemaVersion = 1;
	inline constexpr uint32 ThumbnailIndexMagic = 0x58444954; // TIDX
	inline constexpr uint32 ThumbnailIndexSchemaVersion = 1;

	struct FCacheHeader
	{
		uint32 Magic = 0;
		uint32 SchemaVersion = 0;
		uint32 FormatVersion = 0;
		uint32 Marker = SerializationMarker;
	};

	class CORE_API FWriter
	{
	public:
		auto WriteU8(uint8 Value) -> void;
		auto WriteU32(uint32 Value) -> void;
		auto WriteU64(uint64 Value) -> void;
		auto WriteI64(int64 Value) -> void;
		auto WriteString(std::string_view Value) -> void;
		auto WriteHeader(const FCacheHeader& Header) -> void;
		auto GetBytes() const -> const std::vector<uint8>& { return Bytes; }
		auto TakeBytes() -> std::vector<uint8> { return std::move(Bytes); }

	private:
		std::vector<uint8> Bytes;
	};

	class CORE_API FReader
	{
	public:
		explicit FReader(std::span<const uint8> InBytes) : Bytes(InBytes) {}

		auto ReadU8(uint8& Value) -> bool;
		auto ReadU32(uint32& Value) -> bool;
		auto ReadU64(uint64& Value) -> bool;
		auto ReadI64(int64& Value) -> bool;
		auto ReadString(std::string& Value, uint64 MaximumBytes = MaximumCacheStringBytes) -> bool;
		auto ReadAndValidateHeader(uint32 ExpectedMagic, uint32 ExpectedSchemaVersion, uint32 ExpectedFormatVersion, FCacheHeader* OutHeader = nullptr) -> bool;
		auto IsAtEnd() const -> bool { return Offset == Bytes.size(); }
		auto GetRemainingBytes() const -> size_t { return Bytes.size() - Offset; }

	private:
		std::span<const uint8> Bytes;
		size_t Offset = 0;
	};

	// Cache timestamps use signed nanoseconds in the platform file-clock domain and
	// fixed little-endian bytes. Conversion truncates finer filesystem precision;
	// cache headers isolate platforms with incompatible clock or serialization ABIs.
	CORE_API auto FileTimeToStableTicks(const std::filesystem::file_time_type& Time) -> int64;
	CORE_API auto StableTicksToFileTime(int64 Ticks) -> std::filesystem::file_time_type;
	CORE_API auto WriteFileAtomically(const std::filesystem::path& Destination, std::span<const uint8> Bytes, std::string* OutError = nullptr) -> bool;
} // namespace Durin::DerivedDataCache
