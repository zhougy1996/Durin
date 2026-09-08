#pragma once

#include "Asset/CookedAsset.h"
#include "EngineAPI.h"
#include "DObject/DObjectFwd.h"
#include "DObject/ObjectMacros.h"
#include "PixelFormat.h"
#include "Texture/TextureSourceFormat.h"

#include "Texture2DData.gen.h"

namespace Durin
{
	class FArchive;

	// Selects semantic color handling and platform-build defaults for a texture.
	DENUM()
	enum class ETextureUsage : uint8
	{
		Color,
		Normal,
		DataMask DMETA(DisplayName = "Data / Mask")
	};

	// Controls the offline desktop block-compression search effort.
	DENUM(DisplayName = "Texture Compression Quality")
	enum class ETextureCompressionQuality : uint8
	{
		Low,
		Normal,
		High
	};

	// Controls how Color texture alpha is filtered into smaller mip levels.
	DENUM(DisplayName = "Texture Alpha Mip Mode")
	enum class ETextureAlphaMipMode : uint8
	{
		Average,
		PreserveCoverage DMETA(DisplayName = "Preserve Coverage")
	};

	ENGINE_API auto IsValidTextureUsage(ETextureUsage Usage) -> bool;
	ENGINE_API auto GetDefaultTextureSRGB(ETextureUsage Usage) -> bool;
	ENGINE_API auto IsValidTextureCompressionQuality(
		ETextureCompressionQuality Quality) -> bool;
	ENGINE_API auto IsValidTextureAlphaMipMode(ETextureAlphaMipMode Mode) -> bool;
	ENGINE_API auto IsValidTextureAlphaCoverageThreshold(float Threshold) -> bool;

	// Owns decoded source pixels before platform-specific conversion.
	struct FTextureSourceData
	{
		FByteBuffer Pixels;
		uint32 Width = 0;
		uint32 Height = 0;
		uint8 SourceChannelCount = 0;
		ETextureSourceFormat Format = ETextureSourceFormat::Invalid;
		bool bHasTransparency = false;

		ENGINE_API auto IsValid() const -> bool;
	};

	// Owns one tightly described platform mip and its byte row pitch.
	struct FTexture2DMipData
	{
		FByteBuffer Pixels;
		uint32 Width = 0;
		uint32 Height = 0;
		uint32 RowPitch = 0;

		ENGINE_API auto IsValid(EPixelFormat PixelFormat) const -> bool;
	};

	// Supplies the stable target identity carried by a serialized texture platform value.
	struct FTexturePlatformSerializationContext
	{
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
	};

	// Owns the pixel format and complete mip chain consumed by the render resource.
	struct FTexturePlatformData
	{
		std::vector<FTexture2DMipData> Mips;
		EPixelFormat PixelFormat = EPixelFormat::Unknown;

		ENGINE_API auto IsValid() const -> bool;
		// Serializes the canonical TXPL value for DDC and cooked payload boundaries.
		ENGINE_API auto Serialize(
			FArchive& Ar,
			const FTexturePlatformSerializationContext& Context) -> void;
	};

}
