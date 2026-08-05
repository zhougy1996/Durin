#include "StaticMeshMaterialSlotDetails.h"

#include "Components/StaticMeshComponent.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "Editor/EditorAssetPicker.h"
#include "Icons/FontAwesomeIcons.h"
#include "LevelEditorCustomizations.h"
#include "Materials/MaterialInterface.h"
#include "MonaImGui.h"
#include "StaticMesh/StaticMesh.h"
#include "Workspace/LevelEditorContext.h"

namespace Durin
{
	namespace
	{
		auto GetResetTooltip(EStaticMeshMaterialSource Source) -> const char*
		{
			switch (Source)
			{
			case EStaticMeshMaterialSource::ComponentOverride:
				return "Component override. Reset to the static mesh default material.";
			case EStaticMeshMaterialSource::MeshDefault:
				return "Inherited from the static mesh default material.";
			case EStaticMeshMaterialSource::EngineDefault:
				return "No component override or mesh default is assigned; the renderer fallback is used.";
			}
			return "The material source is unknown.";
		}

		auto FindOverridesProperty(DStaticMeshComponent* Component) -> FArrayProperty*
		{
			FProperty* Property = Component ? Component->GetClass()->FindPropertyByName("OverrideMaterials") : nullptr;
			return Property && Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Array
				? static_cast<FArrayProperty*>(Property) : nullptr;
		}

		auto TrimScratchOverrides(const FArrayProperty& Property, void* Container, uint32 ArrayIndex) -> void
		{
			const auto* ObjectProperty = static_cast<const FObjectProperty*>(Property.GetInner());
			uint64 Count = Property.Num(Container, ArrayIndex);
			while (Count > 0)
			{
				const void* Element = Property.GetElementPtr(Container, Count - 1, ArrayIndex);
				if (ObjectProperty->GetObjectPropertyValue(Element) != nullptr) break;
				--Count;
			}
			Property.Resize(Container, Count, ArrayIndex);
		}

		// Adds resolved material-slot editing to static-mesh component details.
		class FStaticMeshComponentDetailsCustomization final : public IObjectDetailsCustomization
		{
		public:
			auto CustomizeDetails(FLevelEditorContext&, DObject* Object, FObjectPropertyViewBuilder& Builder) -> void override
			{
				auto* Component = Cast<DStaticMeshComponent>(Object);
				if (!Component) return;
				if (FProperty* Property = Component->GetClass()->FindPropertyByName("OverrideMaterials")) Builder.HideProperty(Property);

				const FStaticMeshMaterialSlotDetailsModel Model(Component);
				std::string SearchKeywords = "Materials Material Slots";
				for (const FStaticMeshMaterialSlotDetailsEntry& Entry : Model.GetCurrentEntries())
				{
					SearchKeywords += ' ';
					SearchKeywords += Entry.SearchKeywords;
				}
				Builder.AddCustomRow(SearchKeywords,
					[Component](FReflectedPropertyView& PropertyView, const FReflectedPropertyViewContext& ViewContext) {
						return DrawMaterials(Component, PropertyView, ViewContext);
					});
			}

		private:
			static auto DrawMaterials(DStaticMeshComponent* Component, FReflectedPropertyView& PropertyView,
				const FReflectedPropertyViewContext& Context) -> bool
			{
				const FStaticMeshMaterialSlotDetailsModel Model(Component);
				ImGui::PushID("StaticMeshMaterials");
				if (!MonaImGui::PropertyEdit::BeginFixedArray(
					"##Materials", "Materials", static_cast<uint64>(Model.GetCurrentEntries().size())))
				{
					ImGui::PopID();
					return false;
				}

				bool bChanged = false;
				if (Model.GetCurrentEntries().empty())
				{
					MonaImGui::PropertyEdit::BeginRow("Material Slots", true);
					ImGui::TextDisabled(Model.HasMesh()
						? "The assigned static mesh has no available material slots."
						: "Assign a static mesh to edit its material slots.");
					MonaImGui::PropertyEdit::EndRow(true);
				}
				for (const FStaticMeshMaterialSlotDetailsEntry& Entry : Model.GetCurrentEntries())
					bChanged |= DrawCurrentRow(Component, Entry, PropertyView, Context);
				if (Model.HasStoredOverrides())
				{
					MonaImGui::PropertyEdit::BeginRow("Actions", Context.bReadOnly);
					if (!Context.bReadOnly && ImGui::SmallButton("Clear All Overrides"))
					{
						bChanged |= Model.ClearOverrides(PropertyView, Context);
					}
					MonaImGui::PropertyEdit::EndRow(Context.bReadOnly);
				}

				MonaImGui::PropertyEdit::EndFixedArray();
				ImGui::PopID();
				return bChanged;
			}

