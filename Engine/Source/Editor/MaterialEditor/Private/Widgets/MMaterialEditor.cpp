#include "Widgets/MMaterialEditor.h"
#include "Widgets/MaterialPreview.h"

#include "AssetSystem.h"
#include "DObject/Package.h"
#include "DObject/DurinPropertyTypes.h"
#include "Editor/EditorAssetPicker.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorWorkspaceUI.h"
#include "MaterialParameterDescriptors.h"
#include "MaterialEditorWorkspace.h"
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
		auto FindParameterMap(DObject* Object, FName PropertyName) -> FMapProperty*
		{
			FProperty* Property = Object ? Object->GetClass()->FindPropertyByName(PropertyName) : nullptr;
			return Property && Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Map
				? static_cast<FMapProperty*>(Property) : nullptr;
		}

	}

	MMaterialEditor::MMaterialEditor(FEditorWorkspaceManager& InWorkspaceManager)
		: WorkspaceManager(InWorkspaceManager)
	{
	}

	MMaterialEditor::~MMaterialEditor()
	{
		PropertyView.FinishActiveEdit(nullptr, true);
		MaterialPreviews.clear();
	}

	auto MMaterialEditor::GetWorkspaceType() const -> const FEditorWorkspaceTypeId&
	{
		return MaterialEditorWorkspace::Type;
	}

	auto MMaterialEditor::OpenDocument(const FEditorDocumentTab& Document) -> bool
	{
		if (Document.ResourceId.empty()) return false;
		if (FindOpenMaterial(Document.ResourceId)) return true;
		FAssetPath AssetPath;
		std::string PathError;
		if (!FAssetPath::TryCreate(Document.ResourceId, AssetPath, &PathError))
		{
			SetError(std::move(PathError));
			return false;
		}
		DMaterialInterface* Material = nullptr;
		const Asset::FAssetResult Result = Asset::LoadAsset(AssetPath, Material);
		if (!Result || !Material)
		{
			SetError(Result ? "The selected asset is not a material." : Result.Message);
			return false;
		}
		OpenMaterials.emplace(Document.ResourceId, Material);
		return true;
	}

	auto MMaterialEditor::ActivateDocument(const FEditorDocumentTab& Document) -> void
	{
		DMaterialInterface* Material = FindOpenMaterial(Document.ResourceId);
		if (PropertyView.IsEditing() && !PropertyView.IsEditingObject(Material)) FinishActivePropertyEdit(false);
		if (Material) ActiveResourceId = Document.ResourceId;
		DocumentWindows[Document.Id.Value].RequestFocus();
	}

	auto MMaterialEditor::RequestCloseDocument(const FEditorDocumentTab& Document) -> bool
	{
		if (PropertyView.IsEditingObject(FindOpenMaterial(Document.ResourceId))) FinishActivePropertyEdit(false);
		if (IsDocumentDirty(Document)) return false;
		OpenMaterials.erase(Document.ResourceId);
		DocumentWindows.erase(Document.Id.Value);
		MaterialPreviews.erase(Document.Id.Value);
		if (ActiveResourceId == Document.ResourceId) ActiveResourceId.clear();
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
		if (!bActive && PropertyView.IsEditing()) FinishActivePropertyEdit(false);
		bool bWorkspaceActivated = false;
		std::vector<FEditorDocumentId> CloseRequests;
		for (const FEditorDocumentTab& Document : WorkspaceManager.GetDocuments())
		{
			if (Document.WorkspaceType != MaterialEditorWorkspace::Type) continue;
			if (const auto PreviewIt = MaterialPreviews.find(Document.Id.Value); PreviewIt != MaterialPreviews.end())
			{
				PreviewIt->second->SetVisible(false);
			}
			DMaterialInterface* Material = FindOpenMaterial(Document.ResourceId);
			if (!Material) continue;
			FEditorWorkspaceRootWindow& RootWindow = DocumentWindows[Document.Id.Value];
			const FEditorWorkspaceRootWindowState WindowState = RootWindow.Begin({
				.DisplayName = Document.Label,
				.RootKey = EditorWorkspaceUI::MakeEditorDocumentRootKey(MaterialEditorWorkspace::RootKey, Document.DocumentKey),
				.bDirty = Material->GetPackage() && Material->GetPackage()->IsDirty(),
			});
			if (WindowState.bFocused || WindowState.bActivated)
			{
				bWorkspaceActivated = true;
				if (ActiveResourceId != Document.ResourceId) WorkspaceManager.ActivateDocument(Document.Id);
			}
			if (WindowState.bVisible)
				DrawDocument(Document, Material);
			RootWindow.End();
			if (WindowState.bCloseRequested) CloseRequests.push_back(Document.Id);
		}
		// Closing mutates the document array, so defer it until iteration is complete.
		for (FEditorDocumentId DocumentId : CloseRequests) WorkspaceManager.RequestCloseDocument(DocumentId);
		return bWorkspaceActivated;
	}

	auto MMaterialEditor::ResetLayout() -> void
	{
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
		return true;
	}

	auto MMaterialEditor::DrawDocument(const FEditorDocumentTab& Document, DMaterialInterface* Material) -> void
	{
		if (ImGui::Button("Save")) SaveMaterial(Material);
		ImGui::Separator();
		ImGui::TextDisabled("Asset");
		ImGui::SameLine();
		ImGui::TextUnformatted(Document.ResourceId.c_str());
		ImGui::TextDisabled("Type");
		ImGui::SameLine();
		ImGui::TextUnformatted(Material->GetClass()->GetQualifiedName().ToString().c_str());
		ImGui::Spacing();
		std::unique_ptr<FMaterialPreview>& Preview = MaterialPreviews[Document.Id.Value];
		if (Preview == nullptr) Preview = std::make_unique<FMaterialPreview>(Document.Id.Value);
		Preview->Draw(Material);
		ImGui::Spacing();
		if (auto* Instance = Cast<DMaterialInstance>(Material)) DrawMaterialInstance(Instance);
		else if (auto* BaseMaterial = Cast<DMaterial>(Material)) DrawMaterial(BaseMaterial);

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

	auto MMaterialEditor::DrawMaterial(DMaterial* Material) -> void
	{
		ImGui::SeparatorText("Surface Parameters");
		if (!MonaImGui::BeginPropertyTable("MaterialParameters")) return;
		DrawMaterialParameters(Material, nullptr);
		MonaImGui::EndPropertyTable();
	}

	auto MMaterialEditor::DrawMaterialInstance(DMaterialInstance* Instance) -> void
	{
		ImGui::SeparatorText("Inheritance");
		if (MonaImGui::BeginPropertyTable("MaterialInstanceParent"))
		{
			DrawParentPicker(Instance);
			MonaImGui::EndPropertyTable();
		}
		ImGui::SeparatorText("Parameter Overrides");
		if (!MonaImGui::BeginPropertyTable("MaterialInstanceParameters")) return;
		DrawMaterialParameters(Instance, Instance);
		MonaImGui::EndPropertyTable();
	}

	auto MMaterialEditor::DrawParentPicker(DMaterialInstance* Instance) -> void
	{
		ImGui::PushID("MaterialParent");
		MonaImGui::BeginPropertyRow("Parent");
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
		MonaImGui::EndPropertyRow();
		ImGui::PopID();
	}

	auto MMaterialEditor::DrawMaterialParameters(DMaterialInterface* Material, DMaterialInstance* Instance) -> void
	{
		for (const FMaterialParameterDescriptor& Descriptor : MaterialParameterDescriptors)
		{
			DrawMaterialParameter(Material, Instance, Descriptor);
		}
	}

	auto MMaterialEditor::DrawMaterialParameter(DMaterialInterface* Material, DMaterialInstance* Instance,
		const FMaterialParameterDescriptor& Descriptor) -> void
	{
		switch (Descriptor.Presentation)
		{
		case EMaterialParameterPresentation::Drag: DrawScalarParameter(Material, Instance, Descriptor); break;
		case EMaterialParameterPresentation::Color: DrawColorParameter(Material, Instance, Descriptor); break;
		case EMaterialParameterPresentation::AssetPicker: DrawTextureParameter(Material, Instance, Descriptor); break;
		}
	}

	auto MMaterialEditor::DrawColorParameter(DMaterialInterface* Material, DMaterialInstance* Instance,
		const FMaterialParameterDescriptor& Descriptor) -> void
	{
		FVector3 Value(Descriptor.DefaultValue[0], Descriptor.DefaultValue[1], Descriptor.DefaultValue[2]);
		Material->GetVectorParameterValue(Descriptor.Name, Value);
		auto* Property = FindParameterMap(Material, FName(GetMaterialParameterMapName(Descriptor.ValueType, Instance != nullptr)));
		const FReflectedPropertyBinding Binding = PropertyView.BindStringMapValue(Material, Property, Descriptor.Name);
		bool bOverride = !Instance || Binding.IsPresent();
		ImGui::PushID(Descriptor.Name.data(), Descriptor.Name.data() + Descriptor.Name.size());
		MonaImGui::BeginPropertyRow(Descriptor.Label);
		if (Instance)
		{
			if (ImGui::Checkbox("##Override", &bOverride))
			{
				if (!PropertyView.SetBoundPropertyEnabled(MakePropertyViewContext(), Binding, bOverride,
					[&](FProperty* ValueProperty, void* Container) {
						*ValueProperty->ContainerPtrToValuePtr<FVector3>(Container) = Value;
					}))
				{
					bOverride = !bOverride;
				}
			}
			ImGui::SameLine();
			if (!bOverride) ImGui::BeginDisabled();
		}
		float Color[3] = {static_cast<float>(Value.x), static_cast<float>(Value.y), static_cast<float>(Value.z)};
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::ColorEdit3("##Value", Color, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_InputRGB) && bOverride)
		{
			const FVector3 Edited(Color[0], Color[1], Color[2]);
			if (!Binding.IsValid()) SetError(std::format("The reflected {} parameter is unavailable.", Descriptor.Label));
			else
			{
				PropertyView.SubmitBoundPropertyValueEdit(MakePropertyViewContext(), Binding,
					[&](FProperty* ValueProperty, void* Container) {
						*ValueProperty->ContainerPtrToValuePtr<FVector3>(Container) = Edited;
					}, true);
			}
		}
		if (ImGui::IsItemDeactivatedAfterEdit() && PropertyView.IsEditing()) FinishActivePropertyEdit(false);
		else if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape) && PropertyView.IsEditing()) FinishActivePropertyEdit(true);
		if (Instance && !bOverride) ImGui::EndDisabled();
		MonaImGui::EndPropertyRow();
		ImGui::PopID();
	}

	auto MMaterialEditor::DrawScalarParameter(DMaterialInterface* Material, DMaterialInstance* Instance,
		const FMaterialParameterDescriptor& Descriptor) -> void
	{
		float Value = Descriptor.DefaultValue[0];
		Material->GetScalarParameterValue(Descriptor.Name, Value);
		auto* Property = FindParameterMap(Material, FName(GetMaterialParameterMapName(Descriptor.ValueType, Instance != nullptr)));
		const FReflectedPropertyBinding Binding = PropertyView.BindStringMapValue(Material, Property, Descriptor.Name);
		bool bOverride = !Instance || Binding.IsPresent();
		// The visible label belongs to the property-table column, while the actual controls use
		// hidden labels. Scope the complete row by parameter name so base materials and instances
		// both receive stable, distinct ImGui IDs.
		ImGui::PushID(Descriptor.Name.data(), Descriptor.Name.data() + Descriptor.Name.size());
		MonaImGui::BeginPropertyRow(Descriptor.Label);
		if (Instance)
		{
			if (ImGui::Checkbox("##Override", &bOverride))
			{
				if (!PropertyView.SetBoundPropertyEnabled(MakePropertyViewContext(), Binding, bOverride,
					[&](FProperty* ValueProperty, void* Container) {
						*ValueProperty->ContainerPtrToValuePtr<float>(Container) = Value;
					}))
				{
					bOverride = !bOverride;
				}
			}
			ImGui::SameLine();
			if (!bOverride) ImGui::BeginDisabled();
		}
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::DragFloat("##Value", &Value, 0.01f, Descriptor.Minimum, Descriptor.Maximum, "%.3f", ImGuiSliderFlags_AlwaysClamp) && bOverride)
		{
			if (!Binding.IsValid()) SetError(std::format("The reflected {} parameter is unavailable.", Descriptor.Label));
			else
			{
				PropertyView.SubmitBoundPropertyValueEdit(MakePropertyViewContext(), Binding,
					[&](FProperty* ValueProperty, void* Container) {
						*ValueProperty->ContainerPtrToValuePtr<float>(Container) = Value;
					}, true);
			}
		}
		if (ImGui::IsItemDeactivatedAfterEdit() && PropertyView.IsEditing()) FinishActivePropertyEdit(false);
		else if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape) && PropertyView.IsEditing()) FinishActivePropertyEdit(true);
		if (Instance)
		{
			if (!bOverride) ImGui::EndDisabled();
		}
		MonaImGui::EndPropertyRow();
		ImGui::PopID();
	}

	auto MMaterialEditor::DrawTextureParameter(DMaterialInterface* Material, DMaterialInstance* Instance,
		const FMaterialParameterDescriptor& Descriptor) -> void
	{
		DTexture2D* Texture = nullptr;
		Material->GetTextureParameterValue(Descriptor.Name, Texture);
		auto* Property = FindParameterMap(Material, FName(GetMaterialParameterMapName(Descriptor.ValueType, Instance != nullptr)));
		const FReflectedPropertyBinding Binding = PropertyView.BindStringMapValue(Material, Property, Descriptor.Name);
		bool bOverride = !Instance || Binding.IsPresent();
		ImGui::PushID(Descriptor.Name.data(), Descriptor.Name.data() + Descriptor.Name.size());
		MonaImGui::BeginPropertyRow(Descriptor.Label);
		if (Instance)
		{
			if (ImGui::Checkbox("##Override", &bOverride))
			{
				if (!PropertyView.SetBoundPropertyEnabled(MakePropertyViewContext(), Binding, bOverride,
					[&](FProperty* ValueProperty, void* Container) {
						static_cast<FObjectProperty*>(ValueProperty)->SetObjectPropertyValue(Container, Texture);
					}))
				{
					bOverride = !bOverride;
				}
			}
			ImGui::SameLine();
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
			.AssignSelection = [this, Binding, Label = std::string(Descriptor.Label)](DObject* Selection, std::string& OutError) {
				DTexture2D* Selected = Cast<DTexture2D>(Selection);
				if (Selection && !Selected)
				{
					OutError = "The selected asset is not a texture.";
					return false;
				}
				if (!Binding.IsValid())
				{
					OutError = std::format("The reflected {} parameter is unavailable.", Label);
					return false;
				}
				const bool bAssigned = PropertyView.SubmitBoundPropertyValueEdit(MakePropertyViewContext(), Binding,
					[&](FProperty* ValueProperty, void* Container) {
						static_cast<FObjectProperty*>(ValueProperty)->SetObjectPropertyValue(Container, Selected);
					}, false);
				if (!bAssigned && OutError.empty()) OutError = "Unable to assign the reflected texture parameter.";
				return bAssigned;
			},
		});
		if (!bOverride) ImGui::EndDisabled();
		if (!PickerResult.Error.empty()) SetError(PickerResult.Error);
		MonaImGui::EndPropertyRow();
		ImGui::PopID();
	}

	auto MMaterialEditor::FinishActivePropertyEdit(bool bCancel) -> void
	{
		const FReflectedPropertyViewContext Context = MakePropertyViewContext();
		PropertyView.FinishActiveEdit(&Context, bCancel);
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
