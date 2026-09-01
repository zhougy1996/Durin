#pragma once

#include "TextureBuildAPI.h"
#include "Texture/Texture2DCompilation.h"
#include "Texture/Texture2DBuildProvider.h"

namespace Durin
{
	TEXTUREBUILD_API auto BuildTexture2D(
		FTexture2DBuildRequest Request,
		FTexture2DBuildProduct& OutProduct,
		std::string& OutError,
		const FTexture2DBuildExecutionControl* ExecutionControl = nullptr) -> bool;

	// Synchronous creation-side entrypoint. Detached product details remain an
	// implementation concern for callers that only need to populate a new asset.
	TEXTUREBUILD_API auto BuildTexture2DInto(
		DTexture2D& Texture,
		FTexture2DBuildRequest Request,
		const FTexture2DPublicationContext& Context,
		std::string& OutError) -> bool;

	// Build-owned recipe identity and DDC value loading used by uncooked source processing
	// loads. Source translation remains outside this module.
	TEXTUREBUILD_API auto MakeTexture2DDerivedDataKey(
		const DTexture2D& Texture,
		std::string& OutKey,
		std::string& OutError) -> bool;
	TEXTUREBUILD_API auto LoadTexture2DDerivedData(
		std::string_view Key,
		std::unique_ptr<FTexturePlatformData>& OutPlatformData,
		ETextureDerivedDataStatus& OutStatus,
		std::string& OutMessage) -> bool;

}
