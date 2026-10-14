#pragma once
#include "CoreMinimal.h"
#include "Shader/ShaderDiagnostics.h"

namespace Durin
{
	class FShaderBuilder;
	auto BuildCookShaderInputIdentity(FShaderBuilder& Builder, std::string& OutIdentity,
		const std::function<bool()>& IsCancelled) -> FShaderOperationResult;
}
