#pragma once

#include "EngineAPI.h"
#include "DObject/DObjectFwd.h"
#include "DObject/ObjectMacros.h"
#include "TextureSourceFormat.gen.h"

namespace Durin
{
	// Describes source pixel storage independently of asset and bulk-data ownership.
	DENUM()
	enum class ETextureSourceFormat : uint8
	{
		Invalid,
		RGBA8,
		R8_UNORM,
		RG8_UNORM,
		R16_FLOAT,
		RGBA16_FLOAT,
		G16_UNORM,
		RGBA16_UNORM,
		R32_FLOAT,
		RGBA32_FLOAT,
	};

}
