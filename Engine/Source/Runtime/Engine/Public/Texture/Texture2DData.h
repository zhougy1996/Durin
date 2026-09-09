#pragma once

#include "Asset/CookedAsset.h"
#include "EngineAPI.h"
#include "DObject/DObjectFwd.h"
#include "DObject/ObjectMacros.h"
#include "PixelFormat.h"
#include "Texture/TextureSourceFormat.h"
#include "Image/Image.h"

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

	// Owns one tightly described platform mip and its byte row pitch.
	struct FTexture2DMipData
	{
		FByteBuffer Pixels;
		uint32 Width = 0;
		uint32 Height = 0;
		uint32 RowPitch = 0;

		ENGINE_API auto IsValid(EPixelFormat PixelFormat) const -> bool;
	};

	// Owns the pixel format and complete mip chain consumed by the render resource.
	struct FTexturePlatformData
	{
		std::vector<FTexture2DMipData> Mips;
		EPixelFormat PixelFormat = EPixelFormat::Unknown;

		ENGINE_API auto IsValid() const -> bool;
		// Loads canonical TXPL in place; discard failures and check the owning byte boundary.
		ENGINE_API auto Serialize(
			FArchive& Ar) -> void;
	};

}
