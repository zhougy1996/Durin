#include "Shader/ShaderCompiler.h"

#include "Misc/AppConfigCache.h"

namespace Durin
{
	FShaderCompiler* GShaderCompiler = nullptr;

	FShaderCompiler::FShaderCompiler()
	{
		Settings.bForceRecompile = GAppConfig.GetBoolValue("ForceRecompileShaders", false);;
	}

}

