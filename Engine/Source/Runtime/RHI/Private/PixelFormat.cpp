#include "PixelFormat.h"

namespace Durin
{
	 // Format mapping table. The rows must be in the exactly same order as Format enum members are defined.
    static const FPixelFormatInfo FormatInfo[] = {
        { EPixelFormat::Unknown,           "Unknown",           0,   0, EPixelFormatKind::Integer,      false, false, false, false, false, false, false, false },
        { EPixelFormat::R8_UINT,           "R8_UINT",           1,   1, EPixelFormatKind::Integer,      true,  false, false, false, false, false, false, false },
        { EPixelFormat::R8_SINT,           "R8_SINT",           1,   1, EPixelFormatKind::Integer,      true,  false, false, false, false, false, true,  false },
        { EPixelFormat::R8_UNORM,          "R8_UNORM",          1,   1, EPixelFormatKind::Normalized,   true,  false, false, false, false, false, false, false },
        { EPixelFormat::R8_SNORM,          "R8_SNORM",          1,   1, EPixelFormatKind::Normalized,   true,  false, false, false, false, false, true,  false },
        { EPixelFormat::RG8_UINT,          "RG8_UINT",          2,   1, EPixelFormatKind::Integer,      true,  true,  false, false, false, false, false, false },
        { EPixelFormat::RG8_SINT,          "RG8_SINT",          2,   1, EPixelFormatKind::Integer,      true,  true,  false, false, false, false, true,  false },
        { EPixelFormat::RG8_UNORM,         "RG8_UNORM",         2,   1, EPixelFormatKind::Normalized,   true,  true,  false, false, false, false, false, false },
        { EPixelFormat::RG8_SNORM,         "RG8_SNORM",         2,   1, EPixelFormatKind::Normalized,   true,  true,  false, false, false, false, true,  false },
        { EPixelFormat::R16_UINT,          "R16_UINT",          2,   1, EPixelFormatKind::Integer,      true,  false, false, false, false, false, false, false },
        { EPixelFormat::R16_SINT,          "R16_SINT",          2,   1, EPixelFormatKind::Integer,      true,  false, false, false, false, false, true,  false },
        { EPixelFormat::R16_UNORM,         "R16_UNORM",         2,   1, EPixelFormatKind::Normalized,   true,  false, false, false, false, false, false, false },
        { EPixelFormat::R16_SNORM,         "R16_SNORM",         2,   1, EPixelFormatKind::Normalized,   true,  false, false, false, false, false, true,  false },
        { EPixelFormat::R16_FLOAT,         "R16_FLOAT",         2,   1, EPixelFormatKind::Float,        true,  false, false, false, false, false, true,  false },
        { EPixelFormat::BGRA4_UNORM,       "BGRA4_UNORM",       2,   1, EPixelFormatKind::Normalized,   true,  true,  true,  true,  false, false, false, false },
        { EPixelFormat::B5G6R5_UNORM,      "B5G6R5_UNORM",      2,   1, EPixelFormatKind::Normalized,   true,  true,  true,  false, false, false, false, false },
        { EPixelFormat::B5G5R5A1_UNORM,    "B5G5R5A1_UNORM",    2,   1, EPixelFormatKind::Normalized,   true,  true,  true,  true,  false, false, false, false },
        { EPixelFormat::RGBA8_UINT,        "RGBA8_UINT",        4,   1, EPixelFormatKind::Integer,      true,  true,  true,  true,  false, false, false, false },
        { EPixelFormat::RGBA8_SINT,        "RGBA8_SINT",        4,   1, EPixelFormatKind::Integer,      true,  true,  true,  true,  false, false, true,  false },
        { EPixelFormat::RGBA8_UNORM,       "RGBA8_UNORM",       4,   1, EPixelFormatKind::Normalized,   true,  true,  true,  true,  false, false, false, false },
        { EPixelFormat::RGBA8_SNORM,       "RGBA8_SNORM",       4,   1, EPixelFormatKind::Normalized,   true,  true,  true,  true,  false, false, true,  false },
        { EPixelFormat::BGRA8_UNORM,       "BGRA8_UNORM",       4,   1, EPixelFormatKind::Normalized,   true,  true,  true,  true,  false, false, false, false },
        { EPixelFormat::BGRX8_UNORM,       "BGRX8_UNORM",       4,   1, EPixelFormatKind::Normalized,   true,  true,  true,  false, false, false, false, false },
        { EPixelFormat::SRGBA8_UNORM,      "SRGBA8_UNORM",      4,   1, EPixelFormatKind::Normalized,   true,  true,  true,  true,  false, false, false, true  },
        { EPixelFormat::SBGRA8_UNORM,      "SBGRA8_UNORM",      4,   1, EPixelFormatKind::Normalized,   true,  true,  true,  true,  false, false, false, true  },
        { EPixelFormat::SBGRX8_UNORM,      "SBGRX8_UNORM",      4,   1, EPixelFormatKind::Normalized,   true,  true,  true,  false, false, false, false, true  },
        { EPixelFormat::R10G10B10A2_UNORM, "R10G10B10A2_UNORM", 4,   1, EPixelFormatKind::Normalized,   true,  true,  true,  true,  false, false, false, false },
        { EPixelFormat::R11G11B10_FLOAT,   "R11G11B10_FLOAT",   4,   1, EPixelFormatKind::Float,        true,  true,  true,  false, false, false, false, false },
        { EPixelFormat::RG16_UINT,         "RG16_UINT",         4,   1, EPixelFormatKind::Integer,      true,  true,  false, false, false, false, false, false },
        { EPixelFormat::RG16_SINT,         "RG16_SINT",         4,   1, EPixelFormatKind::Integer,      true,  true,  false, false, false, false, true,  false },
        { EPixelFormat::RG16_UNORM,        "RG16_UNORM",        4,   1, EPixelFormatKind::Normalized,   true,  true,  false, false, false, false, false, false },
        { EPixelFormat::RG16_SNORM,        "RG16_SNORM",        4,   1, EPixelFormatKind::Normalized,   true,  true,  false, false, false, false, true,  false },
        { EPixelFormat::RG16_FLOAT,        "RG16_FLOAT",        4,   1, EPixelFormatKind::Float,        true,  true,  false, false, false, false, true,  false },
        { EPixelFormat::R32_UINT,          "R32_UINT",          4,   1, EPixelFormatKind::Integer,      true,  false, false, false, false, false, false, false },
        { EPixelFormat::R32_SINT,          "R32_SINT",          4,   1, EPixelFormatKind::Integer,      true,  false, false, false, false, false, true,  false },
        { EPixelFormat::R32_FLOAT,         "R32_FLOAT",         4,   1, EPixelFormatKind::Float,        true,  false, false, false, false, false, true,  false },
        { EPixelFormat::RGBA16_UINT,       "RGBA16_UINT",       8,   1, EPixelFormatKind::Integer,      true,  true,  true,  true,  false, false, false, false },
        { EPixelFormat::RGBA16_SINT,       "RGBA16_SINT",       8,   1, EPixelFormatKind::Integer,      true,  true,  true,  true,  false, false, true,  false },
        { EPixelFormat::RGBA16_FLOAT,      "RGBA16_FLOAT",      8,   1, EPixelFormatKind::Float,        true,  true,  true,  true,  false, false, true,  false },
        { EPixelFormat::RGBA16_UNORM,      "RGBA16_UNORM",      8,   1, EPixelFormatKind::Normalized,   true,  true,  true,  true,  false, false, false, false },
        { EPixelFormat::RGBA16_SNORM,      "RGBA16_SNORM",      8,   1, EPixelFormatKind::Normalized,   true,  true,  true,  true,  false, false, true,  false },
        { EPixelFormat::RG32_UINT,         "RG32_UINT",         8,   1, EPixelFormatKind::Integer,      true,  true,  false, false, false, false, false, false },
        { EPixelFormat::RG32_SINT,         "RG32_SINT",         8,   1, EPixelFormatKind::Integer,      true,  true,  false, false, false, false, true,  false },
        { EPixelFormat::RG32_FLOAT,        "RG32_FLOAT",        8,   1, EPixelFormatKind::Float,        true,  true,  false, false, false, false, true,  false },
        { EPixelFormat::RGB32_UINT,        "RGB32_UINT",        12,  1, EPixelFormatKind::Integer,      true,  true,  true,  false, false, false, false, false },
        { EPixelFormat::RGB32_SINT,        "RGB32_SINT",        12,  1, EPixelFormatKind::Integer,      true,  true,  true,  false, false, false, true,  false },
        { EPixelFormat::RGB32_FLOAT,       "RGB32_FLOAT",       12,  1, EPixelFormatKind::Float,        true,  true,  true,  false, false, false, true,  false },
        { EPixelFormat::RGBA32_UINT,       "RGBA32_UINT",       16,  1, EPixelFormatKind::Integer,      true,  true,  true,  true,  false, false, false, false },
        { EPixelFormat::RGBA32_SINT,       "RGBA32_SINT",       16,  1, EPixelFormatKind::Integer,      true,  true,  true,  true,  false, false, true,  false },
        { EPixelFormat::RGBA32_FLOAT,      "RGBA32_FLOAT",      16,  1, EPixelFormatKind::Float,        true,  true,  true,  true,  false, false, true,  false },
        { EPixelFormat::D16,               "D16",               2,   1, EPixelFormatKind::DepthStencil, false, false, false, false, true,  false, false, false },
        { EPixelFormat::D24S8,             "D24S8",             4,   1, EPixelFormatKind::DepthStencil, false, false, false, false, true,  true,  false, false },
        { EPixelFormat::X24G8_UINT,        "X24G8_UINT",        4,   1, EPixelFormatKind::Integer,      false, false, false, false, false, true,  false, false },
        { EPixelFormat::D32,               "D32",               4,   1, EPixelFormatKind::DepthStencil, false, false, false, false, true,  false, false, false },
        { EPixelFormat::D32S8,             "D32S8",             8,   1, EPixelFormatKind::DepthStencil, false, false, false, false, true,  true,  false, false },
        { EPixelFormat::X32G8_UINT,        "X32G8_UINT",        8,   1, EPixelFormatKind::Integer,      false, false, false, false, false, true,  false, false },
        { EPixelFormat::BC1_UNORM,         "BC1_UNORM",         8,   4, EPixelFormatKind::Normalized,   true,  true,  true,  true,  false, false, false, false },
        { EPixelFormat::BC1_UNORM_SRGB,    "BC1_UNORM_SRGB",    8,   4, EPixelFormatKind::Normalized,   true,  true,  true,  true,  false, false, false, true  },
        { EPixelFormat::BC2_UNORM,         "BC2_UNORM",         16,  4, EPixelFormatKind::Normalized,   true,  true,  true,  true,  false, false, false, false },
        { EPixelFormat::BC2_UNORM_SRGB,    "BC2_UNORM_SRGB",    16,  4, EPixelFormatKind::Normalized,   true,  true,  true,  true,  false, false, false, true  },
        { EPixelFormat::BC3_UNORM,         "BC3_UNORM",         16,  4, EPixelFormatKind::Normalized,   true,  true,  true,  true,  false, false, false, false },
        { EPixelFormat::BC3_UNORM_SRGB,    "BC3_UNORM_SRGB",    16,  4, EPixelFormatKind::Normalized,   true,  true,  true,  true,  false, false, false, true  },
        { EPixelFormat::BC4_UNORM,         "BC4_UNORM",         8,   4, EPixelFormatKind::Normalized,   true,  false, false, false, false, false, false, false },
        { EPixelFormat::BC4_SNORM,         "BC4_SNORM",         8,   4, EPixelFormatKind::Normalized,   true,  false, false, false, false, false, true,  false },
        { EPixelFormat::BC5_UNORM,         "BC5_UNORM",         16,  4, EPixelFormatKind::Normalized,   true,  true,  false, false, false, false, false, false },
        { EPixelFormat::BC5_SNORM,         "BC5_SNORM",         16,  4, EPixelFormatKind::Normalized,   true,  true,  false, false, false, false, true,  false },
        { EPixelFormat::BC6H_UFLOAT,       "BC6H_UFLOAT",       16,  4, EPixelFormatKind::Float,        true,  true,  true,  false, false, false, false, false },
        { EPixelFormat::BC6H_SFLOAT,       "BC6H_SFLOAT",       16,  4, EPixelFormatKind::Float,        true,  true,  true,  false, false, false, true,  false },
        { EPixelFormat::BC7_UNORM,         "BC7_UNORM",         16,  4, EPixelFormatKind::Normalized,   true,  true,  true,  true,  false, false, false, false },
        { EPixelFormat::BC7_UNORM_SRGB,    "BC7_UNORM_SRGB",    16,  4, EPixelFormatKind::Normalized,   true,  true,  true,  true,  false, false, false, true  },
    };

	auto GetPixelFormatInfo(EPixelFormat Format) -> const FPixelFormatInfo&
	{
		static_assert(sizeof(FormatInfo) / sizeof(FPixelFormatInfo) == static_cast<size_t>(EPixelFormat::Count),
		   "The format info table doesn't have the right number of elements");

		if (static_cast<uint32_t>(Format) >= static_cast<uint32_t>(EPixelFormat::Count))
			return FormatInfo[0]; // Unknown

		const FPixelFormatInfo& Info = FormatInfo[static_cast<size_t>(Format)];
		check(Info.Format == Format);
		return Info;
	}
} // namespace Durin