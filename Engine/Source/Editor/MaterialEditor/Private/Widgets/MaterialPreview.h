#pragma once

#include "MaterialEditorAPI.h"

namespace Durin
{
	class DMaterialInterface;

	class FMaterialPreview final
	{
	public:
		explicit FMaterialPreview(uint64 PreviewId);
		~FMaterialPreview();

		auto SetVisible(bool bInVisible) -> void;
		auto Draw(DMaterialInterface* Material) -> void;

	private:
		class FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
