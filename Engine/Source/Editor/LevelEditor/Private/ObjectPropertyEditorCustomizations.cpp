#include "ObjectPropertyEditorCustomizations.h"

#include "AssetSystem.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/DObjectGlobals.h"
#include "Editor/ReflectedPropertyView.h"
#include "Engine/Actor.h"
#include "LevelEditorContext.h"
#include "Materials/MaterialInterface.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"
#include "MonaImGuiPropertyTable.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	namespace
	{
		auto IsClassChildOf(const DClass* Class, const DClass* Parent) -> bool
		{
			for (const DClass* Current = Class; Current != nullptr; Current = Current->GetSuperClass())
			{
				if (Current == Parent) return true;
			}
			return false;
		}

		class FActorDetailsCustomization final : public IObjectDetailsCustomization
		{
		public:
			auto CustomizeDetails(FLevelEditorContext&, DObject* Object,
				FObjectPropertyViewBuilder& Builder) -> void override
			{
				auto* Actor = Cast<AActor>(Object);
				DSceneComponent* RootComponent = Actor ? Actor->GetRootComponent() : nullptr;
				FProperty* TransformProperty = RootComponent
					? RootComponent->GetClass()->FindPropertyByName("RelativeTransform") : nullptr;
				if (TransformProperty)
				{
					Builder.AddProperty(RootComponent, TransformProperty, 0, {.Label = "Transform"},
						"Location Rotation Scale");
				}
			}
		};

		class FStaticMeshDetailsCustomization final : public IObjectDetailsCustomization
		{
		public:
			auto CustomizeDetails(FLevelEditorContext& Context, DObject* Object,
				FObjectPropertyViewBuilder& Builder) -> void override
			{
				auto* Component = Cast<DStaticMeshComponent>(Object);
				if (!Component) return;
				FProperty* ReflectedMaterials = Component->GetClass()->FindPropertyByName("Materials");
				Builder.HideProperty(ReflectedMaterials);
				auto* MaterialsProperty = ReflectedMaterials
					&& ReflectedMaterials->GetKind() == DurinCodeGen::EPropertyGenFlags::Array
					? static_cast<FArrayProperty*>(ReflectedMaterials) : nullptr;
				auto* MaterialProperty = MaterialsProperty && MaterialsProperty->GetInner()
					&& MaterialsProperty->GetInner()->GetKind() == DurinCodeGen::EPropertyGenFlags::Object
					? static_cast<FObjectProperty*>(MaterialsProperty->GetInner()) : nullptr;
				DStaticMesh* Mesh = Component->GetStaticMesh();
				const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
				if (!RenderData || !MaterialsProperty || !MaterialsProperty->HasArrayHelper() || !MaterialProperty) return;

				for (uint32 SlotIndex = 0; SlotIndex < RenderData->MaterialSlots.size(); ++SlotIndex)
				{
					const std::string Label = std::format("Material[{}] {}", SlotIndex, RenderData->MaterialSlots[SlotIndex].Name);
					Builder.AddCustomRow(std::format("Materials Material Slots {}", Label),
						[this, &Context, Component, MaterialsProperty, MaterialProperty, SlotIndex, Label]
						(FReflectedPropertyView& PropertyView, const FReflectedPropertyViewContext& ViewContext) {
							return DrawMaterialSlot(Context, Component, MaterialsProperty, MaterialProperty,
								SlotIndex, Label, PropertyView, ViewContext);
						});
				}
			}

		private:
			auto DrawMaterialSlot(FLevelEditorContext& Context, DStaticMeshComponent* Component,
				FArrayProperty* MaterialsProperty, FObjectProperty* MaterialProperty, uint32 SlotIndex,
				const std::string& Label, FReflectedPropertyView& PropertyView,
				const FReflectedPropertyViewContext& ViewContext) -> bool
			{
				ImGui::PushID("StaticMeshMaterial");
				ImGui::PushID(static_cast<int>(SlotIndex));
				MonaImGui::BeginPropertyRow(Label.c_str(), ViewContext.bReadOnly);
				DMaterialInterface* Current = Component->GetMaterial(SlotIndex);
				DMaterialInterface* SelectedMaterial = Current;
				const std::string Preview = Current && Current->GetPackage()
					? Current->GetPackage()->GetPackagePath() : "None";
				if (ImGui::BeginCombo("##Value", Preview.c_str()))
				{
					ImGui::SetNextItemWidth(-FLT_MIN);
					ImGui::InputTextWithHint("##AssetSearch", "Search materials...", AssetSearchText.data(), AssetSearchText.size());
					if (ImGui::Selectable("Clear", Current == nullptr)) SelectedMaterial = nullptr;
					for (const auto& [Path, Data] : Asset::GetAssetRegistry().GetAssets())
					{
						DClass* AssetClass = FindClassByQualifiedName(Data.AssetClassName);
						const std::string PathString = Path.ToString();
						if (!AssetClass || !IsClassChildOf(AssetClass, DMaterialInterface::StaticClass())
							|| !StringUtils::ContainsInsensitive(PathString, AssetSearchText.data())) continue;
						if (ImGui::Selectable(PathString.c_str(), Current && Current->GetPackage()
							&& Current->GetPackage()->GetPackagePath() == PathString))
						{
							DObject* Loaded = nullptr;
							Asset::FAssetResult Result = Asset::LoadAsset(Path, Loaded);
							if (!Result) Context.SetError(Result.Message);
							else if (DMaterialInterface* Selected = Cast<DMaterialInterface>(Loaded)) SelectedMaterial = Selected;
						}
					}
					ImGui::EndCombo();
				}

				bool bChanged = false;
				if (!ViewContext.bReadOnly && SelectedMaterial != Current)
				{
					const FReflectedPropertyEditTarget MaterialsTarget =
						FReflectedPropertyEditTarget::ForMember(Component, MaterialsProperty);
					if (void* Element = MaterialsProperty->GetMutableElementPtr(Component, SlotIndex))
					{
						const FReflectedPropertyEditTarget SlotTarget =
							MaterialsTarget.ForArrayElement(MaterialProperty, Element, SlotIndex);
						bChanged = PropertyView.SubmitPropertyValueEdit(ViewContext, SlotTarget,
							[&](FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
								static_cast<FObjectProperty*>(ScratchProperty)->SetObjectPropertyValue(
									ScratchContainer, SelectedMaterial, ScratchArrayIndex);
						}, false);
					}
					else
					{
						bChanged = PropertyView.SubmitPropertyValueEdit(ViewContext, MaterialsTarget,
							[&](FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
								auto* ScratchMaterials = static_cast<FArrayProperty*>(ScratchProperty);
								ScratchMaterials->Resize(ScratchContainer, static_cast<uint64>(SlotIndex) + 1, ScratchArrayIndex);
								if (void* ScratchElement = ScratchMaterials->GetMutableElementPtr(ScratchContainer, SlotIndex, ScratchArrayIndex))
									MaterialProperty->SetObjectPropertyValue(ScratchElement, SelectedMaterial);
						}, false);
					}
				}
				MonaImGui::EndPropertyRow(ViewContext.bReadOnly);
				ImGui::PopID();
				ImGui::PopID();
				return bChanged;
			}

			std::array<char, 256> AssetSearchText{};
		};
	} // namespace

	auto CreateActorDetailsCustomization() -> std::shared_ptr<IObjectDetailsCustomization>
	{
		return std::make_shared<FActorDetailsCustomization>();
	}

	auto CreateStaticMeshDetailsCustomization() -> std::shared_ptr<IObjectDetailsCustomization>
	{
		return std::make_shared<FStaticMeshDetailsCustomization>();
	}
}
