#pragma once

#include "CoreMinimal.h"
#include "MaterialEditorAPI.h"

namespace Durin
{
	class DMaterial;

	// Produces the first renderable program before a newly created base material is saved or opened.
	MATERIALEDITOR_API auto PrepareNewMaterialForEditing(
		DMaterial& Material, std::string& OutError) -> bool;
}
