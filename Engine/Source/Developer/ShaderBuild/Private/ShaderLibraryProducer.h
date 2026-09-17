#pragma once

#include "Shader/ShaderCookedLibrary.h"

namespace Durin
{
	auto ProduceCookedShaderLibrary(
		EShaderTargetPlatform TargetPlatform,
		EShaderTargetProfile TargetProfile,
		FByteBuffer& OutBytes,
		std::shared_ptr<const FShaderSourceArtifacts> Artifacts = {},
		const std::function<bool()>& IsCancelled = {}) -> FShaderOperationResult;
}
