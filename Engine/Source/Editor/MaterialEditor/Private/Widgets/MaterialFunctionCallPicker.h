#pragma once

#include "CoreMinimal.h"
#include "MaterialGraph.h"

namespace Durin::MaterialEditor
{
	auto DrawMaterialFunctionCallInputs(DMaterialGraph& Graph, const FGuid& CallId,
		std::string& Error) -> void;
}
