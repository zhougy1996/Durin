#pragma once

#include "MaterialEditorAPI.h"

namespace Durin
{
	class DMaterialInterface;

	// Owns the isolated viewport and scene used to preview editor materials.
	class FMaterialPreview final
	{
	public:
		explicit FMaterialPreview(uint64 PreviewId);
		~FMaterialPreview();

		auto SetVisible(bool bInVisible) -> void;
		auto Draw(DMaterialInterface* Material, float PanelHeight = 0.0f) -> void;

	private:
		class FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
