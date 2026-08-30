#pragma once

#include "Shader/ShaderCookedLibrary.h"

namespace Durin
{
	auto ProduceCookedShaderLibrary(
		EShaderTargetPlatform TargetPlatform,
		EShaderTargetProfile TargetProfile,
		std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool;
}
