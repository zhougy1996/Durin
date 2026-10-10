#pragma once

#include "CoreMinimal.h"
#include "Texture/TextureBuilder.h"
#include "Texture/TextureMipView.h"

namespace Durin::TextureBuilder
{
	// Source is validated RGBA8; rows borrow its storage until bounded tasks drain.
	auto CompressTextureMip(const FReadOnlyMip& Source, EPixelFormat Format,
		ETextureCompressionQuality Quality, FTexture2DMipData& OutMip,
		const FBuildExecutionControl* Control) -> std::expected<void, FTexture2DBuildError>;
}
