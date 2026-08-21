#include "Texture/TextureDerivedData.h"

#include "Serialization/BinaryFormat.h"
#include "Serialization/Archive.h"
#include "Serialization/EngineWire.h"
#include "Texture/TextureCube.h"

namespace Durin
{
	namespace
	{
		auto Fail(std::string& OutError, std::string Message) -> bool
		{
			OutError = std::move(Message);
			return false;
		}

		auto IsSupportedTarget(Asset::ECookTargetPlatform Platform, Asset::ECookTargetProfile Profile) -> bool
		{
			return Platform == Asset::ECookTargetPlatform::Win64
				&& (Profile == Asset::ECookTargetProfile::Game
					|| Profile == Asset::ECookTargetProfile::EditorValidation);
		}

		auto ToStablePixelFormat(EPixelFormat Format, ETextureStablePixelFormat& OutFormat) -> bool
		{
			switch (Format)
			{
			case EPixelFormat::BC1_UNORM: OutFormat = ETextureStablePixelFormat::BC1_UNORM; return true;
			case EPixelFormat::BC1_UNORM_SRGB: OutFormat = ETextureStablePixelFormat::BC1_UNORM_SRGB; return true;
			case EPixelFormat::BC3_UNORM: OutFormat = ETextureStablePixelFormat::BC3_UNORM; return true;
			case EPixelFormat::BC3_UNORM_SRGB: OutFormat = ETextureStablePixelFormat::BC3_UNORM_SRGB; return true;
			case EPixelFormat::BC5_UNORM: OutFormat = ETextureStablePixelFormat::BC5_UNORM; return true;
			case EPixelFormat::BC7_UNORM: OutFormat = ETextureStablePixelFormat::BC7_UNORM; return true;
			case EPixelFormat::BC7_UNORM_SRGB: OutFormat = ETextureStablePixelFormat::BC7_UNORM_SRGB; return true;
			default: return false;
			}
		}

		auto FromStablePixelFormat(uint32 StableFormat, EPixelFormat& OutFormat) -> bool
		{
			switch (static_cast<ETextureStablePixelFormat>(StableFormat))
			{
			case ETextureStablePixelFormat::BC1_UNORM: OutFormat = EPixelFormat::BC1_UNORM; return true;
			case ETextureStablePixelFormat::BC1_UNORM_SRGB: OutFormat = EPixelFormat::BC1_UNORM_SRGB; return true;
			case ETextureStablePixelFormat::BC3_UNORM: OutFormat = EPixelFormat::BC3_UNORM; return true;
			case ETextureStablePixelFormat::BC3_UNORM_SRGB: OutFormat = EPixelFormat::BC3_UNORM_SRGB; return true;
			case ETextureStablePixelFormat::BC5_UNORM: OutFormat = EPixelFormat::BC5_UNORM; return true;
			case ETextureStablePixelFormat::BC7_UNORM: OutFormat = EPixelFormat::BC7_UNORM; return true;
			case ETextureStablePixelFormat::BC7_UNORM_SRGB: OutFormat = EPixelFormat::BC7_UNORM_SRGB; return true;
			default: return false;
			}
		}

		auto AlignPayloadOffset(uint64 Offset) -> uint64
		{
			return EngineWire::AlignUp(Offset, TexturePayloadAlignment);
		}

		auto IsCompleteMipChain(const FTexturePlatformData& PlatformData) -> bool
		{
			return PlatformData.IsValid()
				&& PlatformData.Mips.size() <= MaximumTextureMipCount
				&& PlatformData.Mips.front().Width <= MaximumTexture2DDimension
				&& PlatformData.Mips.front().Height <= MaximumTexture2DDimension
				&& PlatformData.Mips.back().Width == 1
				&& PlatformData.Mips.back().Height == 1;
		}

		auto IsCompleteCubeMipChain(const FTextureCubePlatformData& PlatformData) -> bool
		{
			if (!PlatformData.IsValid()) return false;
			const FTexturePlatformData& Reference = PlatformData.Faces[0];
			if (Reference.Mips.size() > MaximumTextureMipCount
				|| Reference.Mips.front().Width > MaximumTextureCubeDimension
				|| Reference.Mips.front().Height > MaximumTextureCubeDimension
				|| Reference.Mips.back().Width != 1
				|| Reference.Mips.back().Height != 1) return false;
			for (const FTexturePlatformData& Face : PlatformData.Faces)
			{
				for (size_t MipIndex = 0; MipIndex < Face.Mips.size(); ++MipIndex)
				{
					const FTexture2DMipData& Mip = Face.Mips[MipIndex];
					if (!Mip.IsValid(PlatformData.PixelFormat)) return false;
					if (MipIndex > 0)
					{
						const FTexture2DMipData& Previous = Face.Mips[MipIndex - 1];
						if (Mip.Width != std::max(Previous.Width / 2, 1u)
							|| Mip.Height != std::max(Previous.Height / 2, 1u)) return false;
					}
				}
			}
			return true;
		}