			static auto DrawCurrentRow(DStaticMeshComponent* Component, const FStaticMeshMaterialSlotDetailsEntry& Entry,
				FReflectedPropertyView& PropertyView, const FReflectedPropertyViewContext& Context) -> bool
			{
				FStaticMeshMaterialSlotDetailsModel Model(Component);
				bool bChanged = false;
				ImGui::PushID(static_cast<int>(Entry.SlotIndex));
				MonaImGui::PropertyEdit::BeginRow(Entry.Label.c_str(), Context.bReadOnly);
				const FEditorAssetPickerResult Result = EditorAssetPicker::Draw({
					.ComboId = "##Material", .SearchId = "##MaterialSearch", .SearchHint = "Search materials...",
					.RequiredClass = DMaterialInterface::StaticClass(), .ClassPolicy = EEditorAssetClassPolicy::Derived,
					.CurrentSelection = Entry.Material, .SearchText = AssetSearchText, .bAllowNone = false,
					.AssignSelection = [&](DObject* Selection, std::string& Error) {
						if (Context.bReadOnly) { Error = "Details are read-only."; return false; }
						auto* Material = Cast<DMaterialInterface>(Selection);
						if (!Material) { Error = "Static-mesh slots accept material assets only."; return false; }
						bChanged = Model.AssignMaterial(PropertyView, Context, Entry, Material);
						if (!bChanged) Error = "The material override could not be applied.";
						return bChanged;
					},
					.TrailingAction = FEditorAssetPickerAction{
						.Icon = Icons::Refresh,
						.ButtonId = "ResetOverride",
						.Tooltip = GetResetTooltip(Entry.Source),
						.bEnabled = Entry.bHasOverride,
						.Execute = [&](std::string& Error) {
							bChanged = Model.ResetOverride(PropertyView, Context, Entry);
							if (!bChanged) Error = "The material override could not be reset.";
							return bChanged;
						},
					},
				});
				MonaImGui::PropertyEdit::EndRow(Context.bReadOnly);
				if (!Result.Error.empty() && Context.ReportError) Context.ReportError(Result.Error);
				ImGui::PopID();
				return bChanged;
			}

			inline static std::array<char, 256> AssetSearchText{};
		};
	}

	FStaticMeshMaterialSlotDetailsModel::FStaticMeshMaterialSlotDetailsModel(DStaticMeshComponent* InComponent)
		: Component(InComponent)
	{
		DStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
		bHasMesh = Mesh != nullptr;
		bHasStoredOverrides = Component && !Component->GetOverrideMaterials().empty();
		if (Mesh)
		{
			uint32 Index = 0;
			for (const FStaticMeshMaterialSlotDefinition& Slot : Mesh->GetMaterialSlots())
			{
				DMaterialInterface* Override = Component->GetMaterialOverride(Index);
				DMaterialInterface* Resolved = Component->GetMaterial(Index);
				const EStaticMeshMaterialSource Source = Override ? EStaticMeshMaterialSource::ComponentOverride
					: Slot.DefaultMaterial ? EStaticMeshMaterialSource::MeshDefault : EStaticMeshMaterialSource::EngineDefault;
				const std::string Label = std::format("[{}] {}", Index, Slot.Name.ToString());
				CurrentEntries.push_back({Index, Label,
					std::format("Materials Material Slot {} {} {}", Index, Slot.Name.ToString(), GetSourceLabel(Source)),
					Resolved, Source, Override != nullptr});
				++Index;
			}
		}
	}

	auto FStaticMeshMaterialSlotDetailsModel::IsSupportedMaterialClass(const DClass* CandidateClass) -> bool
	{
		return EditorAssetPicker::MatchesClass(CandidateClass, DMaterialInterface::StaticClass(), EEditorAssetClassPolicy::Derived);
	}

