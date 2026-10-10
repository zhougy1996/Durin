#pragma once

#include "CoreMinimal.h"
#include "Texture/Texture2DBuildTypes.h"

namespace Durin::TextureBuilder
{
	// Borrowed only during synchronous processing; owning images outlive encoder tasks.
	struct FReadOnlyMip
	{
		FByteView Pixels;
		uint32 Width, Height, RowPitch;
		FReadOnlyMip(const Image::FImage& Image)
			: Pixels(Image.GetPixels()), Width(Image.GetInfo().Width),
			Height(Image.GetInfo().Height), RowPitch(Width * 4) {}
		FReadOnlyMip(FByteView Pixels, uint32 Width, uint32 Height, uint32 RowPitch)
			: Pixels(Pixels), Width(Width), Height(Height), RowPitch(RowPitch) {}
	};
}