		using EngineWire::ReadLittleEndianAt;
	}

	auto BuildTexture2DSerializedValue(
		const FTexturePlatformData& PlatformData,
		Asset::ECookTargetPlatform TargetPlatform,
		Asset::ECookTargetProfile TargetProfile,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool
	{
		OutError.clear();
		if (!IsSupportedTarget(TargetPlatform, TargetProfile))
			return Fail(OutError, "Texture payload target platform or profile is unsupported.");
		if (!IsCompleteMipChain(PlatformData))
			return Fail(OutError, "Texture payload requires a valid, complete, bounded mip chain.");
		ETextureStablePixelFormat StableFormat;
		if (!ToStablePixelFormat(PlatformData.PixelFormat, StableFormat))
			return Fail(OutError, "Texture payload pixel format has no stable serialized identifier.");

		const uint32 RecordCount = static_cast<uint32>(PlatformData.Mips.size());
		uint64 DataOffset = AlignPayloadOffset(
			TexturePayloadHeaderSize + static_cast<uint64>(RecordCount) * TexturePayloadRecordSize);
		std::vector<uint64> DataOffsets;
		DataOffsets.reserve(RecordCount);
		for (const FTexture2DMipData& Mip : PlatformData.Mips)
		{
			DataOffsets.push_back(DataOffset);
			if (Mip.Pixels.size() > MaximumTexturePayloadBytes - DataOffset)
				return Fail(OutError, "Texture payload exceeds its stored-size limit.");
			DataOffset += Mip.Pixels.size();
			if (&Mip != &PlatformData.Mips.back()) DataOffset = AlignPayloadOffset(DataOffset);
		}
		if (DataOffset > MaximumTexturePayloadBytes)
			return Fail(OutError, "Texture payload exceeds its stored-size limit.");

		FBinaryWriter Body;
		for (uint32 MipIndex = 0; MipIndex < RecordCount; ++MipIndex)
		{
			const FTexture2DMipData& Mip = PlatformData.Mips[MipIndex];
			Body.WriteU32(0);
			Body.WriteU32(MipIndex);
			Body.WriteU32(Mip.Width);
			Body.WriteU32(Mip.Height);
			Body.WriteU32(Mip.RowPitch);
			Body.WriteU32(0);
			Body.WriteU64(DataOffsets[MipIndex]);
			Body.WriteU64(Mip.Pixels.size());
		}
		uint64 CurrentOffset = TexturePayloadHeaderSize + Body.GetBytes().size();
		for (uint32 MipIndex = 0; MipIndex < RecordCount; ++MipIndex)
		{
			std::vector<uint8> Padding(static_cast<size_t>(DataOffsets[MipIndex] - CurrentOffset), 0);
			Body.WriteBytes(Padding);
			Body.WriteBytes(PlatformData.Mips[MipIndex].Pixels);
			CurrentOffset = DataOffsets[MipIndex] + PlatformData.Mips[MipIndex].Pixels.size();
		}
		const std::vector<uint8> BodyBytes = Body.TakeBytes();

		FBinaryWriter Result;
		Result.WriteU32(TexturePayloadMagic);
		Result.WriteU32(TexturePayloadSchemaVersion);
		Result.WriteU32(Texture2DPayloadProducerVersion);
		Result.WriteU32(static_cast<uint32>(TargetPlatform));
		Result.WriteU32(static_cast<uint32>(TargetProfile));
		Result.WriteU32(static_cast<uint32>(ETexturePayloadDimension::Texture2D));
		Result.WriteU32(static_cast<uint32>(StableFormat));
		Result.WriteU32(1);
		Result.WriteU32(RecordCount);
		Result.WriteU32(TexturePayloadHeaderSize);
		Result.WriteU32(RecordCount);
		Result.WriteU32(TexturePayloadRecordSize);
		Result.WriteU64(TexturePayloadHeaderSize);
		Result.WriteU64(DataOffset);
		Result.WriteU64(FXxHash64::HashBuffer(BodyBytes).HashValue);
		Result.WriteU64(0);
		Result.WriteBytes(BodyBytes);
		OutBytes = Result.TakeBytes();
		return true;
	}

	auto ParseTexture2DSerializedValue(
		std::span<const uint8> Bytes,
		Asset::ECookTargetPlatform ExpectedPlatform,
		Asset::ECookTargetProfile ExpectedProfile,
		std::unique_ptr<FTexturePlatformData>& OutPlatformData,
		std::string& OutError,
		EPayloadDecodeError& OutCode) -> bool
	{
		OutError.clear();
		OutCode = EPayloadDecodeError::Corrupt;
		if (!IsSupportedTarget(ExpectedPlatform, ExpectedProfile))
		{
			OutCode = EPayloadDecodeError::Incompatible;
			return Fail(OutError, "Texture payload expected target is unsupported.");
		}
		if (Bytes.size() < TexturePayloadHeaderSize)
			return Fail(OutError, "Texture payload header is truncated.");

		uint32 Magic = 0;
		uint32 SchemaVersion = 0;
		uint32 BuilderVersion = 0;
		uint32 Platform = 0;
		uint32 Profile = 0;
		uint32 Dimension = 0;
		uint32 StableFormat = 0;
		uint32 SliceCount = 0;
		uint32 MipCount = 0;
		uint32 HeaderSize = 0;
		uint32 RecordCount = 0;
		uint32 RecordSize = 0;
		uint64 RecordTableOffset = 0;
		uint64 StoredSize = 0;
		uint64 StoredHash = 0;
		uint64 Reserved = 0;
		if (!ReadLittleEndianAt(Bytes, 0, Magic) || !ReadLittleEndianAt(Bytes, 4, SchemaVersion)
			|| !ReadLittleEndianAt(Bytes, 8, BuilderVersion) || !ReadLittleEndianAt(Bytes, 12, Platform)
			|| !ReadLittleEndianAt(Bytes, 16, Profile) || !ReadLittleEndianAt(Bytes, 20, Dimension)
			|| !ReadLittleEndianAt(Bytes, 24, StableFormat) || !ReadLittleEndianAt(Bytes, 28, SliceCount)
			|| !ReadLittleEndianAt(Bytes, 32, MipCount) || !ReadLittleEndianAt(Bytes, 36, HeaderSize)
			|| !ReadLittleEndianAt(Bytes, 40, RecordCount) || !ReadLittleEndianAt(Bytes, 44, RecordSize)
			|| !ReadLittleEndianAt(Bytes, 48, RecordTableOffset) || !ReadLittleEndianAt(Bytes, 56, StoredSize)
			|| !ReadLittleEndianAt(Bytes, 64, StoredHash) || !ReadLittleEndianAt(Bytes, 72, Reserved))
			return Fail(OutError, "Texture payload header is truncated.");
		if (Magic != TexturePayloadMagic) return Fail(OutError, "Texture payload magic is invalid.");
		if (SchemaVersion != TexturePayloadSchemaVersion)
		{
			OutCode = EPayloadDecodeError::Incompatible;
			return Fail(OutError, "Texture payload schema version is unsupported.");
		}
		// Producer identity is diagnostic metadata. Runtime compatibility is owned
		// by the payload schema and stable value identifiers.
		(void)BuilderVersion;
		if (Platform != static_cast<uint32>(ExpectedPlatform)
			|| Profile != static_cast<uint32>(ExpectedProfile))
			return Fail(OutError, "Texture payload target platform or profile does not match.");
		if (Dimension != static_cast<uint32>(ETexturePayloadDimension::Texture2D)
			|| SliceCount != 1 || MipCount == 0 || MipCount > MaximumTextureMipCount
			|| RecordCount != MipCount || HeaderSize != TexturePayloadHeaderSize
			|| RecordSize != TexturePayloadRecordSize || RecordTableOffset != TexturePayloadHeaderSize
			|| Reserved != 0)
			return Fail(OutError, "Texture2D payload header layout or counts are invalid.");
		if (StoredSize != Bytes.size() || StoredSize > MaximumTexturePayloadBytes)
			return Fail(OutError, "Texture payload stored size is invalid.");
		if (FXxHash64::HashBuffer(Bytes.subspan(TexturePayloadHeaderSize)).HashValue != StoredHash)
			return Fail(OutError, "Texture payload checksum does not match.");

		EPixelFormat PixelFormat = EPixelFormat::Unknown;
		if (!FromStablePixelFormat(StableFormat, PixelFormat))
		{
			OutCode = EPayloadDecodeError::Incompatible;
			return Fail(OutError, "Texture payload pixel format identifier is unsupported.");
		}
		const uint64 TableEnd = RecordTableOffset + static_cast<uint64>(RecordCount) * RecordSize;
		if (TableEnd < RecordTableOffset || TableEnd > StoredSize)
			return Fail(OutError, "Texture payload record table is outside the stored object.");

		auto Candidate = std::make_unique<FTexturePlatformData>();
		Candidate->PixelFormat = PixelFormat;
		Candidate->Mips.reserve(MipCount);
		uint64 PreviousEnd = TableEnd;
		for (uint32 MipIndex = 0; MipIndex < MipCount; ++MipIndex)
		{
			const size_t Offset = static_cast<size_t>(
				RecordTableOffset + static_cast<uint64>(MipIndex) * RecordSize);
			uint32 Slice = 0;
			uint32 StoredMip = 0;
			uint32 Width = 0;
			uint32 Height = 0;
			uint32 RowPitch = 0;
			uint32 RecordReserved = 0;
			uint64 DataOffset = 0;
			uint64 ByteCount = 0;
			if (!ReadLittleEndianAt(Bytes, Offset, Slice) || !ReadLittleEndianAt(Bytes, Offset + 4, StoredMip)
				|| !ReadLittleEndianAt(Bytes, Offset + 8, Width) || !ReadLittleEndianAt(Bytes, Offset + 12, Height)
				|| !ReadLittleEndianAt(Bytes, Offset + 16, RowPitch) || !ReadLittleEndianAt(Bytes, Offset + 20, RecordReserved)
				|| !ReadLittleEndianAt(Bytes, Offset + 24, DataOffset) || !ReadLittleEndianAt(Bytes, Offset + 32, ByteCount))
				return Fail(OutError, "Texture payload subresource record is truncated.");
			if (Slice != 0 || StoredMip != MipIndex || RecordReserved != 0
				|| Width == 0 || Height == 0 || Width > MaximumTexture2DDimension
				|| Height > MaximumTexture2DDimension)
				return Fail(OutError, "Texture payload subresource identity or dimensions are invalid.");
			if (MipIndex > 0)
			{
				const FTexture2DMipData& PreviousMip = Candidate->Mips.back();
				if (Width != std::max(PreviousMip.Width / 2, 1u)
					|| Height != std::max(PreviousMip.Height / 2, 1u))
					return Fail(OutError, "Texture payload mip dimensions are not a complete progression.");
			}
			const FPixelFormatLayout Layout = GetPixelFormatLayout(PixelFormat, Width, Height);
			if (RowPitch != Layout.RowPitch || ByteCount != Layout.DataSize)
				return Fail(OutError, "Texture payload subresource layout does not match its format.");
			if (DataOffset % TexturePayloadAlignment != 0 || DataOffset < PreviousEnd
				|| DataOffset > StoredSize || ByteCount > StoredSize - DataOffset)
				return Fail(OutError, "Texture payload subresource range is misaligned, overlapping, or outside the object.");
			for (uint64 PaddingOffset = PreviousEnd; PaddingOffset < DataOffset; ++PaddingOffset)
				if (Bytes[static_cast<size_t>(PaddingOffset)] != 0)
					return Fail(OutError, "Texture payload contains non-zero alignment padding.");

			FTexture2DMipData& Mip = Candidate->Mips.emplace_back();
			Mip.Width = Width;
			Mip.Height = Height;
			Mip.RowPitch = RowPitch;
			Mip.Pixels.assign(
				Bytes.begin() + static_cast<size_t>(DataOffset),
				Bytes.begin() + static_cast<size_t>(DataOffset + ByteCount));
			PreviousEnd = DataOffset + ByteCount;
		}
		if (PreviousEnd != StoredSize)
			return Fail(OutError, "Texture payload contains trailing data.");
		if (!IsCompleteMipChain(*Candidate))
			return Fail(OutError, "Texture payload mip chain is incomplete or invalid.");
		OutPlatformData = std::move(Candidate);
		return true;
	}

	auto FTexturePlatformData::Serialize(
		FArchive& Ar,
		const FTexturePlatformSerializationContext& Context) -> void
	{
		if (Ar.HasError()) return;
		if (Ar.IsSaving())
		{
			std::vector<uint8> Bytes;
			std::string Error;
			if (!BuildTexture2DSerializedValue(
				*this, Context.TargetPlatform, Context.TargetProfile, Bytes, Error))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, Error);
				return;
			}
			Ar.WriteBytes(std::as_bytes(std::span<const uint8>(Bytes)));
			return;
		}

