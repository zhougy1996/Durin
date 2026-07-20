#include "Widgets/MMaterialEditor.h"

#include "AssetSystem.h"
#include "DObject/Package.h"
#include "Editor/EditorAssetPicker.h"
#include "Editor/EditorWorkspaceUI.h"
#include "MaterialEditorWorkspace.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Math/Color.h"
#include "MonaImGui.h"
#include "MonaImGuiPropertyTable.h"
#include "Texture/Texture2D.h"

namespace Durin
{
	MMaterialEditor::MMaterialEditor(FEditorWorkspaceManager& InWorkspaceManager)
		: WorkspaceManager(InWorkspaceManager)
	{
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
		if (FindOpenMaterial(Document.ResourceId)) ActiveResourceId = Document.ResourceId;
		DocumentWindows[Document.Id.Value].RequestFocus();
	}

	auto MMaterialEditor::RequestCloseDocument(const FEditorDocumentTab& Document) -> bool
	{
		if (IsDocumentDirty(Document)) return false;
		OpenMaterials.erase(Document.ResourceId);
		DocumentWindows.erase(Document.Id.Value);
		if (ActiveResourceId == Document.ResourceId) ActiveResourceId.clear();
		return true;
	}

	auto MMaterialEditor::IsDocumentDirty(const FEditorDocumentTab& Document) const -> bool
	{
		DMaterialInterface* Material = FindOpenMaterial(Document.ResourceId);
		return Material && Material->GetPackage() && Material->GetPackage()->IsDirty();
	}

	auto MMaterialEditor::DrawMainMenu() -> void
	{
		if (ImGui::BeginMenu("File"))
		{
			DMaterialInterface* Material = GetActiveMaterial();
			if (ImGui::MenuItem("Save Material", "Ctrl+S", false, Material && Material->GetPackage())) SaveMaterial(Material);
			ImGui::EndMenu();
		}
	}

