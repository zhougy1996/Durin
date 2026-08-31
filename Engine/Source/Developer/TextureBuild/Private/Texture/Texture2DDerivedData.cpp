#include "Texture/Texture2DDerivedData.h"

#include "Serialization/Archive.h"

namespace Durin::Asset
{
	auto FTexture2DBuildKeyInput::Serialize(FArchive& Ar) -> void
	{
		uint32 KeySchemaVersion = TextureDerivedDataKeySchemaVersion;
		uint32 Dimension = static_cast<uint32>(ETexturePayloadDimension::Texture2D);
		uint8 EncodedUsage = static_cast<uint8>(Usage);
		uint8 EncodedSRGB = bSRGB ? 1 : 0;
		uint8 EncodedCompressionQuality = static_cast<uint8>(CompressionQuality);
		uint8 EncodedAlphaMipMode = static_cast<uint8>(AlphaMipMode);
		uint32 EncodedAlphaCoverageThreshold = std::bit_cast<uint32>(AlphaCoverageThreshold);
		uint32 EncodedTargetPlatform = static_cast<uint32>(TargetPlatform);
		uint32 EncodedTargetProfile = static_cast<uint32>(TargetProfile);
		Ar << KeySchemaVersion << Dimension
			<< SourceContentHash.HashLow << SourceContentHash.HashHigh
			<< EncodedUsage << EncodedSRGB << EncodedCompressionQuality << EncodedAlphaMipMode
			<< MaximumResolution << EncodedAlphaCoverageThreshold
			<< BuilderVersion << PayloadSchemaVersion
			<< EncodedTargetPlatform << EncodedTargetProfile;
		if (Ar.IsLoading())
			Ar.Fail(EArchiveFailureCode::UnsupportedCapability,
				"Texture2D build-key input is save-only.");
	}

	auto BuildTexture2DDerivedDataKeyBytes(
		const FTexture2DBuildKeyInput& Input) -> FByteArray
	{
		FByteArray Bytes;
		FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataKey);
		const_cast<FTexture2DBuildKeyInput&>(Input).Serialize(Ar);
		return Bytes;
	}

	auto BuildTexture2DDerivedDataKey(
		const FTexture2DBuildKeyInput& Input) -> std::string
	{
		return FXxHash128::HashBuffer(BuildTexture2DDerivedDataKeyBytes(Input)).ToString();
	}
}
