#pragma once

#include "CoreMinimal.h"
#include "Texture/TextureMipBuilder.h"
#include "Texture/TextureMipView.h"

namespace Durin::TextureMipBuilder
{
	// Source is validated RGBA8; rows borrow its storage until bounded tasks drain.
	auto CompressTextureMip(const FReadOnlyMipView& Source, EPixelFormat Format,
		ETextureCompressionQuality Quality, FTexture2DMipData& OutMip,
		const FBuildExecutionOptions* ExecutionOptions) -> std::expected<void, std::string>;
}
