#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"

namespace Durin
{
	enum class ECookedMeshLoadError : uint8
	{
		None, Unavailable, Read, Product, Publication, Cancelled, Task
	};

	// The asset owns its last load failure; execution state lives in its pending update.
	struct FCookedMeshLoadError
	{
		ECookedMeshLoadError Code = ECookedMeshLoadError::None;
		std::string Diagnostic;
	};

	ENGINE_API auto FormatCookedMeshLoadError(const FCookedMeshLoadError& Error) -> std::string;
}
