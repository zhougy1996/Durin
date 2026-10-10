#pragma once

#include "CoreMinimal.h"

namespace Durin::TextureCompressorPrivate
{
	// Algorithm revisions belong to the recipe implementation, never to payload compatibility.
	inline constexpr uint32 Texture2DAlgorithmRevision = 4;
	inline constexpr uint32 TextureCubeAlgorithmRevision = 4;
	inline constexpr uint32 VolumeTextureAlgorithmRevision = 3;
	inline constexpr uint32 CubeProjectionRevision = 3;
	// Shared output-affecting helpers used by the 2D and Cube recipes.
	inline constexpr uint32 ColorRevision = 1;
	inline constexpr uint32 MipRevision = 1;
}
