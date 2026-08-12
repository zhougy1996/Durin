#include "Texture/TextureDerivedDataWriter.h"

#include "Misc/DerivedDataCache.h"

namespace Durin::AssetBuild::TextureDerivedDataWriter
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

		auto AlignPayloadOffset(uint64 Offset) -> uint64
		{
			return (Offset + TexturePayloadAlignment - 1)
				& ~(static_cast<uint64>(TexturePayloadAlignment) - 1);
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

	}
	auto BuildTexture2DDerivedDataKeyBytes(
		const FTexture2DDerivedDataKeyInput& Input) -> std::vector<uint8>
	{
		DerivedDataCache::FWriter Writer;
		Writer.WriteU32(TextureDerivedDataKeySchemaVersion);
		Writer.WriteU32(static_cast<uint32>(ETexturePayloadDimension::Texture2D));
		Writer.WriteU64(Input.SourceContentHash.HashLow);
		Writer.WriteU64(Input.SourceContentHash.HashHigh);
		Writer.WriteU8(static_cast<uint8>(Input.Usage));
		Writer.WriteU8(Input.bSRGB ? 1 : 0);
		Writer.WriteU8(static_cast<uint8>(Input.CompressionQuality));
		Writer.WriteU8(static_cast<uint8>(Input.AlphaMipMode));
		Writer.WriteU32(Input.MaximumResolution);
		Writer.WriteU32(std::bit_cast<uint32>(Input.AlphaCoverageThreshold));
		Writer.WriteU32(Input.BuilderVersion);
		Writer.WriteU32(Input.PayloadSchemaVersion);
		Writer.WriteU32(static_cast<uint32>(Input.TargetPlatform));
		Writer.WriteU32(static_cast<uint32>(Input.TargetProfile));
		return Writer.TakeBytes();
	}

	auto BuildTexture2DDerivedDataKey(
		const FTexture2DDerivedDataKeyInput& Input) -> std::string
	{
		return FXxHash128::HashBuffer(
			TextureDerivedDataWriter::BuildTexture2DDerivedDataKeyBytes(Input)).ToString();
	}

	auto BuildTextureCubeDerivedDataKeyBytes(
		const FTextureCubeDerivedDataKeyInput& Input,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool
	{
		OutBytes.clear();
		OutError.clear();
		if (!IsSupportedTarget(Input.TargetPlatform, Input.TargetProfile))
			return Fail(OutError, "TextureCube derived-data target is unsupported.");
		if (!std::isfinite(Input.ExposureEV)
			|| std::bit_cast<uint32>(Input.ExposureEV) == 0x80000000u
			|| Input.ExposureEV < -32.0f || Input.ExposureEV > 32.0f)
			return Fail(OutError, "TextureCube panorama exposure is not canonical.");
		if (Input.FaceDimension > MaximumTextureCubeDimension)
			return Fail(OutError, "TextureCube requested face dimension exceeds the supported limit.");

		DerivedDataCache::FWriter Writer;
		Writer.WriteU32(TextureDerivedDataKeySchemaVersion);
		Writer.WriteU32(static_cast<uint32>(ETexturePayloadDimension::TextureCube));
		Writer.WriteU32(static_cast<uint32>(Input.SourceLayout));
		switch (Input.SourceLayout)
		{
		case ETextureCubeDerivedDataSourceLayout::SixFaces:
			for (const FXxHash128& Hash : Input.FaceContentHashes)
			{
				Writer.WriteU64(Hash.HashLow);
				Writer.WriteU64(Hash.HashHigh);
			}
			Writer.WriteU8(Input.bSRGB ? 1 : 0);
			break;
		case ETextureCubeDerivedDataSourceLayout::EquirectangularPanorama:
			Writer.WriteU64(Input.PanoramaContentHash.HashLow);
			Writer.WriteU64(Input.PanoramaContentHash.HashHigh);
			Writer.WriteU32(Input.FaceDimension);
			Writer.WriteU32(std::bit_cast<uint32>(Input.ExposureEV));
			Writer.WriteU8(Input.bSRGB ? 1 : 0);
			break;
		default:
			return Fail(OutError, "TextureCube source layout is unsupported.");
		}
		Writer.WriteU32(Input.BuilderVersion);
		Writer.WriteU32(Input.PayloadSchemaVersion);
		Writer.WriteU32(Input.ProjectionVersion);
		Writer.WriteU32(static_cast<uint32>(Input.TargetPlatform));
		Writer.WriteU32(static_cast<uint32>(Input.TargetProfile));
		OutBytes = Writer.TakeBytes();
		return true;
	}

	auto BuildTextureCubeDerivedDataKey(
		const FTextureCubeDerivedDataKeyInput& Input,
		std::string& OutKey,
		std::string& OutError) -> bool
	{
		std::vector<uint8> Bytes;
		if (!TextureDerivedDataWriter::BuildTextureCubeDerivedDataKeyBytes(
			Input, Bytes, OutError)) return false;
		OutKey = FXxHash128::HashBuffer(Bytes).ToString();
		return true;
	}

	auto EncodeTexture2DPayload(
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

		DerivedDataCache::FWriter Body;
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

		DerivedDataCache::FWriter Result;
		Result.WriteU32(TexturePayloadMagic);
		Result.WriteU32(TexturePayloadSchemaVersion);
		Result.WriteU32(Texture2DBuilderVersion);
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

	auto EncodeTextureCubePayload(
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

		DerivedDataCache::FWriter Body;
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

		DerivedDataCache::FWriter Result;
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

}
