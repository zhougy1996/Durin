#include "Texture/TextureDerivedData.h"

#include "Hash/XxHash.h"
#include "Serialization/Archive.h"
#include "Serialization/BinaryFormat.h"
#include "Serialization/EngineWire.h"
#include "Texture/VolumeTexture.h"

namespace Durin
{
	namespace
	{
		auto FailVolume(std::string& OutError, std::string Message) -> bool
		{
			OutError = std::move(Message);
			return false;
		}

		auto IsVolumeTargetSupported(Asset::ECookTargetPlatform Platform,
			Asset::ECookTargetProfile Profile) -> bool
		{
			return Platform == Asset::ECookTargetPlatform::Win64
				&& (Profile == Asset::ECookTargetProfile::Game
					|| Profile == Asset::ECookTargetProfile::EditorValidation);
		}

		auto ToVolumeStableFormat(EPixelFormat Format,
			ETextureStablePixelFormat& OutFormat) -> bool
		{
			switch (Format)
			{
			case EPixelFormat::R8_UNORM: OutFormat = ETextureStablePixelFormat::R8_UNORM; return true;
			case EPixelFormat::RG8_UNORM: OutFormat = ETextureStablePixelFormat::RG8_UNORM; return true;
			case EPixelFormat::RGBA8_UNORM: OutFormat = ETextureStablePixelFormat::RGBA8_UNORM; return true;
			case EPixelFormat::R16_FLOAT: OutFormat = ETextureStablePixelFormat::R16_FLOAT; return true;
			case EPixelFormat::RGBA16_FLOAT: OutFormat = ETextureStablePixelFormat::RGBA16_FLOAT; return true;
			default: return false;
			}
		}

		auto FromVolumeStableFormat(uint32 StableFormat, EPixelFormat& OutFormat) -> bool
		{
			switch (static_cast<ETextureStablePixelFormat>(StableFormat))
			{
			case ETextureStablePixelFormat::R8_UNORM: OutFormat = EPixelFormat::R8_UNORM; return true;
			case ETextureStablePixelFormat::RG8_UNORM: OutFormat = EPixelFormat::RG8_UNORM; return true;
			case ETextureStablePixelFormat::RGBA8_UNORM: OutFormat = EPixelFormat::RGBA8_UNORM; return true;
			case ETextureStablePixelFormat::R16_FLOAT: OutFormat = EPixelFormat::R16_FLOAT; return true;
			case ETextureStablePixelFormat::RGBA16_FLOAT: OutFormat = EPixelFormat::RGBA16_FLOAT; return true;
			default: return false;
			}
		}
	}

