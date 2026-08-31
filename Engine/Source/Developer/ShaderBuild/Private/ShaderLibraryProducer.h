#pragma once

#include "Shader/ShaderCookedLibrary.h"

namespace Durin
{
	auto ProduceCookedShaderLibrary(
		EShaderTargetPlatform TargetPlatform,
		EShaderTargetProfile TargetProfile,
		FByteArray& OutBytes,
		std::string& OutError) -> bool;
}
