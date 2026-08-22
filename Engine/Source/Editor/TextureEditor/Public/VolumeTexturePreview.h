#pragma once

#include "TextureEditorAPI.h"
#include "Texture/VolumeTexture.h"

namespace Durin::Editor::Texture
{
	enum class EVolumeTexturePreviewAxis : uint8 { XY, XZ, YZ };

	struct FVolumeTexturePreviewSlice
	{
		std::vector<std::byte> Pixels;
		uint32 Width = 0;
		uint32 Height = 0;

		auto IsValid() const -> bool
		{
			return Width > 0 && Height > 0
				&& Pixels.size() == static_cast<uint64>(Width) * Height * 4;
		}
	};

	TEXTUREEDITOR_API auto ExtractVolumeTexturePreviewSlice(
		const FVolumeTextureMipData& Mip, EPixelFormat Format,
		EVolumeTexturePreviewAxis Axis, uint32 SliceIndex)
		-> FVolumeTexturePreviewSlice;
	TEXTUREEDITOR_API auto ExtractVolumeTexturePreviewSlice(
		const FVolumeTextureSourceData& Source,
		EVolumeTexturePreviewAxis Axis, uint32 SliceIndex)
		-> FVolumeTexturePreviewSlice;
}
