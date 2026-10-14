#pragma once
#include "CoreMinimal.h"
#include "ShaderBuildAPI.h"
#include "DerivedDataBuildExecution.h"

namespace Durin::ShaderBuildPrivate
{
	// Call only with no outstanding subscribers; drains before replacing the service.
	SHADERBUILD_API auto ResetServiceForTests(DerivedData::FBuildServiceOptions Options = {}) -> void;
}
