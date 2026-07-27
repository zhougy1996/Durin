#include "Widgets/MMaterialEditor.h"
#include "Widgets/MaterialParameterPanelModel.h"
#include "Widgets/MaterialPreview.h"

#include "Asset/AssetUpgradeAuditService.h"
#include "AssetSystem.h"
#include "DObject/Package.h"
#include "DObject/DurinPropertyTypes.h"
#include "Editor/EditorAssetPicker.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorWorkspaceUI.h"
#include "Workspace/MaterialEditorWorkspace.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Math/Color.h"
#include "MonaImGui.h"
#include "MonaImGuiPropertyTable.h"
#include "Texture/Texture2D.h"

namespace Durin
{
	namespace
	{
		constexpr float DefaultSidebarRatio = 0.34f;
		constexpr float DefaultPreviewPaneRatio = 0.68f;
		constexpr float WideLayoutMinimumWidth = 760.0f;
		constexpr float MinimumSidebarWidth = 280.0f;
		constexpr float MinimumDetailsWidth = 360.0f;
		constexpr float MinimumPreviewHeight = 260.0f;
		constexpr float MinimumOverviewHeight = 110.0f;
		constexpr float NarrowPreviewHeight = 340.0f;

		auto FormatParameterSource(const FMaterialParameterPanelEntry& Entry) -> std::string
		{
			if (Entry.bHasLocalOverride) return "Local override";
			if (!Entry.Source) return "Unresolved";
			return std::format("Inherited from {}", Entry.Source->GetName());
		}
	}

	MMaterialEditor::MMaterialEditor(FEditorWorkspaceManager& InWorkspaceManager)
		: WorkspaceManager(InWorkspaceManager)
	{
	}

	MMaterialEditor::~MMaterialEditor()
	{
		FinishActivePropertyEdit(true);
		MaterialPreviews.clear();
	}

	auto MMaterialEditor::GetWorkspaceType() const -> const FEditorWorkspaceTypeId&
	{
		return MaterialEditorWorkspace::Type;
	}

	auto MMaterialEditor::OpenDocument(const FEditorDocumentTab& Document) -> EEditorDocumentOpenResult
	{
		if (Document.ResourceId.empty()) return EEditorDocumentOpenResult::Rejected;
		if (FindOpenMaterial(Document.ResourceId)) return EEditorDocumentOpenResult::Opened;
		FAssetPath AssetPath;
		std::string PathError;
		if (!FAssetPath::TryCreate(Document.ResourceId, AssetPath, &PathError))
		{
			SetError(std::move(PathError));
			return EEditorDocumentOpenResult::Rejected;
		}
		DMaterialInterface* Material = nullptr;
		Asset::FAssetLoadReport LoadReport;
		const Asset::FAssetResult Result = Asset::LoadAsset(AssetPath, Material, &LoadReport);
		if (!Result || !Material)
		{
			SetError(Result ? "The selected asset is not a material." : Result.Message);
			return EEditorDocumentOpenResult::Rejected;
		}
		if (GEditor) GEditor->GetAssetUpgradeAuditService().MergeWorkspaceLoadReport(LoadReport);
		OpenMaterials.emplace(Document.ResourceId, Material);
		return EEditorDocumentOpenResult::Opened;
	}

	auto MMaterialEditor::ActivateDocument(const FEditorDocumentTab& Document) -> void
	{
		DMaterialInterface* Material = FindOpenMaterial(Document.ResourceId);
		if (PropertyView.IsEditing() && !PropertyView.IsEditingObject(Material) && !FinishActivePropertyEdit(true)) return;
		if (Material) ActiveResourceId = Document.ResourceId;
		DocumentHost.RequestFocus(Document.Id);
	}

	auto MMaterialEditor::RequestDeactivate() -> bool
	{
		return FinishActivePropertyEdit(true);
	}

