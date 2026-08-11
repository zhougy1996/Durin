#pragma once

#include "DObject/ObjectPtr.h"
#include "Editor/PropertyView.h"
#include "Panels/DetailsComponentTree.h"
#include "Panels/LevelEditorPanel.h"

namespace Durin
{
	class AActor;
	class DActorComponent;
	class DSceneComponent;
	class DObject;
	class FProperty;
	class FLevelEditorSessionSettings;

	// Draws reflected and customized properties for the current editor selection.
	class FDetailsPanel final : public ILevelEditorPanel
	{
	public:
		explicit FDetailsPanel(FLevelEditorSessionSettings& InSessionSettings);
		~FDetailsPanel() override;
		auto GetWindowName() const -> const char* override { return "Details"; }
		auto Draw(FLevelEditorContext& Context) -> void override;
		auto RequestDeactivate(FLevelEditorContext& Context) -> bool;

	private:
		auto DrawReflectedProperties(FLevelEditorContext& Context, DObject* Object) -> void;
		auto FinishActivePropertyEdit(FLevelEditorContext* Context, bool bCancel) -> bool;
		auto MakePropertyViewContext(FLevelEditorContext& Context) const -> Editor::FPropertyViewContext;

		std::array<char, 128> PropertySearchText{};
		TObjectPtr<AActor> PropertyActor;
		FLevelEditorSessionSettings& SessionSettings;
		Editor::FPropertyView PropertyView;
		FDetailsComponentTree ComponentTree;
		float ComponentPaneRatio;
	};
} // namespace Durin
