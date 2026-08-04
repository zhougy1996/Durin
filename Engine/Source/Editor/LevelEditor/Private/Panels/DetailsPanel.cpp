#include "Panels/DetailsPanel.h"
#include "Panels/DetailsPanelTargeting.h"
#include "Editor/ReflectedPropertyView.h"

#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/MathStructs.h"
#include "Engine/Actor.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorWorkspaceUI.h"
#include "Workspace/LevelEditorContext.h"
#include "Workspace/LevelEditorHelpers.h"
#include "Workspace/LevelEditorWorkspace.h"
#include "LevelEditorCustomizations.h"
#include "Settings/LevelEditorSessionSettings.h"
#include "Math/Color.h"
#include "MonaImGui.h"
#include "MonaImGuiPropertyTable.h"

namespace Durin
{
	FDetailsPanel::FDetailsPanel(FLevelEditorSessionSettings& InSessionSettings)
		: SessionSettings(InSessionSettings)
		, ComponentPaneRatio(InSessionSettings.GetDetailsPaneRatio())
	{
	}

	FDetailsPanel::~FDetailsPanel()
	{
		PropertyView.FinishActiveEdit(nullptr, true);
	}

	auto FDetailsPanel::Draw(FLevelEditorContext& Context) -> void
	{
		if (!EditorWorkspaceUI::BeginDockablePanel(LevelEditorWorkspace::Type, "Details", "Details", GetOpenPtr()))
		{
			if (!FinishActivePropertyEdit(&Context, true)) SetOpen(true);
			ImGui::End();
			return;
		}

		AActor* Actor = Context.GetPrimarySelectedActor();
		if (Actor == nullptr)
		{
			if (!FinishActivePropertyEdit(&Context, true))
			{
				ImGui::TextDisabled("The active property preview must be restored before changing selection.");
				ImGui::End();
				return;
			}
			PropertyActor = nullptr;
			ComponentTree.ResetSelection();
			ImGui::TextDisabled("Select an actor to inspect it.");
			ImGui::End();
			return;
		}

		if (PropertyActor.Get() != Actor)
		{
			if (!FinishActivePropertyEdit(&Context, true))
			{
				Actor = PropertyActor.Get();
			}
			else
			{
				PropertyActor = Actor;
				ComponentTree.SetSelectedComponent(DetailsPanelTargeting::ResolveDefaultComponent(Actor));
				ComponentTree.ResetRenameState();
			}
		}
		if (!Actor)
		{
			ImGui::End();
			return;
		}
		ComponentTree.SetSelectedComponent(DetailsPanelTargeting::ResolveSelectedComponent(Actor, ComponentTree.GetSelectedComponent()));

		ImGui::TextUnformatted(Actor->GetName().c_str());
		ImGui::TextDisabled("%s", Actor->GetClass()->GetName().c_str());
		if (Context.bReadOnly) ImGui::TextColored(MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Warning), "Runtime values (read-only)");
		if (Context.GetSelectedActors().size() > 1)
		{
			ImGui::TextDisabled("%zu actors selected; editing primary actor only.", Context.GetSelectedActors().size());
		}
		const float SplitterThickness = MonaImGui::GetUIStyleMetrics().SplitterThickness;
		const float AvailableHeight = ImGui::GetContentRegionAvail().y;
		const float UsableHeight = FMath::Max(AvailableHeight - SplitterThickness, 0.0f);
		float ComponentHeight = UsableHeight * ComponentPaneRatio;
		if (UsableHeight >= MonaImGui::ScaleUI(220.0f))
		{
			ComponentHeight = std::clamp(ComponentHeight, MonaImGui::ScaleUI(90.0f), UsableHeight - MonaImGui::ScaleUI(120.0f));
		}

		if (ImGui::BeginChild("DetailsComponents", ImVec2(0.0f, ComponentHeight), ImGuiChildFlags_Borders))
		{
			ComponentTree.Draw(Context, Actor);
		}
		ImGui::EndChild();

