#pragma once

#include "CoreMinimal.h"
#include "RendererModule.h"

namespace Durin::Testing
{
	// Import the renderer's static Shader registrations without starting a scene renderer.
	inline FRendererModule MaterialShaderInventory;
}