		const uint64 ByteCount = Ar.GetRemainingPayloadBytes();
		if (ByteCount == std::numeric_limits<uint64>::max())
		{
			Ar.Fail(EArchiveFailureCode::UnsupportedCapability,
				"Texture platform data requires a bounded input archive.");
			return;
		}
		if (ByteCount > MaximumTexturePayloadBytes
			|| ByteCount > static_cast<uint64>(std::vector<uint8>().max_size()))
		{
			Ar.Fail(EArchiveFailureCode::LimitExceeded,
				"Texture platform data exceeds its stored-size limit.");
			return;
		}
		std::vector<uint8> Bytes(static_cast<size_t>(ByteCount));
		Ar.ReadBytes(std::as_writable_bytes(std::span<uint8>(Bytes)));
		if (Ar.HasError()) return;

		std::unique_ptr<FTexturePlatformData> Candidate;
		std::string Error;
		EPayloadDecodeError Code = EPayloadDecodeError::Corrupt;
		if (!ParseTexture2DSerializedValue(
			Bytes, Context.TargetPlatform, Context.TargetProfile, Candidate, Error, Code))
		{
			Ar.Fail(Code == EPayloadDecodeError::Incompatible
				? EArchiveFailureCode::UnsupportedVersion : EArchiveFailureCode::InvalidData,
				Error);
			return;
		}
		*this = std::move(*Candidate);
	}

	auto BuildTextureCubeSerializedValue(
		const FTextureCubePlatformData& PlatformData,
		Asset::ECookTargetPlatform TargetPlatform,
		Asset::ECookTargetProfile TargetProfile,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool
	{
		OutError.clear();
		if (!IsSupportedTarget(TargetPlatform, TargetProfile))
			return Fail(OutError, "Texture payload target platform or profile is unsupported.");
		if (!IsCompleteCubeMipChain(PlatformData))
			return Fail(OutError, "TextureCube payload requires six compatible, complete, bounded mip chains.");
		ETextureStablePixelFormat StableFormat;
		if (!ToStablePixelFormat(PlatformData.PixelFormat, StableFormat))
			return Fail(OutError, "Texture payload pixel format has no stable serialized identifier.");

		const uint32 MipCount = static_cast<uint32>(PlatformData.Faces[0].Mips.size());
		const uint32 RecordCount = static_cast<uint32>(TextureCubeFaceCount) * MipCount;
		uint64 DataOffset = AlignPayloadOffset(
			TexturePayloadHeaderSize + static_cast<uint64>(RecordCount) * TexturePayloadRecordSize);
		std::vector<uint64> DataOffsets;
		DataOffsets.reserve(RecordCount);
		for (const FTexturePlatformData& Face : PlatformData.Faces)
		{
			for (const FTexture2DMipData& Mip : Face.Mips)
			{
				DataOffsets.push_back(DataOffset);
				if (Mip.Pixels.size() > MaximumTexturePayloadBytes - DataOffset)
					return Fail(OutError, "Texture payload exceeds its stored-size limit.");
				DataOffset += Mip.Pixels.size();
				if (DataOffsets.size() != RecordCount) DataOffset = AlignPayloadOffset(DataOffset);
			}
		}
		if (DataOffset > MaximumTexturePayloadBytes)
			return Fail(OutError, "Texture payload exceeds its stored-size limit.");

		FBinaryWriter Body;
		uint32 RecordIndex = 0;
		for (uint32 Slice = 0; Slice < TextureCubeFaceCount; ++Slice)
		{
			for (uint32 MipIndex = 0; MipIndex < MipCount; ++MipIndex, ++RecordIndex)
			{
				const FTexture2DMipData& Mip = PlatformData.Faces[Slice].Mips[MipIndex];
				Body.WriteU32(Slice);
				Body.WriteU32(MipIndex);
				Body.WriteU32(Mip.Width);
				Body.WriteU32(Mip.Height);
				Body.WriteU32(Mip.RowPitch);
				Body.WriteU32(0);
				Body.WriteU64(DataOffsets[RecordIndex]);
				Body.WriteU64(Mip.Pixels.size());
			}
		}
		uint64 CurrentOffset = TexturePayloadHeaderSize + Body.GetBytes().size();
		RecordIndex = 0;
		for (const FTexturePlatformData& Face : PlatformData.Faces)
		{
			for (const FTexture2DMipData& Mip : Face.Mips)
			{
				std::vector<uint8> Padding(
					static_cast<size_t>(DataOffsets[RecordIndex] - CurrentOffset), 0);
				Body.WriteBytes(Padding);
				Body.WriteBytes(Mip.Pixels);
				CurrentOffset = DataOffsets[RecordIndex] + Mip.Pixels.size();
				++RecordIndex;
			}
		}
		const std::vector<uint8> BodyBytes = Body.TakeBytes();

		FBinaryWriter Result;
		Result.WriteU32(TexturePayloadMagic);
		Result.WriteU32(TexturePayloadSchemaVersion);
		Result.WriteU32(TextureCubeBuilderVersion);
		Result.WriteU32(static_cast<uint32>(TargetPlatform));
		Result.WriteU32(static_cast<uint32>(TargetProfile));
		Result.WriteU32(static_cast<uint32>(ETexturePayloadDimension::TextureCube));
		Result.WriteU32(static_cast<uint32>(StableFormat));
		Result.WriteU32(TextureCubeFaceCount);
		Result.WriteU32(MipCount);
		Result.WriteU32(TexturePayloadHeaderSize);
		Result.WriteU32(RecordCount);
		Result.WriteU32(TexturePayloadRecordSize);
		Result.WriteU64(TexturePayloadHeaderSize);
		Result.WriteU64(DataOffset);
		Result.WriteU64(FXxHash64::HashBuffer(BodyBytes).HashValue);
		Result.WriteU64(0);
		Result.WriteBytes(BodyBytes);
		OutBytes = Result.TakeBytes();
		return true;
	}

	auto ParseTextureCubeSerializedValue(
		std::span<const uint8> Bytes,
		Asset::ECookTargetPlatform ExpectedPlatform,
		Asset::ECookTargetProfile ExpectedProfile,
		std::unique_ptr<FTextureCubePlatformData>& OutPlatformData,
		std::string& OutError,
		EPayloadDecodeError& OutCode) -> bool
	{
		OutError.clear();
		OutCode = EPayloadDecodeError::Corrupt;
		if (!IsSupportedTarget(ExpectedPlatform, ExpectedProfile))
		{
			OutCode = EPayloadDecodeError::Incompatible;
			return Fail(OutError, "Texture payload expected target is unsupported.");
		}
		if (Bytes.size() < TexturePayloadHeaderSize)
			return Fail(OutError, "Texture payload header is truncated.");

		uint32 Magic = 0;
		uint32 SchemaVersion = 0;
		uint32 BuilderVersion = 0;
		uint32 Platform = 0;
		uint32 Profile = 0;
		uint32 Dimension = 0;
		uint32 StableFormat = 0;
		uint32 SliceCount = 0;
		uint32 MipCount = 0;
		uint32 HeaderSize = 0;
		uint32 RecordCount = 0;
		uint32 RecordSize = 0;
		uint64 RecordTableOffset = 0;
		uint64 StoredSize = 0;
		uint64 StoredHash = 0;
		uint64 Reserved = 0;
		if (!ReadLittleEndianAt(Bytes, 0, Magic) || !ReadLittleEndianAt(Bytes, 4, SchemaVersion)
			|| !ReadLittleEndianAt(Bytes, 8, BuilderVersion) || !ReadLittleEndianAt(Bytes, 12, Platform)
			|| !ReadLittleEndianAt(Bytes, 16, Profile) || !ReadLittleEndianAt(Bytes, 20, Dimension)
			|| !ReadLittleEndianAt(Bytes, 24, StableFormat) || !ReadLittleEndianAt(Bytes, 28, SliceCount)
			|| !ReadLittleEndianAt(Bytes, 32, MipCount) || !ReadLittleEndianAt(Bytes, 36, HeaderSize)
			|| !ReadLittleEndianAt(Bytes, 40, RecordCount) || !ReadLittleEndianAt(Bytes, 44, RecordSize)
			|| !ReadLittleEndianAt(Bytes, 48, RecordTableOffset) || !ReadLittleEndianAt(Bytes, 56, StoredSize)
			|| !ReadLittleEndianAt(Bytes, 64, StoredHash) || !ReadLittleEndianAt(Bytes, 72, Reserved))
			return Fail(OutError, "Texture payload header is truncated.");
		if (Magic != TexturePayloadMagic) return Fail(OutError, "Texture payload magic is invalid.");
		if (SchemaVersion != TexturePayloadSchemaVersion)
		{
			OutCode = EPayloadDecodeError::Incompatible;
			return Fail(OutError, "Texture payload schema version is unsupported.");
		}
		if (BuilderVersion != TextureCubeBuilderVersion)
		{
			OutCode = EPayloadDecodeError::Incompatible;
			return Fail(OutError, "TextureCube payload builder version is unsupported.");
		}
		if (Platform != static_cast<uint32>(ExpectedPlatform)
			|| Profile != static_cast<uint32>(ExpectedProfile))
			return Fail(OutError, "Texture payload target platform or profile does not match.");
		if (Dimension != static_cast<uint32>(ETexturePayloadDimension::TextureCube)
			|| SliceCount != TextureCubeFaceCount || MipCount == 0
			|| MipCount > MaximumTextureMipCount
			|| RecordCount != SliceCount * MipCount
			|| HeaderSize != TexturePayloadHeaderSize
			|| RecordSize != TexturePayloadRecordSize
			|| RecordTableOffset != TexturePayloadHeaderSize
			|| Reserved != 0)
			return Fail(OutError, "TextureCube payload header layout or counts are invalid.");
		if (StoredSize != Bytes.size() || StoredSize > MaximumTexturePayloadBytes)
			return Fail(OutError, "Texture payload stored size is invalid.");
		if (FXxHash64::HashBuffer(Bytes.subspan(TexturePayloadHeaderSize)).HashValue != StoredHash)
			return Fail(OutError, "Texture payload checksum does not match.");

		EPixelFormat PixelFormat = EPixelFormat::Unknown;
		if (!FromStablePixelFormat(StableFormat, PixelFormat))
		{
			OutCode = EPayloadDecodeError::Incompatible;
			return Fail(OutError, "Texture payload pixel format identifier is unsupported.");
		}
		const uint64 TableEnd =
			RecordTableOffset + static_cast<uint64>(RecordCount) * RecordSize;
		if (TableEnd < RecordTableOffset || TableEnd > StoredSize)
			return Fail(OutError, "Texture payload record table is outside the stored object.");

		auto Candidate = std::make_unique<FTextureCubePlatformData>();
		Candidate->PixelFormat = PixelFormat;
		uint64 PreviousEnd = TableEnd;
		for (uint32 RecordIndex = 0; RecordIndex < RecordCount; ++RecordIndex)
		{
			const uint32 ExpectedSlice = RecordIndex / MipCount;
			const uint32 ExpectedMip = RecordIndex % MipCount;
			const size_t Offset = static_cast<size_t>(
				RecordTableOffset + static_cast<uint64>(RecordIndex) * RecordSize);
			uint32 Slice = 0;
			uint32 MipIndex = 0;
			uint32 Width = 0;
			uint32 Height = 0;
			uint32 RowPitch = 0;
			uint32 RecordReserved = 0;
			uint64 DataOffset = 0;
			uint64 ByteCount = 0;
			if (!ReadLittleEndianAt(Bytes, Offset, Slice) || !ReadLittleEndianAt(Bytes, Offset + 4, MipIndex)
				|| !ReadLittleEndianAt(Bytes, Offset + 8, Width) || !ReadLittleEndianAt(Bytes, Offset + 12, Height)
				|| !ReadLittleEndianAt(Bytes, Offset + 16, RowPitch)
				|| !ReadLittleEndianAt(Bytes, Offset + 20, RecordReserved)
				|| !ReadLittleEndianAt(Bytes, Offset + 24, DataOffset)
				|| !ReadLittleEndianAt(Bytes, Offset + 32, ByteCount))
				return Fail(OutError, "Texture payload subresource record is truncated.");
			if (Slice != ExpectedSlice || MipIndex != ExpectedMip || RecordReserved != 0
				|| Width == 0 || Height == 0 || Width != Height
				|| Width > MaximumTextureCubeDimension)
				return Fail(OutError, "TextureCube payload subresource identity or dimensions are invalid.");

			FTexturePlatformData& Face = Candidate->Faces[Slice];
			Face.PixelFormat = PixelFormat;
			if (MipIndex > 0)
			{
				const FTexture2DMipData& PreviousMip = Face.Mips.back();
				if (Width != std::max(PreviousMip.Width / 2, 1u)
					|| Height != std::max(PreviousMip.Height / 2, 1u))
					return Fail(OutError, "TextureCube payload mip dimensions are not a complete progression.");
			}
			else if (Slice > 0)
			{
				const FTexture2DMipData& Reference = Candidate->Faces[0].Mips[0];
				if (Width != Reference.Width || Height != Reference.Height)
					return Fail(OutError, "TextureCube payload face dimensions do not match.");
			}
			const FPixelFormatLayout Layout = GetPixelFormatLayout(PixelFormat, Width, Height);
			if (RowPitch != Layout.RowPitch || ByteCount != Layout.DataSize)
				return Fail(OutError, "Texture payload subresource layout does not match its format.");
			if (Slice > 0)
			{
				const FTexture2DMipData& Reference = Candidate->Faces[0].Mips[MipIndex];
				if (Width != Reference.Width || Height != Reference.Height
					|| RowPitch != Reference.RowPitch || ByteCount != Reference.Pixels.size())
					return Fail(OutError, "TextureCube payload faces have incompatible mip layouts.");
			}
			if (DataOffset % TexturePayloadAlignment != 0 || DataOffset < PreviousEnd
				|| DataOffset > StoredSize || ByteCount > StoredSize - DataOffset)
				return Fail(OutError,
					"Texture payload subresource range is misaligned, overlapping, or outside the object.");
			for (uint64 PaddingOffset = PreviousEnd; PaddingOffset < DataOffset; ++PaddingOffset)
				if (Bytes[static_cast<size_t>(PaddingOffset)] != 0)
					return Fail(OutError, "Texture payload contains non-zero alignment padding.");

			FTexture2DMipData& Mip = Face.Mips.emplace_back();
			Mip.Width = Width;
			Mip.Height = Height;
			Mip.RowPitch = RowPitch;
			Mip.Pixels.assign(
				Bytes.begin() + static_cast<size_t>(DataOffset),
				Bytes.begin() + static_cast<size_t>(DataOffset + ByteCount));
			PreviousEnd = DataOffset + ByteCount;
		}
		if (PreviousEnd != StoredSize)
			return Fail(OutError, "Texture payload contains trailing data.");
		if (!IsCompleteCubeMipChain(*Candidate))
			return Fail(OutError, "TextureCube payload mip chains are incomplete or invalid.");
		OutPlatformData = std::move(Candidate);
		return true;
	}

	auto FTextureCubePlatformData::Serialize(
		FArchive& Ar,
		const FTexturePlatformSerializationContext& Context) -> void
	{
		if (Ar.HasError()) return;
		if (Ar.IsSaving())
		{
			std::vector<uint8> Bytes;
			std::string Error;
			if (!BuildTextureCubeSerializedValue(
				*this, Context.TargetPlatform, Context.TargetProfile, Bytes, Error))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, Error);
				return;
			}
			Ar.WriteBytes(std::as_bytes(std::span<const uint8>(Bytes)));
			return;
		}

		const uint64 ByteCount = Ar.GetRemainingPayloadBytes();
		if (ByteCount == std::numeric_limits<uint64>::max())
		{
			Ar.Fail(EArchiveFailureCode::UnsupportedCapability,
				"TextureCube platform data requires a bounded input archive.");
			return;
		}
		if (ByteCount > MaximumTexturePayloadBytes
			|| ByteCount > static_cast<uint64>(std::vector<uint8>().max_size()))
		{
			Ar.Fail(EArchiveFailureCode::LimitExceeded,
				"TextureCube platform data exceeds its stored-size limit.");
			return;
		}
		std::vector<uint8> Bytes(static_cast<size_t>(ByteCount));
		Ar.ReadBytes(std::as_writable_bytes(std::span<uint8>(Bytes)));
		if (Ar.HasError()) return;

		std::unique_ptr<FTextureCubePlatformData> Candidate;
		std::string Error;
		EPayloadDecodeError Code = EPayloadDecodeError::Corrupt;
		if (!ParseTextureCubeSerializedValue(
			Bytes, Context.TargetPlatform, Context.TargetProfile, Candidate, Error, Code))
		{
			Ar.Fail(Code == EPayloadDecodeError::Incompatible
				? EArchiveFailureCode::UnsupportedVersion : EArchiveFailureCode::InvalidData,
				Error);
			return;
		}
		*this = std::move(*Candidate);
	}
}