		if (MonaImGui::DrawSplitter("DetailsSplitter", MonaImGui::EUISplitterAxis::Y, ImGui::GetContentRegionAvail().x, UsableHeight, MonaImGui::ScaleUI(90.0f), MonaImGui::ScaleUI(120.0f), ComponentPaneRatio))
			SessionSettings.SetDetailsPaneRatio(ComponentPaneRatio);

		if (ImGui::BeginChild("DetailsProperties", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders))
		{
			DObject* InspectedObject = ComponentTree.GetSelectedComponent() ? static_cast<DObject*>(ComponentTree.GetSelectedComponent()) : static_cast<DObject*>(Actor);
			if (Context.bReadOnly) ImGui::BeginDisabled();
			DrawReflectedProperties(Context, InspectedObject);
			if (Context.bReadOnly) ImGui::EndDisabled();
		}
		ImGui::EndChild();
		ImGui::End();
	}

	auto FDetailsPanel::DrawReflectedProperties(FLevelEditorContext& Context, DObject* Object) -> void
	{
		const FReflectedPropertyViewContext ViewContext = MakePropertyViewContext(Context);
		if (!PropertyView.HandleOwnerContext(ViewContext, Object))
		{
			ImGui::TextDisabled("The active property preview must be restored before changing targets.");
			return;
		}
		if (!Object)
		{
			ImGui::TextDisabled("Nothing to inspect.");
			return;
		}

		ImGui::TextDisabled("%s", Object->GetClass()->GetName().c_str());
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##PropertySearch", "Search properties...", PropertySearchText.data(), PropertySearchText.size());

		FObjectPropertyViewBuilder Builder(PropertySearchText.data());
		for (const std::shared_ptr<IObjectDetailsCustomization>& Customization
			: FLevelEditorCustomizationRegistry::Get().FindObjectDetailsCustomizations(Object->GetClass()))
		{
			Customization->CustomizeDetails(Context, Object, Builder);
		}
		if (!MonaImGui::PropertyEdit::BeginTable("DetailsPropertyTable")) return;

		const FObjectPropertyViewBuilderResult BuilderResult = Builder.DrawRows(PropertyView, ViewContext);
		FObjectPropertyViewResult ObjectViewResult;
		if (!Builder.IsReplacingDefaultProperties())
		{
			ObjectViewResult = PropertyView.EditObject(ViewContext, Object, {
				.SearchText = PropertySearchText.data(),
				.Filter = [&Builder](const FProperty& Property, uint32) {
					return !Builder.IsPropertyHidden(Property);
				},
				.bCreatePropertyTable = false,
				.bShowEmptyMessage = false,
			});
		}
		MonaImGui::PropertyEdit::EndTable();

		if (BuilderResult.VisibleRowCount + ObjectViewResult.VisiblePropertyCount == 0)
		{
			ImGui::TextDisabled(PropertySearchText[0] != '\0'
				? "No properties match the current search."
				: "This object has no reflected Edit properties.");
		}
	}

	auto FDetailsPanel::MakePropertyViewContext(FLevelEditorContext& Context) const -> FReflectedPropertyViewContext
	{
		return {
			.Transactions = GEditor ? &GEditor->GetTransactionManager() : nullptr,
			.ReportError = [&Context](std::string Error) { Context.SetError(std::move(Error)); },
			.bReadOnly = Context.bReadOnly,
		};
	}

	auto FDetailsPanel::RequestDeactivate(FLevelEditorContext& Context) -> bool
	{
		return FinishActivePropertyEdit(&Context, true);
	}

	auto FDetailsPanel::FinishActivePropertyEdit(FLevelEditorContext* Context, bool bCancel) -> bool
	{
		if (!Context)
			return PropertyView.FinishActiveEdit(nullptr, bCancel);
		const FReflectedPropertyViewContext ViewContext = MakePropertyViewContext(*Context);
		return PropertyView.FinishActiveEdit(&ViewContext, bCancel);
	}
} // namespace Durin