	auto BuildVolumeTextureSerializedValue(
		const FVolumeTexturePlatformData& PlatformData,
		Asset::ECookTargetPlatform TargetPlatform,
		Asset::ECookTargetProfile TargetProfile,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool
	{
		OutBytes.clear();
		if (!IsVolumeTargetSupported(TargetPlatform, TargetProfile))
			return FailVolume(OutError, "Volume texture payload target is unsupported.");
		if (!PlatformData.IsValid() || PlatformData.Mips.size() > MaximumTextureMipCount)
			return FailVolume(OutError, "Volume texture payload requires a valid complete mip chain.");
		ETextureStablePixelFormat StableFormat;
		if (!ToVolumeStableFormat(PlatformData.PixelFormat, StableFormat))
			return FailVolume(OutError, "Volume texture format has no stable identifier.");

		const uint32 RecordCount = static_cast<uint32>(PlatformData.Mips.size());
		uint64 DataOffset = EngineWire::AlignUp(TexturePayloadHeaderSize
			+ static_cast<uint64>(RecordCount) * TexturePayloadRecordSize,
			TexturePayloadAlignment);
		std::vector<uint64> DataOffsets;
		for (const FVolumeTextureMipData& Mip : PlatformData.Mips)
		{
			DataOffsets.push_back(DataOffset);
			if (DataOffset > MaximumTexturePayloadBytes
				|| Mip.Voxels.size() > MaximumTexturePayloadBytes - DataOffset)
				return FailVolume(OutError, "Volume texture payload exceeds its byte limit.");
			DataOffset += Mip.Voxels.size();
			if (&Mip != &PlatformData.Mips.back())
				DataOffset = EngineWire::AlignUp(DataOffset, TexturePayloadAlignment);
		}

		FBinaryWriter Body;
		for (uint32 MipIndex = 0; MipIndex < RecordCount; ++MipIndex)
		{
			const FVolumeTextureMipData& Mip = PlatformData.Mips[MipIndex];
			Body.WriteU32(Mip.Depth);
			Body.WriteU32(MipIndex);
			Body.WriteU32(Mip.Width);
			Body.WriteU32(Mip.Height);
			Body.WriteU32(Mip.RowPitch);
			Body.WriteU32(Mip.DepthPitch);
			Body.WriteU64(DataOffsets[MipIndex]);
			Body.WriteU64(Mip.Voxels.size());
		}
		uint64 CurrentOffset = TexturePayloadHeaderSize + Body.GetBytes().size();
		for (uint32 MipIndex = 0; MipIndex < RecordCount; ++MipIndex)
		{
			const FVolumeTextureMipData& Mip = PlatformData.Mips[MipIndex];
			Body.WriteBytes(std::vector<uint8>(
				static_cast<size_t>(DataOffsets[MipIndex] - CurrentOffset), 0));
			Body.WriteBytes(Mip.Voxels);
			CurrentOffset = DataOffsets[MipIndex] + Mip.Voxels.size();
		}
		const std::vector<uint8> BodyBytes = Body.TakeBytes();
		FBinaryWriter Result;
		Result.WriteU32(TexturePayloadMagic);
		Result.WriteU32(TexturePayloadSchemaVersion);
		Result.WriteU32(VolumeTextureBuilderVersion);
		Result.WriteU32(static_cast<uint32>(TargetPlatform));
		Result.WriteU32(static_cast<uint32>(TargetProfile));
		Result.WriteU32(static_cast<uint32>(ETexturePayloadDimension::Texture3D));
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
		OutError.clear();
		return true;
	}

	auto ParseVolumeTextureSerializedValue(
		std::span<const uint8> Bytes,
		Asset::ECookTargetPlatform ExpectedPlatform,
		Asset::ECookTargetProfile ExpectedProfile,
		std::unique_ptr<FVolumeTexturePlatformData>& OutPlatformData,
		std::string& OutError,
		EPayloadDecodeError& OutCode) -> bool
	{
		using EngineWire::ReadLittleEndianAt;
		OutPlatformData.reset();
		OutCode = EPayloadDecodeError::Corrupt;
		if (!IsVolumeTargetSupported(ExpectedPlatform, ExpectedProfile))
		{
			OutCode = EPayloadDecodeError::Incompatible;
			return FailVolume(OutError, "Volume texture expected target is unsupported.");
		}
		if (Bytes.size() < TexturePayloadHeaderSize)
			return FailVolume(OutError, "Volume texture payload header is truncated.");
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
			return FailVolume(OutError, "Volume texture payload header is truncated.");
		if (Magic != TexturePayloadMagic)
			return FailVolume(OutError, "Volume texture payload magic is invalid.");
		if (Schema != TexturePayloadSchemaVersion || Producer != VolumeTextureBuilderVersion)
		{
			OutCode = EPayloadDecodeError::Incompatible;
			return FailVolume(OutError, "Volume texture payload version is unsupported.");
		}
		if (Platform != static_cast<uint32>(ExpectedPlatform)
			|| Profile != static_cast<uint32>(ExpectedProfile))
			return FailVolume(OutError, "Volume texture payload target does not match.");
		if (Dimension != static_cast<uint32>(ETexturePayloadDimension::Texture3D)
			|| SliceCount != 1 || MipCount == 0 || MipCount > MaximumTextureMipCount
			|| RecordCount != MipCount || HeaderSize != TexturePayloadHeaderSize
			|| RecordSize != TexturePayloadRecordSize || TableOffset != TexturePayloadHeaderSize
			|| Reserved != 0)
			return FailVolume(OutError, "Volume texture payload header layout is invalid.");
		if (StoredSize != Bytes.size() || StoredSize > MaximumTexturePayloadBytes
			|| FXxHash64::HashBuffer(Bytes.subspan(TexturePayloadHeaderSize)).HashValue != StoredHash)
			return FailVolume(OutError, "Volume texture payload size or checksum is invalid.");
		EPixelFormat PixelFormat = EPixelFormat::Unknown;
		if (!FromVolumeStableFormat(StableFormat, PixelFormat))
		{
			OutCode = EPayloadDecodeError::Incompatible;
			return FailVolume(OutError, "Volume texture stable format is unsupported.");
		}
		const uint64 TableEnd = TableOffset + static_cast<uint64>(RecordCount) * RecordSize;
		if (TableEnd < TableOffset || TableEnd > StoredSize)
			return FailVolume(OutError, "Volume texture record table is outside the payload.");

		auto Candidate = std::make_unique<FVolumeTexturePlatformData>();
		Candidate->PixelFormat = PixelFormat;
		uint64 PreviousEnd = TableEnd;
		for (uint32 MipIndex = 0; MipIndex < MipCount; ++MipIndex)
		{
			const size_t Offset = static_cast<size_t>(TableOffset
				+ static_cast<uint64>(MipIndex) * RecordSize);
			uint32 Depth = 0, StoredMip = 0, Width = 0, Height = 0;
			uint32 RowPitch = 0, DepthPitch = 0;
			uint64 DataOffset = 0, ByteCount = 0;
			if (!ReadLittleEndianAt(Bytes, Offset, Depth)
				|| !ReadLittleEndianAt(Bytes, Offset + 4, StoredMip)
				|| !ReadLittleEndianAt(Bytes, Offset + 8, Width)
				|| !ReadLittleEndianAt(Bytes, Offset + 12, Height)
				|| !ReadLittleEndianAt(Bytes, Offset + 16, RowPitch)
				|| !ReadLittleEndianAt(Bytes, Offset + 20, DepthPitch)
				|| !ReadLittleEndianAt(Bytes, Offset + 24, DataOffset)
				|| !ReadLittleEndianAt(Bytes, Offset + 32, ByteCount))
				return FailVolume(OutError, "Volume texture mip record is truncated.");
			if (StoredMip != MipIndex || Width == 0 || Height == 0 || Depth == 0
				|| Width > MaximumVolumeTextureDimension
				|| Height > MaximumVolumeTextureDimension
				|| Depth > MaximumVolumeTextureDimension)
				return FailVolume(OutError, "Volume texture mip identity or dimensions are invalid.");
			if (MipIndex > 0)
			{
				const FVolumeTextureMipData& Previous = Candidate->Mips.back();
				if (Width != std::max(1u, Previous.Width / 2)
					|| Height != std::max(1u, Previous.Height / 2)
					|| Depth != std::max(1u, Previous.Depth / 2))
					return FailVolume(OutError, "Volume texture mip progression is invalid.");
			}
			const FPixelFormatLayout Slice = GetPixelFormatLayout(PixelFormat, Width, Height);
			if (Slice.RowPitch != RowPitch || Slice.DataSize != DepthPitch
				|| Depth > std::numeric_limits<uint64>::max() / DepthPitch
				|| ByteCount != static_cast<uint64>(DepthPitch) * Depth)
				return FailVolume(OutError, "Volume texture mip pitches do not match its format.");
			if (DataOffset % TexturePayloadAlignment != 0 || DataOffset < PreviousEnd
				|| DataOffset > StoredSize || ByteCount > StoredSize - DataOffset)
				return FailVolume(OutError, "Volume texture mip range is invalid.");
			for (uint64 Padding = PreviousEnd; Padding < DataOffset; ++Padding)
				if (Bytes[static_cast<size_t>(Padding)] != 0)
					return FailVolume(OutError, "Volume texture alignment padding is nonzero.");
			FVolumeTextureMipData& Mip = Candidate->Mips.emplace_back();
			Mip.Width = Width;
			Mip.Height = Height;
			Mip.Depth = Depth;
			Mip.RowPitch = RowPitch;
			Mip.DepthPitch = DepthPitch;
			Mip.Voxels.assign(Bytes.begin() + static_cast<size_t>(DataOffset),
				Bytes.begin() + static_cast<size_t>(DataOffset + ByteCount));
			PreviousEnd = DataOffset + ByteCount;
		}
		if (PreviousEnd != StoredSize || !Candidate->IsValid())
			return FailVolume(OutError, "Volume texture payload is incomplete or has trailing data.");
		OutPlatformData = std::move(Candidate);
		OutError.clear();
		return true;
	}

	auto FVolumeTexturePlatformData::Serialize(FArchive& Ar,
		const FTexturePlatformSerializationContext& Context) -> void
	{
		if (Ar.HasError()) return;
		if (Ar.IsSaving())
		{
			std::vector<uint8> Bytes;
			std::string Error;
			if (!BuildVolumeTextureSerializedValue(*this, Context.TargetPlatform,
				Context.TargetProfile, Bytes, Error))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, Error);
				return;
			}
			Ar.WriteBytes(std::as_bytes(std::span<const uint8>(Bytes)));
			return;
		}
		const uint64 ByteCount = Ar.GetRemainingPayloadBytes();
		if (ByteCount == std::numeric_limits<uint64>::max()
			|| ByteCount > MaximumTexturePayloadBytes
			|| ByteCount > static_cast<uint64>(std::vector<uint8>().max_size()))
		{
			Ar.Fail(EArchiveFailureCode::LimitExceeded,
				"Volume texture platform data requires a bounded payload.");
			return;
		}
		std::vector<uint8> Bytes(static_cast<size_t>(ByteCount));
		Ar.ReadBytes(std::as_writable_bytes(std::span<uint8>(Bytes)));
		if (Ar.HasError()) return;
		std::unique_ptr<FVolumeTexturePlatformData> Candidate;
		std::string Error;
		EPayloadDecodeError Code = EPayloadDecodeError::Corrupt;
		if (!ParseVolumeTextureSerializedValue(Bytes, Context.TargetPlatform,
			Context.TargetProfile, Candidate, Error, Code))
		{
			Ar.Fail(Code == EPayloadDecodeError::Incompatible
				? EArchiveFailureCode::UnsupportedVersion
				: EArchiveFailureCode::InvalidData, Error);
			return;
		}
		*this = std::move(*Candidate);
	}
}
