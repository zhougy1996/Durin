#pragma once

#include "Texture/TextureDerivedData.h"

namespace Durin::TexturePrivate
{
	inline auto ToStablePixelFormat(EPixelFormat Format, ETextureStablePixelFormat& OutFormat) -> bool
	{
		switch (Format)
		{
		case EPixelFormat::RGBA32_FLOAT: OutFormat = ETextureStablePixelFormat::RGBA32_FLOAT; return true;
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

	inline auto FromStablePixelFormat(uint32 StableFormat, EPixelFormat& OutFormat) -> bool
	{
		switch (static_cast<ETextureStablePixelFormat>(StableFormat))
		{
		case ETextureStablePixelFormat::RGBA32_FLOAT: OutFormat = EPixelFormat::RGBA32_FLOAT; return true;
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

	inline auto ToVolumeStableFormat(EPixelFormat Format,
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

	inline auto FromVolumeStableFormat(uint32 StableFormat, EPixelFormat& OutFormat) -> bool
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
