#pragma once

#include "EngineAPI.h"
#include "Texture/Texture2D.h"

namespace Durin::TextureBuild
{
	inline constexpr uint32 ChannelCount = 4;
	inline constexpr uint32 MaxDimension = 16384;

	ENGINE_API auto IsValidUsage(ETextureUsage Usage) -> bool;
	ENGINE_API auto GetDefaultSRGB(ETextureUsage Usage) -> bool;
	ENGINE_API auto SelectPixelFormat(ETextureUsage Usage, bool bSRGB, bool bHasTransparency) -> EPixelFormat;

	// Decodes a supported source image into the canonical top-left-origin RGBA8 representation.
	ENGINE_API auto DecodeRGBA8(std::string_view PhysicalFilePath, FTextureSourceData& OutSourceData, std::string& OutError) -> bool;

	// Builds and platform-compresses the complete mip chain used by both 2D and cube textures.
	ENGINE_API auto BuildMipChain(const FTextureSourceData& SourceData, ETextureUsage Usage, bool bSRGB,
		FTexturePlatformData& OutPlatformData, std::string& OutError) -> bool;
}
