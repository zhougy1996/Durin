#include "Texture/TexturePayloadContainer.h"

#include "Hash/XxHash.h"
#include "Serialization/BinaryFormat.h"
#include "Templates/CheckedArithmetic.h"

namespace Durin::TexturePayloadContainer
{
	namespace
	{
		auto FailContainer(std::string& OutError, std::string Message) -> bool
		{
			OutError = std::move(Message);
			return false;
		}
	}

	auto Build(
		const FDescriptor& Descriptor,
		std::span<const FBuildRecord> Records,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool
	{
		OutBytes.clear();
		OutError.clear();
		if (Records.size() > std::numeric_limits<uint32>::max())
			return FailContainer(OutError, "Texture payload has too many records.");

		const uint64 RecordCount = Records.size();
		const uint64 TableBytes = RecordCount * TexturePayloadRecordSize;
		if (TableBytes > MaximumTexturePayloadBytes - TexturePayloadHeaderSize)
			return FailContainer(OutError, "Texture payload record table exceeds its byte limit.");

		uint64 DataOffset = 0;
		if (!TryAlignUp(TexturePayloadHeaderSize + TableBytes, TexturePayloadAlignment,
			MaximumTexturePayloadBytes, DataOffset))
			return FailContainer(OutError, "Texture payload table alignment exceeds its byte limit.");
		std::vector<uint64> DataOffsets;
		DataOffsets.reserve(Records.size());
		for (size_t RecordIndex = 0; RecordIndex < Records.size(); ++RecordIndex)
		{
			DataOffsets.push_back(DataOffset);
			if (DataOffset > MaximumTexturePayloadBytes
				|| Records[RecordIndex].Data.size() > MaximumTexturePayloadBytes - DataOffset)
				return FailContainer(OutError, "Texture payload exceeds its byte limit.");
			DataOffset += Records[RecordIndex].Data.size();
			if (RecordIndex + 1 < Records.size())
			{
				uint64 AlignedOffset = 0;
				if (!TryAlignUp(DataOffset, TexturePayloadAlignment,
					MaximumTexturePayloadBytes, AlignedOffset))
					return FailContainer(OutError, "Texture payload alignment exceeds its byte limit.");
				DataOffset = AlignedOffset;
			}
		}

		FBinaryWriter Body;
		for (size_t RecordIndex = 0; RecordIndex < Records.size(); ++RecordIndex)
		{
			const FRecord& Record = Records[RecordIndex].Record;
			Body.WriteU32(Record.Coordinate);
			Body.WriteU32(Record.MipIndex);
			Body.WriteU32(Record.Width);
			Body.WriteU32(Record.Height);
			Body.WriteU32(Record.RowPitch);
			Body.WriteU32(Record.LayerPitch);
			Body.WriteU64(DataOffsets[RecordIndex]);
			Body.WriteU64(Records[RecordIndex].Data.size());
		}

		uint64 CurrentOffset = TexturePayloadHeaderSize + Body.GetBytes().size();
		for (size_t RecordIndex = 0; RecordIndex < Records.size(); ++RecordIndex)
		{
			Body.WriteBytes(std::vector<uint8>(
				static_cast<size_t>(DataOffsets[RecordIndex] - CurrentOffset), 0));
			Body.WriteBytes(Records[RecordIndex].Data);
			CurrentOffset = DataOffsets[RecordIndex] + Records[RecordIndex].Data.size();
		}
		const std::vector<uint8> BodyBytes = Body.TakeBytes();

		FBinaryWriter Result;
		Result.WriteU32(TexturePayloadMagic);
		Result.WriteU32(TexturePayloadSchemaVersion);
		Result.WriteU32(Descriptor.ProducerVersion);
		Result.WriteU32(static_cast<uint32>(Descriptor.TargetPlatform));
		Result.WriteU32(static_cast<uint32>(Descriptor.TargetProfile));
		Result.WriteU32(static_cast<uint32>(Descriptor.Dimension));
		Result.WriteU32(static_cast<uint32>(Descriptor.StableFormat));
		Result.WriteU32(Descriptor.SliceCount);
		Result.WriteU32(Descriptor.MipCount);
		Result.WriteU32(TexturePayloadHeaderSize);
		Result.WriteU32(static_cast<uint32>(RecordCount));
		Result.WriteU32(TexturePayloadRecordSize);
		Result.WriteU64(TexturePayloadHeaderSize);
		Result.WriteU64(DataOffset);
		Result.WriteU64(FXxHash64::HashBuffer(BodyBytes).HashValue);
		Result.WriteU64(0);
		Result.WriteBytes(BodyBytes);
		OutBytes = Result.TakeBytes();
		return true;
	}

	auto Parse(
		std::span<const uint8> Bytes,
		Asset::ECookTargetPlatform ExpectedPlatform,
		Asset::ECookTargetProfile ExpectedProfile,
		FDecodedContainer& OutContainer) -> FPayloadDecodeResult
	{
		auto Reject = [](EPayloadDecodeError Code, std::string Message) {
			return FPayloadDecodeResult{Code, std::move(Message)};
		};
		if (Bytes.size() < TexturePayloadHeaderSize)
			return Reject(EPayloadDecodeError::Corrupt,
				"Texture payload header is truncated.");

		uint32 Magic = 0, Schema = 0, Producer = 0, Platform = 0, Profile = 0;
		uint32 Dimension = 0, StableFormat = 0, SliceCount = 0, MipCount = 0;
		uint32 HeaderSize = 0, RecordCount = 0, RecordSize = 0;
		uint64 TableOffset = 0, StoredSize = 0, StoredHash = 0, Reserved = 0;
		if (!ReadLittleEndianAt(Bytes, 0, Magic) || !ReadLittleEndianAt(Bytes, 4, Schema)
			|| !ReadLittleEndianAt(Bytes, 8, Producer) || !ReadLittleEndianAt(Bytes, 12, Platform)
			|| !ReadLittleEndianAt(Bytes, 16, Profile) || !ReadLittleEndianAt(Bytes, 20, Dimension)
			|| !ReadLittleEndianAt(Bytes, 24, StableFormat) || !ReadLittleEndianAt(Bytes, 28, SliceCount)
			|| !ReadLittleEndianAt(Bytes, 32, MipCount) || !ReadLittleEndianAt(Bytes, 36, HeaderSize)
			|| !ReadLittleEndianAt(Bytes, 40, RecordCount) || !ReadLittleEndianAt(Bytes, 44, RecordSize)
			|| !ReadLittleEndianAt(Bytes, 48, TableOffset) || !ReadLittleEndianAt(Bytes, 56, StoredSize)
			|| !ReadLittleEndianAt(Bytes, 64, StoredHash) || !ReadLittleEndianAt(Bytes, 72, Reserved))
			return Reject(EPayloadDecodeError::Corrupt,
				"Texture payload header is truncated.");
		if (Magic != TexturePayloadMagic)
			return Reject(EPayloadDecodeError::Corrupt,
				"Texture payload magic is invalid.");
		if (Schema != TexturePayloadSchemaVersion)
			return Reject(EPayloadDecodeError::Incompatible,
				"Texture payload schema version is unsupported.");
		// Producer identity is diagnostic metadata. Runtime compatibility is owned
		// by the container schema and the stable identifiers interpreted by callers.
		if (Platform != static_cast<uint32>(ExpectedPlatform)
			|| Profile != static_cast<uint32>(ExpectedProfile))
			return Reject(EPayloadDecodeError::Incompatible,
				"Texture payload target platform or profile does not match.");
		if (HeaderSize != TexturePayloadHeaderSize || RecordSize != TexturePayloadRecordSize
			|| TableOffset != TexturePayloadHeaderSize || Reserved != 0)
			return Reject(EPayloadDecodeError::Corrupt,
				"Texture payload container layout is invalid.");
		if (StoredSize != Bytes.size() || StoredSize > MaximumTexturePayloadBytes)
			return Reject(EPayloadDecodeError::Corrupt,
				"Texture payload stored size is invalid.");
		if (FXxHash64::HashBuffer(Bytes.subspan(TexturePayloadHeaderSize)).HashValue != StoredHash)
			return Reject(EPayloadDecodeError::Corrupt,
				"Texture payload checksum does not match.");

		const uint64 TableBytes = static_cast<uint64>(RecordCount) * RecordSize;
		if (TableOffset > StoredSize || TableBytes > StoredSize - TableOffset)
			return Reject(EPayloadDecodeError::Corrupt,
				"Texture payload record table is outside the stored object.");
		const uint64 TableEnd = TableOffset + TableBytes;

		FDecodedContainer Candidate;
		Candidate.Descriptor = {
			.ProducerVersion = Producer,
			.TargetPlatform = static_cast<Asset::ECookTargetPlatform>(Platform),
			.TargetProfile = static_cast<Asset::ECookTargetProfile>(Profile),
			.Dimension = static_cast<ETexturePayloadDimension>(Dimension),
			.StableFormat = static_cast<ETextureStablePixelFormat>(StableFormat),
			.SliceCount = SliceCount,
			.MipCount = MipCount};
		Candidate.Records.reserve(RecordCount);
		uint64 PreviousEnd = TableEnd;
		for (uint32 RecordIndex = 0; RecordIndex < RecordCount; ++RecordIndex)
		{
			const size_t Offset = static_cast<size_t>(
				TableOffset + static_cast<uint64>(RecordIndex) * RecordSize);
			FRecord& Record = Candidate.Records.emplace_back();
			if (!ReadLittleEndianAt(Bytes, Offset, Record.Coordinate)
				|| !ReadLittleEndianAt(Bytes, Offset + 4, Record.MipIndex)
				|| !ReadLittleEndianAt(Bytes, Offset + 8, Record.Width)
				|| !ReadLittleEndianAt(Bytes, Offset + 12, Record.Height)
				|| !ReadLittleEndianAt(Bytes, Offset + 16, Record.RowPitch)
				|| !ReadLittleEndianAt(Bytes, Offset + 20, Record.LayerPitch)
				|| !ReadLittleEndianAt(Bytes, Offset + 24, Record.DataOffset)
				|| !ReadLittleEndianAt(Bytes, Offset + 32, Record.ByteCount))
				return Reject(EPayloadDecodeError::Corrupt,
					"Texture payload record is truncated.");
			if (Record.DataOffset % TexturePayloadAlignment != 0
				|| Record.DataOffset < PreviousEnd || Record.DataOffset > StoredSize
				|| Record.ByteCount > StoredSize - Record.DataOffset)
				return Reject(EPayloadDecodeError::Corrupt,
					"Texture payload record range is misaligned, overlapping, or outside the object.");
			for (uint64 PaddingOffset = PreviousEnd; PaddingOffset < Record.DataOffset; ++PaddingOffset)
				if (Bytes[static_cast<size_t>(PaddingOffset)] != 0)
					return Reject(EPayloadDecodeError::Corrupt,
						"Texture payload contains non-zero alignment padding.");
			PreviousEnd = Record.DataOffset + Record.ByteCount;
		}
		if (PreviousEnd != StoredSize)
			return Reject(EPayloadDecodeError::Corrupt,
				"Texture payload contains trailing data.");
		OutContainer = std::move(Candidate);
		return {};
	}
}
