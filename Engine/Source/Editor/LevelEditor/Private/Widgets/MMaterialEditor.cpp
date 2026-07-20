#include "Widgets/MMaterialEditor.h"

#include "AssetSystem.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "Editor/EditorWorkspaceUI.h"
#include "MaterialEditorWorkspace.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Math/Color.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"
#include "MonaImGuiPropertyTable.h"
#include "Texture/Texture2D.h"

namespace Durin
{
	namespace
	{
		using StringUtils::ContainsInsensitive;

		auto AssetPathOrNone(const DObject* Object) -> std::string
		{
			return Object && Object->GetPackage() ? Object->GetPackage()->GetPackagePath() : "None";
		}

		auto AssetLeaf(std::string_view ResourceId) -> std::string
		{
			const size_t Separator = ResourceId.find_last_of("/\\");
			return std::string(Separator == std::string_view::npos ? ResourceId : ResourceId.substr(Separator + 1));
		}

		auto IsClassChildOf(const DClass* Class, const DClass* Parent) -> bool
		{
			for (const DClass* Current = Class; Current != nullptr; Current = Current->GetSuperClass())
			{
				if (Current == Parent) return true;
			}
			return false;
		}
	}

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
		bFocusRequested = true;
	}

	auto MMaterialEditor::RequestCloseDocument(const FEditorDocumentTab& Document) -> bool
	{
		if (IsDocumentDirty(Document)) return false;
		OpenMaterials.erase(Document.ResourceId);
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
			if (ImGui::MenuItem("Save Material", "Ctrl+S", false, Material && Material->GetPackage())) SaveActiveMaterial();
			ImGui::EndMenu();
		}
	}

	auto MMaterialEditor::DrawWorkspace(bool bActive) -> bool
	{
		DMaterialInterface* Material = GetActiveMaterial();
		if (!Material) return false;

		EditorWorkspaceUI::SetNextEditorRootWindowClass();
		if (bFocusRequested)
		{
			ImGui::SetNextWindowFocus();
			bFocusRequested = false;
		}
		const bool bDirty = Material->GetPackage() && Material->GetPackage()->IsDirty();
		const std::string DisplayName = std::format("Material Editor - {}{}", AssetLeaf(ActiveResourceId), bDirty ? " *" : "");
		const std::string RootWindowName = EditorWorkspaceUI::MakeEditorRootWindowName(DisplayName, MaterialEditorWorkspace::RootKey);
		const bool bVisible = ImGui::Begin(RootWindowName.c_str(), nullptr, ImGuiWindowFlags_NoCollapse);
		const bool bFocused = bVisible && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows | ImGuiFocusedFlags_DockHierarchy);
		if (!bVisible)
		{
			ImGui::End();
			return false;
		}

		const ImGuiIO& IO = ImGui::GetIO();
		if ((bActive || bFocused) && IO.KeyCtrl && !IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_S, false)) SaveActiveMaterial();

		if (ImGui::Button("Save")) SaveActiveMaterial();
		ImGui::SameLine();
		if (ImGui::BeginCombo("##OpenMaterialDocuments", ActiveResourceId.c_str()))
		{
			for (const FEditorDocumentTab& Document : WorkspaceManager.GetDocuments())
			{
				if (Document.WorkspaceType != MaterialEditorWorkspace::Type) continue;
				if (ImGui::Selectable(Document.ResourceId.c_str(), Document.ResourceId == ActiveResourceId)) WorkspaceManager.ActivateDocument(Document.Id);
			}
			ImGui::EndCombo();
		}
		ImGui::Separator();
		ImGui::TextDisabled("Asset");
		ImGui::SameLine();
		ImGui::TextUnformatted(ActiveResourceId.c_str());
		ImGui::TextDisabled("Type");
		ImGui::SameLine();
		ImGui::TextUnformatted(Material->GetClass()->GetQualifiedName().ToString().c_str());
		ImGui::Spacing();

		if (auto* Instance = Cast<DMaterialInstance>(Material)) DrawMaterialInstance(Instance);
		else if (auto* BaseMaterial = Cast<DMaterial>(Material)) DrawMaterial(BaseMaterial);

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

		ImGui::End();
		return bFocused;
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

	auto MMaterialEditor::SaveActiveMaterial() -> bool
	{
		DMaterialInterface* Material = GetActiveMaterial();
		if (!Material || !Material->GetPackage()) return false;
		const Asset::FAssetResult Result = Asset::SavePackage(Material->GetPackage());
		if (!Result)
		{
			SetError(Result.Message);
			return false;
		}
		return true;
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
		MonaImGui::BeginPropertyRow("Parent");
		DMaterialInterface* Current = Instance->GetParent();
		const std::string Preview = AssetPathOrNone(Current);
		if (ImGui::BeginCombo("##Parent", Preview.c_str()))
		{
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::InputTextWithHint("##ParentSearch", "Search materials...", ParentSearchText.data(), ParentSearchText.size());
			if (ImGui::Selectable("None", Current == nullptr)) Instance->SetParent(nullptr);
			for (const auto& [Path, Data] : Asset::GetAssetRegistry().GetAssets())
			{
				DClass* AssetClass = FindClassByQualifiedName(Data.AssetClassName);
				const std::string PathString = Path.ToString();
				if (!AssetClass || !IsClassChildOf(AssetClass, DMaterialInterface::StaticClass()) || !ContainsInsensitive(PathString, ParentSearchText.data())) continue;
				if (ImGui::Selectable(PathString.c_str(), Current && Current->GetPackage() && Current->GetPackage()->GetPackagePath() == PathString))
				{
					DMaterialInterface* Parent = nullptr;
					const Asset::FAssetResult Result = Asset::LoadAsset(Path, Parent);
					if (!Result) SetError(Result.Message);
					else if (!Instance->SetParent(Parent)) SetError("A material instance cannot create a parent cycle.");
				}
			}
			ImGui::EndCombo();
		}
		MonaImGui::EndPropertyRow();
	}

	auto MMaterialEditor::DrawBaseColor(DMaterialInterface* Material, DMaterialInstance* Instance) -> void
	{
		FVector3 Value(0.95, 0.62, 0.22);
		Material->GetVectorParameterValue(MaterialParameterBaseColor, Value);
		bool bOverride = !Instance || Instance->HasVectorParameterOverride(MaterialParameterBaseColor);
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
	}

	auto MMaterialEditor::DrawScalarParameter(DMaterialInterface* Material, DMaterialInstance* Instance, std::string_view Name, const char* Label, float DefaultValue, float Minimum, float Maximum) -> void
	{
		float Value = DefaultValue;
		Material->GetScalarParameterValue(Name, Value);
		bool bOverride = !Instance || Instance->HasScalarParameterOverride(Name);
		MonaImGui::BeginPropertyRow(Label);
		if (Instance)
		{
			ImGui::PushID(Label);
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
			ImGui::PopID();
		}
		MonaImGui::EndPropertyRow();
	}

	auto MMaterialEditor::DrawBaseColorTexture(DMaterialInterface* Material, DMaterialInstance* Instance) -> void
	{
		DTexture2D* Texture = nullptr;
		Material->GetTextureParameterValue(MaterialParameterBaseColorTexture, Texture);
		bool bOverride = !Instance || Instance->HasTextureParameterOverride(MaterialParameterBaseColorTexture);
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
		DrawTexturePicker(Texture, bOverride, [Material, Instance](DTexture2D* Selected) {
			if (Instance) Instance->SetTextureParameterValue(MaterialParameterBaseColorTexture, Selected);
			else if (auto* BaseMaterial = Cast<DMaterial>(Material)) BaseMaterial->SetTextureParameterValue(MaterialParameterBaseColorTexture, Selected);
		});
		MonaImGui::EndPropertyRow();
	}

	auto MMaterialEditor::DrawTexturePicker(DTexture2D* CurrentTexture, bool bEnabled, const std::function<void(DTexture2D*)>& AssignTexture) -> void
	{
		if (!bEnabled) ImGui::BeginDisabled();
		const std::string Preview = AssetPathOrNone(CurrentTexture);
		if (ImGui::BeginCombo("##Texture", Preview.c_str()))
		{
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::InputTextWithHint("##TextureSearch", "Search textures...", TextureSearchText.data(), TextureSearchText.size());
			if (ImGui::Selectable("None", CurrentTexture == nullptr)) AssignTexture(nullptr);
			for (const auto& [Path, Data] : Asset::GetAssetRegistry().GetAssets())
			{
				DClass* AssetClass = FindClassByQualifiedName(Data.AssetClassName);
				const std::string PathString = Path.ToString();
				if (!AssetClass || !IsClassChildOf(AssetClass, DTexture2D::StaticClass()) || !ContainsInsensitive(PathString, TextureSearchText.data())) continue;
				if (ImGui::Selectable(PathString.c_str(), CurrentTexture && CurrentTexture->GetPackage() && CurrentTexture->GetPackage()->GetPackagePath() == PathString))
				{
					DTexture2D* Texture = nullptr;
					const Asset::FAssetResult Result = Asset::LoadAsset(Path, Texture);
					if (!Result) SetError(Result.Message);
					else AssignTexture(Texture);
				}
			}
			ImGui::EndCombo();
		}
		if (!bEnabled) ImGui::EndDisabled();
	}

	auto MMaterialEditor::SetError(std::string Message) -> void
	{
		ErrorMessage = std::move(Message);
		DURIN_ERROR("Material editor: {}", ErrorMessage);
	}
}
