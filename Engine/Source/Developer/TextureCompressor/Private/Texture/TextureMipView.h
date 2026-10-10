#pragma once

#include "CoreMinimal.h"
#include "Image/Image.h"

namespace Durin::TextureMipBuilder
{
	// Borrowed only during synchronous processing; owning images outlive encoder tasks.
	struct FReadOnlyMipView
	{
		FByteView Pixels;
		uint32 Width, Height, RowPitch;
		FReadOnlyMipView(const Image::FImage& Image)
			: Pixels(Image.GetPixels()), Width(Image.GetInfo().Width),
			Height(Image.GetInfo().Height), RowPitch(Width * 4) {}
		FReadOnlyMipView(FByteView Pixels, uint32 Width, uint32 Height, uint32 RowPitch)
			: Pixels(Pixels), Width(Width), Height(Height), RowPitch(RowPitch) {}
	};
}
