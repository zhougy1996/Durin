#pragma once

#include "Shader/ShaderCookedLibrary.h"

namespace Durin
{
	auto ProduceCookedShaderLibrary(
		EShaderTargetPlatform TargetPlatform,
		EShaderTargetProfile TargetProfile,
		FByteBuffer& OutBytes,
		std::string& OutError) -> bool;
}
