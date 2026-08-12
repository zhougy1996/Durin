#include "Misc/DerivedDataCache.h"

#include "Misc/FileHelper.h"

namespace Durin::DerivedDataCache
{
	auto FWriter::WriteU8(uint8 Value) -> void
	{
		FCanonicalMemoryWriter Archive(Bytes, EArchivePurpose::DerivedDataPayload);
		Archive << Value;
	}
	auto FWriter::WriteU32(uint32 Value) -> void
	{
		FCanonicalMemoryWriter Archive(Bytes, EArchivePurpose::DerivedDataPayload);
		Archive << Value;
	}
	auto FWriter::WriteU64(uint64 Value) -> void
	{
		FCanonicalMemoryWriter Archive(Bytes, EArchivePurpose::DerivedDataPayload);
		Archive << Value;
	}
	auto FWriter::WriteI64(int64 Value) -> void
	{
		FCanonicalMemoryWriter Archive(Bytes, EArchivePurpose::DerivedDataPayload);
		Archive << Value;
	}

	auto FWriter::WriteString(std::string_view Value) -> void
	{
		FCanonicalMemoryWriter Archive(Bytes, EArchivePurpose::DerivedDataPayload);
		std::string Owned(Value);
		SerializeBoundedString(Archive, Owned, std::numeric_limits<uint64>::max());
	}

	auto FWriter::WriteBytes(std::span<const uint8> Value) -> void
	{
		FCanonicalMemoryWriter Archive(Bytes, EArchivePurpose::DerivedDataPayload);
		Archive.WriteBytes(std::as_bytes(Value));
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
		Archive << Value;
		return !Archive.HasError();
	}

	auto FReader::ReadBytes(std::vector<uint8>& Value, uint64 ByteCount, uint64 MaximumBytes) -> bool
	{
		if (ByteCount > MaximumBytes || ByteCount > GetRemainingBytes()
			|| ByteCount > static_cast<uint64>(std::vector<uint8>().max_size())) return false;
		std::vector<uint8> Loaded(static_cast<size_t>(ByteCount));
		if (ByteCount != 0) Archive.ReadBytes(std::as_writable_bytes(std::span<uint8>(Loaded)));
		if (Archive.HasError()) return false;
		Value = std::move(Loaded);
		return true;
	}

	auto FReader::ReadU32(uint32& Value) -> bool { Archive << Value; return !Archive.HasError(); }
	auto FReader::ReadU64(uint64& Value) -> bool { Archive << Value; return !Archive.HasError(); }
	auto FReader::ReadI64(int64& Value) -> bool { Archive << Value; return !Archive.HasError(); }

	auto FReader::ReadString(std::string& Value, uint64 MaximumBytes) -> bool
	{
		SerializeBoundedString(Archive, Value, MaximumBytes);
		return !Archive.HasError();
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
		FFileHelper::FAtomicFileError Error;
		if (!FFileHelper::SaveArrayToFileAtomically(
			std::span{reinterpret_cast<const std::byte*>(Bytes.data()), Bytes.size()},
			Destination,
			&Error
		))
		{
			if (OutError) *OutError = Error.ToString();
			return false;
		}
		return true;
	}
} // namespace Durin::DerivedDataCache
