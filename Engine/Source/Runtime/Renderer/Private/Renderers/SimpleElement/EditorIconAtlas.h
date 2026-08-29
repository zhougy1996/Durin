#pragma once

#include "Misc/CoreTypes.h"

namespace Durin
{
	// Defines the tightly packed horizontal atlas used by built-in editor sprites.
	struct FEditorIconAtlasLayout
	{
		static constexpr uint32 IconExtent = 64;
		static constexpr uint32 IconCount = 3;
		static constexpr uint32 Width = IconExtent * IconCount;
		static constexpr uint32 Height = IconExtent;
		static constexpr uint32 BytesPerPixel = 4;
		static constexpr uint32 RowPitchBytes = Width * BytesPerPixel;
		static constexpr size_t PixelByteCount =
			static_cast<size_t>(RowPitchBytes) * Height;

		static constexpr auto GetTileX(uint32 IconIndex) -> uint32
		{
			return IconIndex * IconExtent;
		}

		static constexpr auto GetMinU(uint32 IconIndex) -> float
		{
			return static_cast<float>(GetTileX(IconIndex)) / Width;
		}

	};

	auto BuildEditorIconAtlasPixels()
		-> std::array<uint8, FEditorIconAtlasLayout::PixelByteCount>;
} // namespace Durin
