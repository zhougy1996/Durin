#pragma once
#include "CoreMinimal.h"
#include "Materials/MaterialProgramCompiler.h"
namespace Durin::MaterialBuildPrivate
{
	enum class EMaterialCollectionSource { RequestSnapshot, LiveObjects };

	// Fill one consumer's program from its request and the shared shader map.
	ENGINE_API auto BindMaterialShaderMap(FMaterialProgram& Program, std::shared_ptr<const FMaterialShaderMap> ShaderMap,
		const MIR::FCompilerInput& Input, EMaterialCollectionSource Source,
		std::vector<FMaterialProgramDiagnostic>& Diagnostics) -> bool;
}