	auto MMaterialEditor::RequestCloseDocument(const FEditorDocumentTab& Document) -> EEditorDocumentCloseResult
	{
		if (PropertyView.IsEditingObject(FindOpenMaterial(Document.ResourceId)) && !FinishActivePropertyEdit(true))
			return EEditorDocumentCloseResult::Rejected;
		if (IsDocumentDirty(Document)) return EEditorDocumentCloseResult::PendingConfirmation;
		OpenMaterials.erase(Document.ResourceId);
		MaterialPreviews.erase(Document.Id.Value);
		if (ActiveResourceId == Document.ResourceId) ActiveResourceId.clear();
		return EEditorDocumentCloseResult::Closed;
	}

	auto MMaterialEditor::SaveDocument(const FEditorDocumentTab& Document) -> bool
	{
		return SaveMaterial(FindOpenMaterial(Document.ResourceId));
	}

	auto MMaterialEditor::DiscardDocument(const FEditorDocumentTab& Document) -> bool
	{
		DMaterialInterface* Material = FindOpenMaterial(Document.ResourceId);
		if (!Material || !Material->GetPackage()) return false;
		Material->GetPackage()->ClearDirty();
		return true;
	}

	auto MMaterialEditor::IsDocumentDirty(const FEditorDocumentTab& Document) const -> bool
	{
		DMaterialInterface* Material = FindOpenMaterial(Document.ResourceId);
		return Material && Material->GetPackage() && Material->GetPackage()->IsDirty();
	}

	auto MMaterialEditor::CanSaveActiveDocument() const -> bool
	{
		DMaterialInterface* Material = GetActiveMaterial();
		return Material && Material->GetPackage();
	}

	auto MMaterialEditor::SaveActiveDocument() -> bool
	{
		return SaveMaterial(GetActiveMaterial());
	}

	auto MMaterialEditor::DrawWorkspace(bool bActive) -> bool
	{
		if (!bActive && PropertyView.IsEditing()) FinishActivePropertyEdit(true);
		return DocumentHost.DrawDocuments(
			WorkspaceManager,
			MaterialEditorWorkspace::Type,
			MaterialEditorWorkspace::RootKey,
			[this](const FEditorDocumentTab& Document) {
				return FindOpenMaterial(Document.ResourceId) != nullptr;
			},
			[this](const FEditorDocumentTab& Document) {
				DrawDocument(Document, FindOpenMaterial(Document.ResourceId));
			},
			[this](const FEditorDocumentTab& Document) {
				if (const auto PreviewIt = MaterialPreviews.find(Document.Id.Value); PreviewIt != MaterialPreviews.end())
					PreviewIt->second->SetVisible(false);
			}
		);
	}

	auto MMaterialEditor::ResetLayout() -> void
	{
		SidebarRatio = DefaultSidebarRatio;
		PreviewPaneRatio = DefaultPreviewPaneRatio;
	}

	auto MMaterialEditor::FindOpenMaterial(std::string_view ResourceId) const -> DMaterialInterface*
	{
		const auto It = OpenMaterials.find(std::string(ResourceId));
		return It == OpenMaterials.end() ? nullptr : It->second.Get();
	}

	auto MMaterialEditor::GetActiveMaterial() const -> DMaterialInterface*
	{
		return FindOpenMaterial(ActiveResourceId);
	}

	auto MMaterialEditor::SaveMaterial(DMaterialInterface* Material) -> bool
	{
		if (!Material || !Material->GetPackage()) return false;
		const Asset::FAssetResult Result = Asset::SavePackage(Material->GetPackage());
		if (!Result)
		{
			SetError(Result.Message);
			return false;
		}
		FAssetPath SavedPath;
		if (FAssetPath::TryCreate(Material->GetPackage()->GetPackagePath(), SavedPath) && GEditor)
			GEditor->GetAssetUpgradeAuditService().InvalidatePackage(SavedPath);
		return true;
	}

	auto MMaterialEditor::CanUndo() const -> bool
	{
		return GEditor && GEditor->GetTransactionManager().CanUndo();
	}

