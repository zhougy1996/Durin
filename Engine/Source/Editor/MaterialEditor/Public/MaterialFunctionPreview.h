#pragma once

#include "MaterialGraphDocument.h"

namespace Durin::Editor::Material
{
	// Builds an explicit transient root graph. Required inputs receive neutral
	// preview values; optional inputs retain the function's declared defaults.
	MATERIALEDITOR_API auto BuildMaterialFunctionPreview(DMaterialFunctionInterface& Function,
		const FGuid& OutputId, FMaterialGraphDocumentState& OutState,
		FMaterialStaticProperties& OutProperties) -> FMaterialGraphCommandResult;
}
