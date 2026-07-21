#include "Widgets/MMaterialEditor.h"
#include "Widgets/MaterialPreview.h"

#include "AssetSystem.h"
#include "DObject/Package.h"
#include "DObject/DurinPropertyTypes.h"
#include "Editor/EditorAssetPicker.h"
#include "Editor/EditorEngine.h"
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
	namespace
	{
		auto CaptureProposedValue(
			const FProperty* Property,
			void* Container,
			uint32 ArrayIndex,
			const std::function<void()>& Mutation,
			FPropertyValueSnapshot& OutSnapshot,
			std::string* OutError
		) -> bool
		{
			FPropertyValueSnapshot Original;
			if (!CapturePropertyValue(Property, Container, ArrayIndex, Original, OutError)) return false;
			Mutation();
			const bool bCaptured = CapturePropertyValue(Property, Container, ArrayIndex, OutSnapshot, OutError);
			std::string RestoreError;
			if (!RestorePropertyValue(Property, Container, ArrayIndex, Original, &RestoreError))
			{
				if (OutError) *OutError = std::move(RestoreError);
				return false;
			}
			return bCaptured;
		}

		auto FindParameterMap(DObject* Object, FName PropertyName) -> FMapProperty*
		{
			FProperty* Property = Object ? Object->GetClass()->FindPropertyByName(PropertyName) : nullptr;
			return Property && Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Map
				? static_cast<FMapProperty*>(Property) : nullptr;
		}

		auto FindParameterIndex(const FMapProperty* Property, const DObject* Object, std::string_view Name) -> uint64
		{
			if (!Property || !Property->GetKeyProp() || Property->GetKeyProp()->GetKind() != DurinCodeGen::EPropertyGenFlags::String) return UINT64_MAX;
			auto* KeyProperty = static_cast<FStringProperty*>(Property->GetKeyProp());
			const uint64 Num = Property->Num(Object);
			for (uint64 Index = 0; Index < Num; ++Index)
			{
				const void* Key = Property->GetKeyPtr(Object, Index);
				if (Key && *KeyProperty->GetStringValuePtr(Key) == Name) return Index;
			}
			return UINT64_MAX;
		}

		auto CaptureMapKey(const FMapProperty* Property, const void* Key) -> std::vector<uint8>
		{
			FPropertyValueSnapshot Snapshot;
			if (!Property || !CapturePropertyValue(Property->GetKeyProp(), Key, 0, Snapshot)) return {};
			return Snapshot.GetBytes();
		}

		auto MakeParameterTarget(DObject* Object, FMapProperty* Property, uint64 Index) -> FReflectedPropertyEditTarget
		{
			FReflectedPropertyEditTarget Target = FReflectedPropertyEditTarget::ForMember(Object, Property);
			const void* Key = Property->GetKeyPtr(Object, Index);
			// The proposal snapshot restores the whole map before the session starts,
			// so an entry address is not stable even for non-structural value edits.
			return Target.ForMapEntry(Property->GetValueProp(), Object, CaptureMapKey(Property, Key));
		}

		auto MakeStructuralParameterTarget(DObject* Object, FMapProperty* Property, const void* Key, EPropertyChangeKind Kind) -> FReflectedPropertyEditTarget
		{
			FReflectedPropertyEditTarget Target = FReflectedPropertyEditTarget::ForMember(Object, Property);
			Target.Path.back().Selector = EPropertyPathSelector::MapKey;
			Target.Path.back().MapKeyData = CaptureMapKey(Property, Key);
			Target.Kind = Kind;
			return Target;
		}

		auto CaptureOverrideProposal(
			DObject* Object,
			FMapProperty* Property,
			std::string_view Name,
			bool bEnable,
			const std::function<void(FProperty*, void*)>& InitializeValue,
			FReflectedPropertyEditTarget& OutTarget,
			FPropertyValueSnapshot& OutSnapshot,
			std::string* OutError
		) -> bool
		{
			void* Key = Property->CreateKey();
			void* Value = Property->CreateValue();
			if (!Key || !Value)
			{
				if (OutError) *OutError = "Unable to create a material parameter override.";
				if (Key) Property->DestroyKey(Key);
				if (Value) Property->DestroyValue(Value);
				return false;
			}
			*static_cast<FStringProperty*>(Property->GetKeyProp())->GetStringValuePtr(Key) = Name;
			InitializeValue(Property->GetValueProp(), Value);
			OutTarget = MakeStructuralParameterTarget(Object, Property, Key,
				bEnable ? EPropertyChangeKind::MapInsert : EPropertyChangeKind::MapRemove);
			const bool bCaptured = CaptureProposedValue(Property, Object, 0, [&] {
				if (bEnable) Property->Insert(Object, Key, Value);
				else Property->Remove(Object, Key);
			}, OutSnapshot, OutError);
			Property->DestroyKey(Key);
			Property->DestroyValue(Value);
			return bCaptured;
		}

		auto CaptureParameterValueProposal(
			DObject* Object,
			FMapProperty* Property,
			std::string_view Name,
			const std::function<void(FProperty*, void*)>& AssignValue,
			FReflectedPropertyEditTarget& OutTarget,
			FPropertyValueSnapshot& OutSnapshot,
			std::string* OutError
		) -> bool
		{
			const uint64 Index = FindParameterIndex(Property, Object, Name);
			if (Index != UINT64_MAX)
			{
				OutTarget = MakeParameterTarget(Object, Property, Index);
				return CaptureProposedValue(Property, Object, 0, [&] {
					AssignValue(Property->GetValueProp(), Property->GetMutableMappedValuePtr(Object, Index));
				}, OutSnapshot, OutError);
			}

			void* Key = Property->CreateKey();
			void* Value = Property->CreateValue();
			if (!Key || !Value)
			{
				if (OutError) *OutError = "Unable to create a material parameter value.";
				if (Key) Property->DestroyKey(Key);
				if (Value) Property->DestroyValue(Value);
				return false;
			}
			*static_cast<FStringProperty*>(Property->GetKeyProp())->GetStringValuePtr(Key) = Name;
			AssignValue(Property->GetValueProp(), Value);
			OutTarget = FReflectedPropertyEditTarget::ForMember(Object, Property)
				.ForMapEntry(Property->GetValueProp(), Object, CaptureMapKey(Property, Key));
			// Named base parameters conceptually exist even in assets serialized before
			// a default was introduced, so their first assignment remains ValueSet.
			const bool bCaptured = CaptureProposedValue(Property, Object, 0, [&] {
				Property->Insert(Object, Key, Value);
			}, OutSnapshot, OutError);
			Property->DestroyKey(Key);
			Property->DestroyValue(Value);
			return bCaptured;
		}
	}

	MMaterialEditor::MMaterialEditor(FEditorWorkspaceManager& InWorkspaceManager)
		: WorkspaceManager(InWorkspaceManager)
	{
	}

	MMaterialEditor::~MMaterialEditor()
	{
		if (PropertyEditSession.IsActive()) PropertyEditSession.Cancel();
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
		if (PropertyEditSession.IsActive() && ActiveEditObject != Material) FinishActivePropertyEdit(false);
		if (Material) ActiveResourceId = Document.ResourceId;
		DocumentWindows[Document.Id.Value].RequestFocus();
	}

	auto MMaterialEditor::RequestCloseDocument(const FEditorDocumentTab& Document) -> bool
	{
		if (PropertyEditSession.IsActive() && ActiveEditObject == FindOpenMaterial(Document.ResourceId)) FinishActivePropertyEdit(false);
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
		if (!bActive && PropertyEditSession.IsActive()) FinishActivePropertyEdit(false);
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
				FPropertyValueSnapshot Proposed;
				if (!CaptureProposedValue(Property, Instance, 0, [&] {
					static_cast<FObjectProperty*>(Property)->SetObjectPropertyValue(Instance, Parent);
				}, Proposed, &OutError)) return false;
				return SubmitPropertyEdit(FReflectedPropertyEditTarget::ForMember(Instance, Property), Proposed, false);
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
				auto* Property = FindParameterMap(Instance, FName("VectorParameterOverrides"));
				FReflectedPropertyEditTarget Target;
				FPropertyValueSnapshot Proposed;
				std::string Error;
				if (!Property || !CaptureOverrideProposal(Instance, Property, MaterialParameterBaseColor, bOverride,
					[&](FProperty* ValueProperty, void* Container) {
						*ValueProperty->ContainerPtrToValuePtr<FVector3>(Container) = Value;
					}, Target, Proposed, &Error) || !SubmitPropertyEdit(Target, Proposed, false))
				{
					if (!Error.empty()) SetError(std::move(Error));
					bOverride = !bOverride;
				}
			}
			ImGui::SameLine();
			if (!bOverride) ImGui::BeginDisabled();
		}
		float Color[3] = {static_cast<float>(Value.x), static_cast<float>(Value.y), static_cast<float>(Value.z)};
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::ColorEdit3("##BaseColor", Color, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_InputRGB) && bOverride)
		{
			const FVector3 Edited(Color[0], Color[1], Color[2]);
			auto* Property = FindParameterMap(Material, FName(Instance ? "VectorParameterOverrides" : "VectorParameters"));
			if (!Property) SetError("The reflected base-color parameter is unavailable.");
			else
			{
				FReflectedPropertyEditTarget Target;
				FPropertyValueSnapshot Proposed;
				std::string Error;
				if (!CaptureParameterValueProposal(Material, Property, MaterialParameterBaseColor,
					[&](FProperty* ValueProperty, void* Container) {
						*ValueProperty->ContainerPtrToValuePtr<FVector3>(Container) = Edited;
					}, Target, Proposed, &Error)) SetError(std::move(Error));
				else SubmitPropertyEdit(Target, Proposed, true);
			}
		}
		if (ImGui::IsItemDeactivatedAfterEdit() && PropertyEditSession.IsActive()) FinishActivePropertyEdit(false);
		else if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape) && PropertyEditSession.IsActive()) FinishActivePropertyEdit(true);
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
				auto* Property = FindParameterMap(Instance, FName("ScalarParameterOverrides"));
				FReflectedPropertyEditTarget Target;
				FPropertyValueSnapshot Proposed;
				std::string Error;
				if (!Property || !CaptureOverrideProposal(Instance, Property, Name, bOverride,
					[&](FProperty* ValueProperty, void* Container) {
						*ValueProperty->ContainerPtrToValuePtr<float>(Container) = Value;
					}, Target, Proposed, &Error) || !SubmitPropertyEdit(Target, Proposed, false))
				{
					if (!Error.empty()) SetError(std::move(Error));
					bOverride = !bOverride;
				}
			}
			ImGui::SameLine();
			if (!bOverride) ImGui::BeginDisabled();
		}
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::DragFloat("##Value", &Value, 0.01f, Minimum, Maximum, "%.3f", ImGuiSliderFlags_AlwaysClamp) && bOverride)
		{
			auto* Property = FindParameterMap(Material, FName(Instance ? "ScalarParameterOverrides" : "ScalarParameters"));
			if (!Property) SetError("The reflected scalar parameter is unavailable.");
			else
			{
				FReflectedPropertyEditTarget Target;
				FPropertyValueSnapshot Proposed;
				std::string Error;
				if (!CaptureParameterValueProposal(Material, Property, Name,
					[&](FProperty* ValueProperty, void* Container) {
						*ValueProperty->ContainerPtrToValuePtr<float>(Container) = Value;
					}, Target, Proposed, &Error)) SetError(std::move(Error));
				else SubmitPropertyEdit(Target, Proposed, true);
			}
		}
		if (ImGui::IsItemDeactivatedAfterEdit() && PropertyEditSession.IsActive()) FinishActivePropertyEdit(false);
		else if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape) && PropertyEditSession.IsActive()) FinishActivePropertyEdit(true);
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
				auto* Property = FindParameterMap(Instance, FName("TextureParameterOverrides"));
				FReflectedPropertyEditTarget Target;
				FPropertyValueSnapshot Proposed;
				std::string Error;
				if (!Property || !CaptureOverrideProposal(Instance, Property, MaterialParameterBaseColorTexture, bOverride,
					[&](FProperty* ValueProperty, void* Container) {
						static_cast<FObjectProperty*>(ValueProperty)->SetObjectPropertyValue(Container, Texture);
					}, Target, Proposed, &Error) || !SubmitPropertyEdit(Target, Proposed, false))
				{
					if (!Error.empty()) SetError(std::move(Error));
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
			.AssignSelection = [this, Material, Instance](DObject* Selection, std::string& OutError) {
				DTexture2D* Selected = Cast<DTexture2D>(Selection);
				if (Selection && !Selected)
				{
					OutError = "The selected asset is not a texture.";
					return false;
				}
				auto* Property = FindParameterMap(Material, FName(Instance ? "TextureParameterOverrides" : "TextureParameters"));
				if (!Property)
				{
					OutError = "The reflected texture parameter is unavailable.";
					return false;
				}
				FReflectedPropertyEditTarget Target;
				FPropertyValueSnapshot Proposed;
				if (!CaptureParameterValueProposal(Material, Property, MaterialParameterBaseColorTexture,
					[&](FProperty* ValueProperty, void* Container) {
						static_cast<FObjectProperty*>(ValueProperty)->SetObjectPropertyValue(Container, Selected);
					}, Target, Proposed, &OutError)) return false;
				return SubmitPropertyEdit(Target, Proposed, false);
			},
		});
		if (!bOverride) ImGui::EndDisabled();
		if (!PickerResult.Error.empty()) SetError(PickerResult.Error);
		MonaImGui::EndPropertyRow();
		ImGui::PopID();
	}

	auto MMaterialEditor::SubmitPropertyEdit(
		const FReflectedPropertyEditTarget& Target,
		const FPropertyValueSnapshot& ProposedValue,
		bool bContinuous
	) -> bool
	{
		if (PropertyEditSession.IsActive() && !PropertyEditSession.MatchesTarget(Target)) FinishActivePropertyEdit(false);
		std::string Error;
		if (!PropertyEditSession.IsActive())
		{
			FEditorTransactionManager* Transactions = GEditor ? &GEditor->GetTransactionManager() : nullptr;
			const std::string Description = std::format("Edit {}", Target.MemberProperty->NamePrivate.ToString());
			if (!PropertyEditSession.Begin(Target, Description, nullptr, &Error, Transactions))
			{
				SetError(std::move(Error));
				return false;
			}
			ActiveEditObject = Target.Object;
		}

		const EReflectedPropertyEditResult Result = PropertyEditSession.Apply(ProposedValue, &Error);
		if (Result == EReflectedPropertyEditResult::Failed)
		{
			SetError(std::move(Error));
			FinishActivePropertyEdit(true);
			return false;
		}
		if (!bContinuous) FinishActivePropertyEdit(false);
		return true;
	}

	auto MMaterialEditor::FinishActivePropertyEdit(bool bCancel) -> void
	{
		if (!PropertyEditSession.IsActive()) return;
		std::string Error;
		const EReflectedPropertyEditResult Result = bCancel ? PropertyEditSession.Cancel(&Error) : PropertyEditSession.Commit(&Error);
		if (Result == EReflectedPropertyEditResult::Failed) SetError(std::move(Error));
		if (!PropertyEditSession.IsActive()) ActiveEditObject = nullptr;
	}

	auto MMaterialEditor::SetError(std::string Message) -> void
	{
		ErrorMessage = std::move(Message);
		DURIN_ERROR("Material editor: {}", ErrorMessage);
	}
}
