#include "Misc/DerivedDataCache.h"

#include "Misc/FileHelper.h"

#if PLATFORM_WINDOWS
#include <Windows.h>
#endif

namespace Durin::DerivedDataCache
{
	namespace
	{
		template<typename T>
		auto WriteLittleEndian(std::vector<uint8>& Bytes, T Value) -> void
		{
			using U = std::make_unsigned_t<T>;
			const U UnsignedValue = static_cast<U>(Value);
			for (size_t Index = 0; Index < sizeof(T); ++Index)
				Bytes.push_back(static_cast<uint8>((UnsignedValue >> (Index * 8)) & 0xff));
		}

		template<typename T>
		auto ReadLittleEndian(std::span<const uint8> Bytes, size_t& Offset, T& Value) -> bool
		{
			if (sizeof(T) > Bytes.size() - Offset) return false;
			using U = std::make_unsigned_t<T>;
			U Result = 0;
			for (size_t Index = 0; Index < sizeof(T); ++Index)
				Result |= static_cast<U>(Bytes[Offset++]) << (Index * 8);
			Value = static_cast<T>(Result);
			return true;
		}
	}

	auto FWriter::WriteU8(uint8 Value) -> void { Bytes.push_back(Value); }
	auto FWriter::WriteU32(uint32 Value) -> void { WriteLittleEndian(Bytes, Value); }
	auto FWriter::WriteU64(uint64 Value) -> void { WriteLittleEndian(Bytes, Value); }
	auto FWriter::WriteI64(int64 Value) -> void { WriteLittleEndian(Bytes, Value); }

	auto FWriter::WriteString(std::string_view Value) -> void
	{
		WriteU64(Value.size());
		Bytes.insert(Bytes.end(), Value.begin(), Value.end());
	}

	auto FWriter::WriteBytes(std::span<const uint8> Value) -> void
	{
		Bytes.insert(Bytes.end(), Value.begin(), Value.end());
	}

	auto FWriter::WriteHeader(const FCacheHeader& Header) -> void
	{
		WriteU32(Header.Magic);
		WriteU32(Header.SchemaVersion);
		WriteU32(Header.FormatVersion);
		WriteU32(Header.Marker);
	}

	auto FReader::ReadU8(uint8& Value) -> bool
	{
		if (Offset == Bytes.size()) return false;
		Value = Bytes[Offset++];
		return true;
	}

	auto FReader::ReadBytes(std::vector<uint8>& Value, uint64 ByteCount, uint64 MaximumBytes) -> bool
	{
		if (ByteCount > MaximumBytes || ByteCount > GetRemainingBytes()) return false;
		Value.assign(Bytes.begin() + static_cast<ptrdiff_t>(Offset),
			Bytes.begin() + static_cast<ptrdiff_t>(Offset + static_cast<size_t>(ByteCount)));
		Offset += static_cast<size_t>(ByteCount);
		return true;
	}

	auto FReader::ReadU32(uint32& Value) -> bool { return ReadLittleEndian(Bytes, Offset, Value); }
	auto FReader::ReadU64(uint64& Value) -> bool { return ReadLittleEndian(Bytes, Offset, Value); }
	auto FReader::ReadI64(int64& Value) -> bool { return ReadLittleEndian(Bytes, Offset, Value); }

	auto FReader::ReadString(std::string& Value, uint64 MaximumBytes) -> bool
	{
		uint64 Size = 0;
		if (!ReadU64(Size) || Size > MaximumBytes || Size > GetRemainingBytes()) return false;
		Value.assign(reinterpret_cast<const char*>(Bytes.data() + Offset), static_cast<size_t>(Size));
		Offset += static_cast<size_t>(Size);
		return true;
	}

	auto FReader::ReadAndValidateHeader(uint32 ExpectedMagic, uint32 ExpectedSchemaVersion, uint32 ExpectedFormatVersion, FCacheHeader* OutHeader) -> bool
	{
		FCacheHeader Header;
		if (!ReadU32(Header.Magic) || !ReadU32(Header.SchemaVersion) || !ReadU32(Header.FormatVersion) || !ReadU32(Header.Marker)) return false;
		if (OutHeader) *OutHeader = Header;
		return Header.Magic == ExpectedMagic && Header.SchemaVersion == ExpectedSchemaVersion
			&& Header.FormatVersion == ExpectedFormatVersion && Header.Marker == SerializationMarker;
	}

	auto FileTimeToStableTicks(const std::filesystem::file_time_type& Time) -> int64
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(Time.time_since_epoch()).count();
	}

	auto StableTicksToFileTime(int64 Ticks) -> std::filesystem::file_time_type
	{
		return std::filesystem::file_time_type{
			std::chrono::duration_cast<std::filesystem::file_time_type::duration>(std::chrono::nanoseconds(Ticks))};
	}

	auto WriteFileAtomically(const std::filesystem::path& Destination, std::span<const uint8> Bytes, std::string* OutError) -> bool
	{
		const std::filesystem::path Temporary = Destination.string() + ".tmp";
		if (!FFileHelper::SaveArrayToFile(std::span{reinterpret_cast<const std::byte*>(Bytes.data()), Bytes.size()}, Temporary))
		{
			if (OutError) *OutError = std::format("Failed to write temporary cache file {}.", Temporary.generic_string());
			return false;
		}

		bool bReplaced = false;
		std::string ReplacementError;
#if PLATFORM_WINDOWS
		bReplaced = MoveFileExW(Temporary.c_str(), Destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
		if (!bReplaced) ReplacementError = std::system_category().message(static_cast<int>(GetLastError()));
#else
		std::error_code Error;
		std::filesystem::rename(Temporary, Destination, Error);
		bReplaced = !Error;
		if (Error) ReplacementError = Error.message();
#endif
		if (!bReplaced)
		{
			std::filesystem::remove(Temporary);
			if (OutError) *OutError = std::format("Failed to replace cache file {}: {}", Destination.generic_string(), ReplacementError);
			return false;
		}
		return true;
	}
} // namespace Durin::DerivedDataCache
