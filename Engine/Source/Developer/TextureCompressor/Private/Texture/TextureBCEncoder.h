#pragma once

#include "CoreMinimal.h"
#include "Texture/Texture2DData.h"

namespace Durin::TextureMipBuilder
{
	struct FBuildExecutionOptions;
	struct FReadOnlyMipView;

	// Source is validated RGBA8; rows borrow its storage until bounded tasks drain.
	auto CompressTextureMip(const FReadOnlyMipView& Source, EPixelFormat Format,
		ETextureCompressionQuality Quality, FTexture2DMipData& OutMip,
		const FBuildExecutionOptions* ExecutionOptions) -> std::expected<void, std::string>;
}
