#pragma once

#include "DObject/Archive.h"
#include "MaterialEditorAPI.h"

namespace Durin
{
	class DMaterialInterface;
	class FProperty;
}

namespace Durin::Editor::Material
{
	// Captures the authored state that a material document must restore on discard.
	class FMaterialDocumentSnapshot
	{
	public:
		MATERIALEDITOR_API auto Capture(
			DMaterialInterface& Material, std::string& OutError) -> bool;
		MATERIALEDITOR_API auto Restore(
			DMaterialInterface& Material, std::string& OutError) const -> bool;

	private:
		struct FEntry
		{
			FProperty* Property = nullptr;
			uint32 ArrayIndex = 0;
			FPropertyValueSnapshot Value;
		};

		DClass* MaterialClass = nullptr;
		std::vector<FEntry> Entries;
	};
}
