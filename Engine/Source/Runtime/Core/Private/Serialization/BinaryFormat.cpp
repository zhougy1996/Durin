#include "Serialization/BinaryFormat.h"

namespace Durin
{
	auto FBinaryWriter::WriteU8(uint8 Value) -> void
	{
		FCanonicalMemoryWriter Archive(Bytes, EArchivePurpose::DerivedDataPayload);
		Archive << Value;
	}

	auto FBinaryWriter::WriteU16(uint16 Value) -> void
	{
		FCanonicalMemoryWriter Archive(Bytes, EArchivePurpose::DerivedDataPayload);
		Archive << Value;
	}

	auto FBinaryWriter::WriteU32(uint32 Value) -> void
	{
		FCanonicalMemoryWriter Archive(Bytes, EArchivePurpose::DerivedDataPayload);
		Archive << Value;
	}

	auto FBinaryWriter::WriteU64(uint64 Value) -> void
	{
		FCanonicalMemoryWriter Archive(Bytes, EArchivePurpose::DerivedDataPayload);
		Archive << Value;
	}

	auto FBinaryWriter::WriteI32(int32 Value) -> void
	{
		FCanonicalMemoryWriter Archive(Bytes, EArchivePurpose::DerivedDataPayload);
		Archive << Value;
	}

	auto FBinaryWriter::WriteI64(int64 Value) -> void
	{
		FCanonicalMemoryWriter Archive(Bytes, EArchivePurpose::DerivedDataPayload);
		Archive << Value;
	}

	auto FBinaryWriter::WriteFloat(float Value) -> void
	{
		FCanonicalMemoryWriter Archive(Bytes, EArchivePurpose::DerivedDataPayload);
		Archive << Value;
	}

	auto FBinaryWriter::WriteString(std::string_view Value) -> void
	{
		FCanonicalMemoryWriter Archive(Bytes, EArchivePurpose::DerivedDataPayload);
		std::string Owned(Value);
		SerializeBoundedString(Archive, Owned, std::numeric_limits<uint64>::max());
	}

	auto FBinaryWriter::WriteBytes(std::span<const uint8> Value) -> void
	{
		FCanonicalMemoryWriter Archive(Bytes, EArchivePurpose::DerivedDataPayload);
		Archive.WriteBytes(std::as_bytes(Value));
	}

	auto FBinaryWriter::WriteBytes(std::span<const std::byte> Value) -> void
	{
		FCanonicalMemoryWriter Archive(Bytes, EArchivePurpose::DerivedDataPayload);
		Archive.WriteBytes(Value);
	}

	auto FBinaryWriter::WriteHeader(const FBinaryFormatHeader& Header) -> void
	{
		WriteU32(Header.Magic);
		WriteU32(Header.SchemaVersion);
		WriteU32(Header.FormatVersion);
		WriteU32(Header.Marker);
	}

	auto FBinaryReader::ReadU8(uint8& Value) -> bool
	{
		Archive << Value;
		return !Archive.HasError();
	}

	auto FBinaryReader::ReadU16(uint16& Value) -> bool { Archive << Value; return !Archive.HasError(); }

	auto FBinaryReader::ReadBytes(std::vector<uint8>& Value, uint64 ByteCount, uint64 MaximumBytes) -> bool
	{
		if (ByteCount > MaximumBytes || ByteCount > GetRemainingBytes()
			|| ByteCount > static_cast<uint64>(std::vector<uint8>().max_size())) return false;
		std::vector<uint8> Loaded(static_cast<size_t>(ByteCount));
		if (ByteCount != 0) Archive.ReadBytes(std::as_writable_bytes(std::span<uint8>(Loaded)));
		if (Archive.HasError()) return false;
		Value = std::move(Loaded);
		return true;
	}

	auto FBinaryReader::ReadBytes(std::vector<std::byte>& Value, uint64 ByteCount, uint64 MaximumBytes) -> bool
	{
		if (ByteCount > MaximumBytes || ByteCount > GetRemainingBytes()
			|| ByteCount > static_cast<uint64>(std::vector<std::byte>().max_size())) return false;
		std::vector<std::byte> Loaded(static_cast<size_t>(ByteCount));
		if (ByteCount != 0) Archive.ReadBytes(Loaded);
		if (Archive.HasError()) return false;
		Value = std::move(Loaded);
		return true;
	}

	auto FBinaryReader::ReadRegion(
		std::span<const uint8>& Value, uint64 ByteCount, uint64 MaximumBytes) -> bool
	{
		Value = {};
		if (ByteCount > MaximumBytes) return false;
		return Archive.ReadRegion(ByteCount, Value);
	}

	auto FBinaryReader::ReadU32(uint32& Value) -> bool { Archive << Value; return !Archive.HasError(); }
	auto FBinaryReader::ReadU64(uint64& Value) -> bool { Archive << Value; return !Archive.HasError(); }
	auto FBinaryReader::ReadI32(int32& Value) -> bool { Archive << Value; return !Archive.HasError(); }
	auto FBinaryReader::ReadI64(int64& Value) -> bool { Archive << Value; return !Archive.HasError(); }
	auto FBinaryReader::ReadFloat(float& Value) -> bool { Archive << Value; return !Archive.HasError(); }

	auto FBinaryReader::ReadString(std::string& Value, uint64 MaximumBytes) -> bool
	{
		SerializeBoundedString(Archive, Value, MaximumBytes);
		return !Archive.HasError();
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