	auto MMaterialEditor::CanRedo() const -> bool
	{
		return GEditor && GEditor->GetTransactionManager().CanRedo();
	}

	auto MMaterialEditor::GetUndoDescription() const -> std::string_view
	{
		return CanUndo() ? GEditor->GetTransactionManager().GetUndoDescription() : std::string_view{};
	}

	auto MMaterialEditor::GetRedoDescription() const -> std::string_view
	{
		return CanRedo() ? GEditor->GetTransactionManager().GetRedoDescription() : std::string_view{};
	}

	auto MMaterialEditor::Undo() -> bool
	{
		return CanUndo() && GEditor->GetTransactionManager().Undo();
	}

	auto MMaterialEditor::Redo() -> bool
	{
		return CanRedo() && GEditor->GetTransactionManager().Redo();
	}

	auto MMaterialEditor::DrawDocument(const FEditorDocumentTab& Document, DMaterialInterface* Material) -> void
	{
		DrawToolbar(Document, Material);
		ImGui::Spacing();

		if (ImGui::GetContentRegionAvail().x >= MonaImGui::ScaleUI(WideLayoutMinimumWidth))
			DrawWideLayout(Document, Material);
		else
			DrawNarrowLayout(Document, Material);

		if (ActiveResourceId != Document.ResourceId) return;
		if (!ErrorMessage.empty()) ImGui::OpenPopup("Material Editor Error");
		if (ImGui::BeginPopupModal("Material Editor Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
		{
			ImGui::TextWrapped("%s", ErrorMessage.c_str());
			if (ImGui::Button("OK"))
			{
				ErrorMessage.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	auto MMaterialEditor::DrawToolbar(const FEditorDocumentTab& Document, DMaterialInterface* Material) -> void
	{
		if (ImGui::Button("Save")) SaveMaterial(Material);
		ImGui::SameLine();
		ImGui::TextDisabled("Material");
		ImGui::SameLine();
		ImGui::TextUnformatted(Document.ResourceId.c_str());
	}

	auto MMaterialEditor::DrawWideLayout(const FEditorDocumentTab& Document, DMaterialInterface* Material) -> void
	{
		const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
		const ImVec2 Available = ImGui::GetContentRegionAvail();
		const float ScaledMinimumSidebarWidth = MonaImGui::ScaleUI(MinimumSidebarWidth);
		const float ScaledMinimumDetailsWidth = MonaImGui::ScaleUI(MinimumDetailsWidth);
		const float SidebarWidth = std::clamp(
			Available.x * SidebarRatio,
			ScaledMinimumSidebarWidth,
			std::max(ScaledMinimumSidebarWidth, Available.x - Metrics.SplitterThickness - ScaledMinimumDetailsWidth));

		if (ImGui::BeginChild("MaterialEditorSidebar", ImVec2(SidebarWidth, Available.y)))
		{
			const float UsableHeight = std::max(Available.y - Metrics.SplitterThickness, 0.0f);
			const float ScaledMinimumPreviewHeight = MonaImGui::ScaleUI(MinimumPreviewHeight);
			const float ScaledMinimumOverviewHeight = MonaImGui::ScaleUI(MinimumOverviewHeight);
			if (UsableHeight >= ScaledMinimumPreviewHeight + ScaledMinimumOverviewHeight)
			{
				const float PreviewHeight = std::clamp(
					UsableHeight * PreviewPaneRatio,
					ScaledMinimumPreviewHeight,
					UsableHeight - ScaledMinimumOverviewHeight);

				DrawPreviewPanel(Document, Material, PreviewHeight);
				MonaImGui::DrawSplitter(
					"MaterialEditorPreviewSplitter",
					MonaImGui::EUISplitterAxis::Y,
					ImGui::GetContentRegionAvail().x,
					UsableHeight,
					ScaledMinimumPreviewHeight,
					ScaledMinimumOverviewHeight,
					PreviewPaneRatio);
				DrawOverviewPanel(Document, Material, 0.0f);
			}
			else
			{
				DrawPreviewPanel(Document, Material, 0.0f);
			}
		}
		ImGui::EndChild();
		ImGui::SameLine();
		MonaImGui::DrawSplitter(
			"MaterialEditorSidebarSplitter",
			MonaImGui::EUISplitterAxis::X,
			Available.y,
			Available.x,
			ScaledMinimumSidebarWidth,
			ScaledMinimumDetailsWidth,
			SidebarRatio);
		ImGui::SameLine();
		DrawDetailsPanel(Material, Available.y);
	}

	auto MMaterialEditor::DrawNarrowLayout(const FEditorDocumentTab& Document, DMaterialInterface* Material) -> void
	{
		DrawPreviewPanel(
			Document,
			Material,
			MonaImGui::ScaleUI(NarrowPreviewHeight));
		ImGui::Spacing();
		DrawOverviewPanel(
			Document,
			Material,
			MonaImGui::ScaleUI(MinimumOverviewHeight));
		ImGui::Spacing();
		DrawDetailsPanel(Material, 0.0f);
	}

	auto MMaterialEditor::DrawPreviewPanel(
		const FEditorDocumentTab& Document,
		DMaterialInterface* Material,
		float Height
	) -> void
	{
		std::unique_ptr<FMaterialPreview>& Preview = MaterialPreviews[Document.Id.Value];
		if (Preview == nullptr) Preview = std::make_unique<FMaterialPreview>(Document.Id.Value);
		Preview->Draw(Material, Height);
	}

	auto MMaterialEditor::DrawOverviewPanel(
		const FEditorDocumentTab& Document,
		DMaterialInterface* Material,
		float Height
	) -> void
	{
		if (ImGui::BeginChild("MaterialOverview", ImVec2(0.0f, Height), ImGuiChildFlags_Borders))
		{
			ImGui::SeparatorText("Material Overview");
			ImGui::TextDisabled("Asset");
			ImGui::TextWrapped("%s", Document.ResourceId.c_str());
			ImGui::Spacing();
			ImGui::TextDisabled("Type");
			ImGui::TextUnformatted(Material->GetClass()->GetQualifiedName().ToString().c_str());
		}
		ImGui::EndChild();
	}

	auto MMaterialEditor::DrawDetailsPanel(DMaterialInterface* Material, float Height) -> void
	{
		if (ImGui::BeginChild("MaterialDetails", ImVec2(0.0f, Height), ImGuiChildFlags_Borders))
		{
			ImGui::SeparatorText("Details");
			if (auto* Instance = Cast<DMaterialInstance>(Material)) DrawMaterialInstance(Instance);
			else if (auto* BaseMaterial = Cast<DMaterial>(Material)) DrawMaterial(BaseMaterial);
		}
		ImGui::EndChild();
	}

	auto MMaterialEditor::DrawMaterial(DMaterial* Material) -> void
	{
		ImGui::SeparatorText("Surface Parameters");
		if (!MonaImGui::PropertyEdit::BeginTable("MaterialParameters")) return;
		DrawMaterialParameters(Material);
		MonaImGui::PropertyEdit::EndTable();
	}

	auto MMaterialEditor::DrawMaterialInstance(DMaterialInstance* Instance) -> void
	{
		ImGui::SeparatorText("Inheritance");
		if (MonaImGui::PropertyEdit::BeginTable("MaterialInstanceParent"))
		{
			DrawParentPicker(Instance);
			MonaImGui::PropertyEdit::EndTable();
		}
		ImGui::SeparatorText("Parameter Overrides");
		if (!MonaImGui::PropertyEdit::BeginTable("MaterialInstanceParameters")) return;
		DrawMaterialParameters(Instance);
		MonaImGui::PropertyEdit::EndTable();
	}

	auto MMaterialEditor::DrawParentPicker(DMaterialInstance* Instance) -> void
	{
		ImGui::PushID("MaterialParent");
		MonaImGui::PropertyEdit::BeginRow("Parent");
		DMaterialInterface* Current = Instance->GetParent();
		const FEditorAssetPickerResult PickerResult = EditorAssetPicker::Draw({
			.ComboId = "##Parent",
			.SearchId = "##ParentSearch",
			.SearchHint = "Search materials...",
			.RequiredClass = DMaterialInterface::StaticClass(),
			.ClassPolicy = EEditorAssetClassPolicy::Derived,
			.CurrentSelection = Current,
			.SearchText = ParentSearchText,
			.bAllowNone = true,
			.AssignSelection = [this, Instance](DObject* Selection, std::string& OutError) {
				DMaterialInterface* Parent = Cast<DMaterialInterface>(Selection);
				if (Selection && !Parent)
				{
					OutError = "The selected asset is not a material.";
					return false;
				}
				FProperty* Property = Instance->GetClass()->FindPropertyByName(FName("Parent"));
				if (!Property)
				{
					OutError = "The reflected material parent property is unavailable.";
					return false;
				}
				const bool bAssigned = PropertyView.SubmitPropertyValueEdit(MakePropertyViewContext(),
					FReflectedPropertyEditTarget::ForMember(Instance, Property), [&](FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
					static_cast<FObjectProperty*>(ScratchProperty)->SetObjectPropertyValue(ScratchContainer, Parent, ScratchArrayIndex);
				}, false);
				if (!bAssigned && OutError.empty()) OutError = "Unable to assign the reflected material parent.";
				return bAssigned;
			},
		});
		if (!PickerResult.Error.empty()) SetError(PickerResult.Error);
		MonaImGui::PropertyEdit::EndRow();
		ImGui::PopID();
	}

	auto MMaterialEditor::DrawMaterialParameters(DMaterialInterface* Material) -> void
	{
		const FMaterialParameterPanelModel Model(Material);
		for (const FMaterialParameterPanelEntry& Entry : Model.GetEntries())
		{
			DrawMaterialParameter(Model, Entry);
		}
	}

	auto MMaterialEditor::DrawMaterialParameter(
		const FMaterialParameterPanelModel& Model,
		const FMaterialParameterPanelEntry& Entry
	) -> void
	{
		if (Entry.bOrphan)
		{
			DrawOrphanParameter(Model, Entry);
			return;
		}
		switch (Entry.Control)
		{
		case EMaterialParameterControlKind::Scalar:
		case EMaterialParameterControlKind::RangedScalar:
			DrawScalarParameter(Model, Entry);
			break;
		case EMaterialParameterControlKind::Color:
			DrawColorParameter(Model, Entry);
			break;
		case EMaterialParameterControlKind::AssetPicker:
			DrawTextureParameter(Model, Entry);
			break;
		case EMaterialParameterControlKind::Unsupported:
			MonaImGui::PropertyEdit::BeginRow(Entry.Definition->DisplayName.c_str(), true);
			ImGui::TextDisabled("<unsupported parameter presentation>");
			MonaImGui::PropertyEdit::EndRow(true);
			break;
		}
	}

	auto MMaterialEditor::DrawColorParameter(
		const FMaterialParameterPanelModel& Model,
		const FMaterialParameterPanelEntry& Entry
	) -> void
	{
		const FMaterialParameterDefinition& Definition = *Entry.Definition;
		FVector3 Value = Entry.Value.VectorValue;
		DMaterialInstance* Instance = Model.GetInstance();
		const std::string ParameterName = Definition.Name.ToString();
		bool bOverride = !Instance || Entry.bHasLocalOverride;
		ImGui::PushID(ParameterName.c_str());
		MonaImGui::PropertyEdit::BeginRow(Definition.DisplayName.c_str());
		if (Instance)
		{
			if (ImGui::Checkbox("##Override", &bOverride))
			{
				if (!Model.SetOverrideEnabled(PropertyView, MakePropertyViewContext(), Entry, bOverride))
					bOverride = !bOverride;
			}
			ImGui::SameLine();
			if (bOverride && ImGui::SmallButton("Reset"))
			{
				if (Model.SetOverrideEnabled(PropertyView, MakePropertyViewContext(), Entry, false))
					bOverride = false;
			}
			if (!bOverride) ImGui::BeginDisabled();
		}
		float Color[3] = {static_cast<float>(Value.x), static_cast<float>(Value.y), static_cast<float>(Value.z)};
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::ColorEdit3("##Value", Color, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_InputRGB) && bOverride)
		{
			FMaterialParameterValue Edited = Entry.Value;
			Edited.VectorValue = FVector3(Color[0], Color[1], Color[2]);
			if (!Model.SubmitValueEdit(PropertyView, MakePropertyViewContext(), Entry, Edited, true))
				SetError(std::format("The reflected {} parameter is unavailable.", Definition.DisplayName));
		}
		if (ImGui::IsItemDeactivatedAfterEdit() && PropertyView.IsEditing()) FinishActivePropertyEdit(false);
		else if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape) && PropertyView.IsEditing()) FinishActivePropertyEdit(true);
		if (Instance && !bOverride) ImGui::EndDisabled();
		if (Instance) ImGui::TextDisabled("%s", FormatParameterSource(Entry).c_str());
		MonaImGui::PropertyEdit::EndRow();
		ImGui::PopID();
	}

	auto MMaterialEditor::DrawScalarParameter(
		const FMaterialParameterPanelModel& Model,
		const FMaterialParameterPanelEntry& Entry
	) -> void
	{
		const FMaterialParameterDefinition& Definition = *Entry.Definition;
		float Value = Entry.Value.ScalarValue;
		DMaterialInstance* Instance = Model.GetInstance();
		const std::string ParameterName = Definition.Name.ToString();
		bool bOverride = !Instance || Entry.bHasLocalOverride;
		// The visible label belongs to the property-table column, while the actual controls use
		// hidden labels. Scope the complete row by parameter name so base materials and instances
		// both receive stable, distinct ImGui IDs.
		ImGui::PushID(ParameterName.c_str());
		MonaImGui::PropertyEdit::BeginRow(Definition.DisplayName.c_str());
		if (Instance)
		{
			if (ImGui::Checkbox("##Override", &bOverride))
			{
				if (!Model.SetOverrideEnabled(PropertyView, MakePropertyViewContext(), Entry, bOverride))
					bOverride = !bOverride;
			}
			ImGui::SameLine();
			if (bOverride && ImGui::SmallButton("Reset"))
			{
				if (Model.SetOverrideEnabled(PropertyView, MakePropertyViewContext(), Entry, false))
					bOverride = false;
			}
			if (!bOverride) ImGui::BeginDisabled();
		}
		ImGui::SetNextItemWidth(-FLT_MIN);
		const float Minimum = Definition.bHasRange ? Definition.MinimumValue : 0.0f;
		const float Maximum = Definition.bHasRange ? Definition.MaximumValue : 0.0f;
		const ImGuiSliderFlags Flags = Definition.bHasRange ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
		if (ImGui::DragFloat("##Value", &Value, 0.01f, Minimum, Maximum, "%.3f", Flags) && bOverride)
		{
			FMaterialParameterValue Edited = Entry.Value;
			Edited.ScalarValue = Value;
			if (!Model.SubmitValueEdit(PropertyView, MakePropertyViewContext(), Entry, Edited, true))
				SetError(std::format("The reflected {} parameter is unavailable.", Definition.DisplayName));
		}
		if (ImGui::IsItemDeactivatedAfterEdit() && PropertyView.IsEditing()) FinishActivePropertyEdit(false);
		else if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape) && PropertyView.IsEditing()) FinishActivePropertyEdit(true);
		if (Instance && !bOverride) ImGui::EndDisabled();
		if (Instance) ImGui::TextDisabled("%s", FormatParameterSource(Entry).c_str());
		MonaImGui::PropertyEdit::EndRow();
		ImGui::PopID();
	}

