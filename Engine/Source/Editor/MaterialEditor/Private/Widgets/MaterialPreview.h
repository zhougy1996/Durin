#pragma once

#include "CoreMinimal.h"

#include "MaterialEditorAPI.h"
#include "Materials/MaterialProgramTypes.h"

namespace Durin { class DMaterialInterface; }

namespace Durin::MaterialEditor
{
	// Owns the isolated viewport and scene used to preview editor materials.
	class FMaterialPreview final
	{
	public:
		explicit FMaterialPreview(std::string_view PreviewId);
		~FMaterialPreview();

		auto SetVisible(bool bInVisible) -> void;
		auto SetMaterial(DMaterialInterface* Material) -> void;
		auto Draw(DMaterialInterface* Material, float PanelHeight = 0.0f) -> void;
		auto DrawRootInputs(DMaterialInterface* Material, float PanelHeight,
			const std::optional<std::pair<EMaterialProperty, FMaterialInputDefault>>& Draft) -> void;
		auto OnRootInputCommitted(uint64 PreviousRevision, uint64 CurrentRevision) -> void;

	private:
		class FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
