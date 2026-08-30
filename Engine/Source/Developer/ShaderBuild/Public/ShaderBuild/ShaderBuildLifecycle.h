#pragma once

#include "ShaderBuildAPI.h"

namespace Durin
{
	// Starts and stops the authoring provider selected by Editor, Cook, or a
	// native-test root. These calls do not start the RenderCore module.
	SHADERBUILD_API auto InitializeShaderBuild() -> void;
	SHADERBUILD_API auto InitializeShaderBuildForTesting() -> void;
	SHADERBUILD_API auto ShutdownShaderBuild() -> void;
}