	auto FStaticMeshMaterialSlotDetailsModel::GetSourceLabel(EStaticMeshMaterialSource Source) -> std::string_view
	{
		switch (Source)
		{
		case EStaticMeshMaterialSource::ComponentOverride: return "Component Override";
		case EStaticMeshMaterialSource::MeshDefault: return "Mesh Default";
		case EStaticMeshMaterialSource::EngineDefault: return "Engine Default (/Engine/Materials/DefaultMaterial)";
		}
		return "Unknown";
	}

	auto FStaticMeshMaterialSlotDetailsModel::SubmitOverrideEdit(FReflectedPropertyView& PropertyView,
		const FReflectedPropertyViewContext& Context, uint32 SlotIndex, DMaterialInterface* Material,
		EPropertyChangeKind Kind, bool bContinuous) const -> bool
	{
		FArrayProperty* Property = FindOverridesProperty(Component);
		if (!Component || !Property) return false;
		FReflectedPropertyEditTarget Target = FReflectedPropertyEditTarget::ForMember(Component, Property);
		Target.LogicalIdentity.resize(sizeof(SlotIndex));
		std::memcpy(Target.LogicalIdentity.data(), &SlotIndex, sizeof(SlotIndex));
		Target.Kind = Kind;
		return PropertyView.SubmitPropertyValueEdit(Context, Target,
			[SlotIndex, Material](FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
				auto& Array = *static_cast<FArrayProperty*>(ScratchProperty);
				auto& Object = *static_cast<FObjectProperty*>(Array.GetInner());
				if (Material && Array.Num(ScratchContainer, ScratchArrayIndex) <= SlotIndex)
				{
					Array.Resize(ScratchContainer, static_cast<uint64>(SlotIndex) + 1, ScratchArrayIndex);
				}
				if (SlotIndex >= Array.Num(ScratchContainer, ScratchArrayIndex)) return;
				Object.SetObjectPropertyValue(
					Array.GetMutableElementPtr(ScratchContainer, SlotIndex, ScratchArrayIndex), Material);
				if (!Material) TrimScratchOverrides(Array, ScratchContainer, ScratchArrayIndex);
			}, bContinuous);
	}

	auto FStaticMeshMaterialSlotDetailsModel::AssignMaterial(FReflectedPropertyView& PropertyView,
		const FReflectedPropertyViewContext& Context, const FStaticMeshMaterialSlotDetailsEntry& Entry,
		DMaterialInterface* Material, bool bContinuous) const -> bool
	{
		if (!Material || !Component || Entry.SlotIndex >= Component->GetNumMaterials()) return false;
		return SubmitOverrideEdit(PropertyView, Context, Entry.SlotIndex, Material,
			Entry.bHasOverride ? EPropertyChangeKind::ValueSet : EPropertyChangeKind::ArrayAdd, bContinuous);
	}

	auto FStaticMeshMaterialSlotDetailsModel::ResetOverride(FReflectedPropertyView& PropertyView,
		const FReflectedPropertyViewContext& Context, const FStaticMeshMaterialSlotDetailsEntry& Entry) const -> bool
	{
		if (!Entry.bHasOverride) return false;
		return SubmitOverrideEdit(PropertyView, Context, Entry.SlotIndex, nullptr, EPropertyChangeKind::ArrayRemove);
	}

	auto FStaticMeshMaterialSlotDetailsModel::ClearOverrides(FReflectedPropertyView& PropertyView,
		const FReflectedPropertyViewContext& Context) const -> bool
	{
		FArrayProperty* Property = FindOverridesProperty(Component);
		if (!Component || !Property || !bHasStoredOverrides) return false;
		FReflectedPropertyEditTarget Target = FReflectedPropertyEditTarget::ForMember(Component, Property);
		Target.Kind = EPropertyChangeKind::ArrayRemove;
		return PropertyView.SubmitPropertyValueEdit(Context, Target,
			[](FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
				static_cast<FArrayProperty*>(ScratchProperty)->Resize(ScratchContainer, 0, ScratchArrayIndex);
			}, false);
	}

	auto CreateStaticMeshComponentDetailsCustomization() -> std::shared_ptr<IObjectDetailsCustomization>
	{
		return std::make_shared<FStaticMeshComponentDetailsCustomization>();
	}
}