	auto MMaterialEditor::DrawTextureParameter(
		const FMaterialParameterPanelModel& Model,
		const FMaterialParameterPanelEntry& Entry
	) -> void
	{
		const FMaterialParameterDefinition& Definition = *Entry.Definition;
		DTexture2D* Texture = Entry.Value.TextureValue.Get();
		DMaterialInstance* Instance = Model.GetInstance();
		const std::string ParameterName = Definition.Name.ToString();
		bool bOverride = !Instance || Entry.bHasLocalOverride;
		ImGui::PushID(ParameterName.c_str());
		MonaImGui::PropertyEdit::BeginRow(Definition.DisplayName.c_str());
		if (Instance)
		{
			if (ImGui::Checkbox("##Override", &bOverride))
			{
				if (!Model.SetOverrideEnabled(PropertyView, MakePropertyViewContext(), Entry, bOverride))
					bOverride = !bOverride;
			}
			ImGui::SameLine();
			if (bOverride && ImGui::SmallButton("Reset"))
			{
				if (Model.SetOverrideEnabled(PropertyView, MakePropertyViewContext(), Entry, false))
					bOverride = false;
			}
		}
		if (!bOverride) ImGui::BeginDisabled();
		const FEditorAssetPickerResult PickerResult = EditorAssetPicker::Draw({
			.ComboId = "##Texture",
			.SearchId = "##TextureSearch",
			.SearchHint = "Search textures...",
			.RequiredClass = DTexture2D::StaticClass(),
			.ClassPolicy = EEditorAssetClassPolicy::Derived,
			.CurrentSelection = Texture,
			.SearchText = TextureSearchText,
			.bAllowNone = true,
			.AssignSelection = [this, &Model, Entry, Label = Definition.DisplayName](DObject* Selection, std::string& OutError) {
				DTexture2D* Selected = Cast<DTexture2D>(Selection);
				if (Selection && !Selected)
				{
					OutError = "The selected asset is not a texture.";
					return false;
				}
				FMaterialParameterValue Edited = Entry.Value;
				Edited.TextureValue = Selected;
				const bool bAssigned = Model.SubmitValueEdit(
					PropertyView, MakePropertyViewContext(), Entry, Edited, false);
				if (!bAssigned && OutError.empty()) OutError = "Unable to assign the reflected texture parameter.";
				return bAssigned;
			},
		});
		if (!bOverride) ImGui::EndDisabled();
		if (!PickerResult.Error.empty()) SetError(PickerResult.Error);
		if (Instance) ImGui::TextDisabled("%s", FormatParameterSource(Entry).c_str());
		MonaImGui::PropertyEdit::EndRow();
		ImGui::PopID();
	}

