#pragma once

#include "LevelEditorAPI.h"
#include "Widgets/MWidget.h"

namespace Durin
{
	class ILevelEditorPanel;
	struct FLevelEditorContext;

	class MLevelEditor final : public MWidget
	{
	public:
		LEVELEDITOR_API MLevelEditor();
		LEVELEDITOR_API ~MLevelEditor() override;
		LEVELEDITOR_API auto Construct() -> void override;
		LEVELEDITOR_API auto Draw() -> void override;

	private:
		auto DrawMainMenu() -> void;
		auto BuildDefaultLayout(uint32 DockSpaceId) -> void;

		std::unique_ptr<FLevelEditorContext> Context;
		std::vector<std::unique_ptr<ILevelEditorPanel>> Panels;
		bool bResetLayoutRequested = false;
	};
} // namespace Durin