	auto MMaterialEditor::DrawWorkspace(bool bActive) -> bool
	{
		bool bWorkspaceActivated = false;
		std::vector<FEditorDocumentId> CloseRequests;
		for (const FEditorDocumentTab& Document : WorkspaceManager.GetDocuments())
		{
			if (Document.WorkspaceType != MaterialEditorWorkspace::Type) continue;
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
				DrawDocument(Document, Material, bActive && ActiveResourceId == Document.ResourceId);
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

	auto MMaterialEditor::DrawDocument(const FEditorDocumentTab& Document, DMaterialInterface* Material, bool bActive) -> void
	{
		const ImGuiIO& IO = ImGui::GetIO();
		if (bActive && IO.KeyCtrl && !IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_S, false)) SaveMaterial(Material);
		if (ImGui::Button("Save")) SaveMaterial(Material);
		ImGui::Separator();
		ImGui::TextDisabled("Asset");
		ImGui::SameLine();
		ImGui::TextUnformatted(Document.ResourceId.c_str());
		ImGui::TextDisabled("Type");
		ImGui::SameLine();
		ImGui::TextUnformatted(Material->GetClass()->GetQualifiedName().ToString().c_str());
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
		DrawBaseColor(Material, nullptr);
		DrawBaseColorTexture(Material, nullptr);
		DrawScalarParameter(Material, nullptr, MaterialParameterOpacity, "Opacity", 1.0f, 0.0f, 1.0f);
		DrawScalarParameter(Material, nullptr, MaterialParameterSpecularStrength, "Specular Strength", 0.35f, 0.0f, 1.0f);
		DrawScalarParameter(Material, nullptr, MaterialParameterShininess, "Shininess", 32.0f, 1.0f, 256.0f);
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
		DrawBaseColor(Instance, Instance);
		DrawBaseColorTexture(Instance, Instance);
		DrawScalarParameter(Instance, Instance, MaterialParameterOpacity, "Opacity", 1.0f, 0.0f, 1.0f);
		DrawScalarParameter(Instance, Instance, MaterialParameterSpecularStrength, "Specular Strength", 0.35f, 0.0f, 1.0f);
		DrawScalarParameter(Instance, Instance, MaterialParameterShininess, "Shininess", 32.0f, 1.0f, 256.0f);
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
			.AssignSelection = [Instance](DObject* Selection, std::string& OutError) {
				DMaterialInterface* Parent = Cast<DMaterialInterface>(Selection);
				if (Selection && !Parent)
				{
					OutError = "The selected asset is not a material.";
					return false;
				}
				if (Instance->SetParent(Parent)) return true;
				OutError = "A material instance cannot create a parent cycle.";
				return false;
			},
		});
		if (!PickerResult.Error.empty()) SetError(PickerResult.Error);
		MonaImGui::EndPropertyRow();
		ImGui::PopID();
	}

	auto MMaterialEditor::DrawBaseColor(DMaterialInterface* Material, DMaterialInstance* Instance) -> void
	{
		FVector3 Value(0.95, 0.62, 0.22);
		Material->GetVectorParameterValue(MaterialParameterBaseColor, Value);
		bool bOverride = !Instance || Instance->HasVectorParameterOverride(MaterialParameterBaseColor);
		ImGui::PushID(MaterialParameterBaseColor.data(), MaterialParameterBaseColor.data() + MaterialParameterBaseColor.size());
		MonaImGui::BeginPropertyRow("Base Color");
		if (Instance)
		{
			if (ImGui::Checkbox("##BaseColorOverride", &bOverride))
			{
				if (bOverride) Instance->SetVectorParameterValue(MaterialParameterBaseColor, Value);
				else Instance->ClearVectorParameterValue(MaterialParameterBaseColor);
			}
			ImGui::SameLine();
			if (!bOverride) ImGui::BeginDisabled();
		}
		float Color[3] = {static_cast<float>(Value.x), static_cast<float>(Value.y), static_cast<float>(Value.z)};
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::ColorEdit3("##BaseColor", Color, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_InputRGB) && bOverride)
		{
			const FVector3 Edited(Color[0], Color[1], Color[2]);
			if (Instance) Instance->SetVectorParameterValue(MaterialParameterBaseColor, Edited);
			else if (auto* BaseMaterial = Cast<DMaterial>(Material)) BaseMaterial->SetVectorParameterValue(MaterialParameterBaseColor, Edited);
		}
		if (Instance && !bOverride) ImGui::EndDisabled();
		MonaImGui::EndPropertyRow();
		ImGui::PopID();
	}

	auto MMaterialEditor::DrawScalarParameter(DMaterialInterface* Material, DMaterialInstance* Instance, std::string_view Name, const char* Label, float DefaultValue, float Minimum, float Maximum) -> void
	{
		float Value = DefaultValue;
		Material->GetScalarParameterValue(Name, Value);
		bool bOverride = !Instance || Instance->HasScalarParameterOverride(Name);
		// The visible label belongs to the property-table column, while the actual controls use
		// hidden labels. Scope the complete row by parameter name so base materials and instances
		// both receive stable, distinct ImGui IDs.
		ImGui::PushID(Name.data(), Name.data() + Name.size());
		MonaImGui::BeginPropertyRow(Label);
		if (Instance)
		{
			if (ImGui::Checkbox("##Override", &bOverride))
			{
				if (bOverride) Instance->SetScalarParameterValue(Name, Value);
				else Instance->ClearScalarParameterValue(Name);
			}
			ImGui::SameLine();
			if (!bOverride) ImGui::BeginDisabled();
		}
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::DragFloat("##Value", &Value, 0.01f, Minimum, Maximum, "%.3f", ImGuiSliderFlags_AlwaysClamp) && bOverride)
		{
			if (Instance) Instance->SetScalarParameterValue(Name, Value);
			else if (auto* BaseMaterial = Cast<DMaterial>(Material)) BaseMaterial->SetScalarParameterValue(Name, Value);
		}
		if (Instance)
		{
			if (!bOverride) ImGui::EndDisabled();
		}
		MonaImGui::EndPropertyRow();
		ImGui::PopID();
	}

	auto MMaterialEditor::DrawBaseColorTexture(DMaterialInterface* Material, DMaterialInstance* Instance) -> void
	{
		DTexture2D* Texture = nullptr;
		Material->GetTextureParameterValue(MaterialParameterBaseColorTexture, Texture);
		bool bOverride = !Instance || Instance->HasTextureParameterOverride(MaterialParameterBaseColorTexture);
		ImGui::PushID(MaterialParameterBaseColorTexture.data(), MaterialParameterBaseColorTexture.data() + MaterialParameterBaseColorTexture.size());
		MonaImGui::BeginPropertyRow("Base Color Texture");
		if (Instance)
		{
			if (ImGui::Checkbox("##BaseColorTextureOverride", &bOverride))
			{
				if (bOverride) Instance->SetTextureParameterValue(MaterialParameterBaseColorTexture, Texture);
				else Instance->ClearTextureParameterValue(MaterialParameterBaseColorTexture);
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
			.AssignSelection = [Material, Instance](DObject* Selection, std::string& OutError) {
				DTexture2D* Selected = Cast<DTexture2D>(Selection);
				if (Selection && !Selected)
				{
					OutError = "The selected asset is not a texture.";
					return false;
				}
				if (Instance) Instance->SetTextureParameterValue(MaterialParameterBaseColorTexture, Selected);
				else if (auto* BaseMaterial = Cast<DMaterial>(Material)) BaseMaterial->SetTextureParameterValue(MaterialParameterBaseColorTexture, Selected);
				return true;
			},
		});
		if (!bOverride) ImGui::EndDisabled();
		if (!PickerResult.Error.empty()) SetError(PickerResult.Error);
		MonaImGui::EndPropertyRow();
		ImGui::PopID();
	}

	auto MMaterialEditor::SetError(std::string Message) -> void
	{
		ErrorMessage = std::move(Message);
		DURIN_ERROR("Material editor: {}", ErrorMessage);
	}
}