	auto MMaterialEditor::DrawOrphanParameter(
		const FMaterialParameterPanelModel& Model,
		const FMaterialParameterPanelEntry& Entry
	) -> void
	{
		const std::string Id = Entry.ParameterId.ToString();
		ImGui::PushID(Id.c_str());
		MonaImGui::PropertyEdit::BeginRow("Orphan Override");
		ImGui::TextDisabled("%s", Id.c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton("Remove"))
			Model.RemoveOrphan(PropertyView, MakePropertyViewContext(), Entry);
		MonaImGui::PropertyEdit::EndRow();
		ImGui::PopID();
	}

	auto MMaterialEditor::FinishActivePropertyEdit(bool bCancel) -> bool
	{
		const FReflectedPropertyViewContext Context = MakePropertyViewContext();
		return PropertyView.FinishActiveEdit(&Context, bCancel);
	}

	auto MMaterialEditor::MakePropertyViewContext() -> FReflectedPropertyViewContext
	{
		return {
			.Transactions = GEditor ? &GEditor->GetTransactionManager() : nullptr,
			.ReportError = [this](std::string Error) { SetError(std::move(Error)); },
		};
	}

	auto MMaterialEditor::SetError(std::string Message) -> void
	{
		ErrorMessage = std::move(Message);
		DURIN_ERROR("Material editor: {}", ErrorMessage);
	}
}
