#include "StaticMeshMaterialSlotDetails.h"

#include "Components/StaticMeshComponent.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "Editor/EditorAssetPicker.h"
#include "LevelEditorCustomizations.h"
#include "Materials/MaterialInterface.h"
#include "MonaImGui.h"
#include "StaticMesh/StaticMesh.h"
#include "Workspace/LevelEditorContext.h"

namespace Durin
{
	namespace
	{
		auto FindOverridesProperty(DStaticMeshComponent* Component) -> FArrayProperty*
		{
			FProperty* Property = Component ? Component->GetClass()->FindPropertyByName("MaterialOverrides") : nullptr;
			return Property && Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Array
				? static_cast<FArrayProperty*>(Property) : nullptr;
		}

		auto RemoveScratchOverride(const FArrayProperty& Property, void* Container, uint32 ArrayIndex,
			const FGuid& SlotId) -> void
		{
			const uint64 Count = Property.Num(Container, ArrayIndex);
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				auto* Entry = static_cast<FStaticMeshMaterialOverride*>(Property.GetMutableElementPtr(Container, Index, ArrayIndex));
				if (!Entry || Entry->SlotId != SlotId) continue;
				for (uint64 MoveIndex = Index + 1; MoveIndex < Count; ++MoveIndex)
				{
					auto* Destination = static_cast<FStaticMeshMaterialOverride*>(Property.GetMutableElementPtr(Container, MoveIndex - 1, ArrayIndex));
					auto* Source = static_cast<FStaticMeshMaterialOverride*>(Property.GetMutableElementPtr(Container, MoveIndex, ArrayIndex));
					*Destination = std::move(*Source);
				}
				Property.Resize(Container, Count - 1, ArrayIndex);
				return;
			}
		}

		class FStaticMeshComponentDetailsCustomization final : public IObjectDetailsCustomization
		{
		public:
			auto CustomizeDetails(FLevelEditorContext&, DObject* Object, FObjectPropertyViewBuilder& Builder) -> void override
			{
				auto* Component = Cast<DStaticMeshComponent>(Object);
				if (!Component) return;
				if (FProperty* Property = Component->GetClass()->FindPropertyByName("MaterialOverrides")) Builder.HideProperty(Property);

				const FStaticMeshMaterialSlotDetailsModel Model(Component);
				std::string SearchKeywords = "Materials Material Slots";
				for (const FStaticMeshMaterialSlotDetailsEntry& Entry : Model.GetCurrentEntries())
				{
					SearchKeywords += ' ';
					SearchKeywords += Entry.SearchKeywords;
				}
				for (const FStaticMeshMaterialSlotDetailsEntry& Entry : Model.GetOrphanEntries())
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
				for (const FStaticMeshMaterialSlotDetailsEntry& Entry : Model.GetOrphanEntries())
					bChanged |= DrawOrphanRow(Component, Entry, PropertyView, Context);

				MonaImGui::PropertyEdit::EndFixedArray();
				ImGui::PopID();
				return bChanged;
			}

			static auto DrawCurrentRow(DStaticMeshComponent* Component, const FStaticMeshMaterialSlotDetailsEntry& Entry,
				FReflectedPropertyView& PropertyView, const FReflectedPropertyViewContext& Context) -> bool
			{
				FStaticMeshMaterialSlotDetailsModel Model(Component);
				bool bChanged = false;
				ImGui::PushID(Entry.SlotId.ToString().c_str());
				if (MonaImGui::PropertyEdit::BeginFixedArrayElement(Entry.Label.c_str()))
				{
					MonaImGui::PropertyEdit::BeginRow("Material", Context.bReadOnly);
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
					});
					MonaImGui::PropertyEdit::EndRow(Context.bReadOnly);
					if (!Result.Error.empty() && Context.ReportError) Context.ReportError(Result.Error);

					MonaImGui::PropertyEdit::BeginRow("Source", Context.bReadOnly);
					ImGui::TextDisabled("%s", FStaticMeshMaterialSlotDetailsModel::GetSourceLabel(Entry.Source).data());
					if (Entry.bHasOverride)
					{
						ImGui::SameLine();
						if (ImGui::SmallButton("Reset")) bChanged |= Model.ResetOverride(PropertyView, Context, Entry);
					}
					MonaImGui::PropertyEdit::EndRow(Context.bReadOnly);
					MonaImGui::PropertyEdit::EndFixedArrayElement();
				}
				ImGui::PopID();
				return bChanged;
			}

			static auto DrawOrphanRow(DStaticMeshComponent* Component, const FStaticMeshMaterialSlotDetailsEntry& Entry,
				FReflectedPropertyView& PropertyView, const FReflectedPropertyViewContext& Context) -> bool
			{
				FStaticMeshMaterialSlotDetailsModel Model(Component);
				ImGui::PushID(Entry.SlotId.ToString().c_str());
				bool bRemove = false;
				if (MonaImGui::PropertyEdit::BeginFixedArrayElement(Entry.Label.c_str()))
				{
					MonaImGui::PropertyEdit::BeginRow("Material", true);
					ImGui::TextDisabled("%s", EditorAssetPicker::GetAssetPathOrNone(Entry.Material).c_str());
					MonaImGui::PropertyEdit::EndRow(true);
					MonaImGui::PropertyEdit::BeginRow("Actions", Context.bReadOnly);
					bRemove = ImGui::SmallButton("Remove");
					MonaImGui::PropertyEdit::EndRow(Context.bReadOnly);
					MonaImGui::PropertyEdit::EndFixedArrayElement();
				}
				ImGui::PopID();
				return bRemove && Model.RemoveOrphan(PropertyView, Context, Entry);
			}

			inline static std::array<char, 256> AssetSearchText{};
		};
	}

	FStaticMeshMaterialSlotDetailsModel::FStaticMeshMaterialSlotDetailsModel(DStaticMeshComponent* InComponent)
		: Component(InComponent)
	{
		DStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
		bHasMesh = Mesh != nullptr;
		if (Mesh)
		{
			uint32 Index = 0;
			for (const FStaticMeshMaterialSlotDefinition& Slot : Mesh->GetMaterialSlots())
			{
				DMaterialInterface* Override = Component->GetMaterialOverride(Slot.SlotId);
				DMaterialInterface* Resolved = Component->GetMaterialBySlotId(Slot.SlotId);
				const EStaticMeshMaterialSource Source = Override ? EStaticMeshMaterialSource::ComponentOverride
					: Slot.DefaultMaterial ? EStaticMeshMaterialSource::MeshDefault : EStaticMeshMaterialSource::RendererFallback;
				const std::string Label = std::format("[{}] {}", Index, Slot.Name.ToString());
				CurrentEntries.push_back({Slot.SlotId, Index, Label,
					std::format("Materials Material Slot {} {} {}", Index, Slot.Name.ToString(), GetSourceLabel(Source)),
					Resolved, Source, Override != nullptr, false});
				++Index;
			}
		}
		if (!Component) return;
		for (const FStaticMeshMaterialOverride& Override : Component->GetMaterialOverrides())
		{
			if (!Component->IsMaterialOverrideOrphan(Override.SlotId)) continue;
			const std::string Guid = Override.SlotId.ToString();
			OrphanEntries.push_back({Override.SlotId, std::numeric_limits<uint32>::max(),
				std::format("Orphan {}", Guid), std::format("Materials Orphan Warning {} Remove", Guid),
				Override.Material.Get(), EStaticMeshMaterialSource::Orphan, true, true});
		}
		std::ranges::sort(OrphanEntries, {}, &FStaticMeshMaterialSlotDetailsEntry::SlotId);
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
		case EStaticMeshMaterialSource::RendererFallback: return "Renderer Fallback";
		case EStaticMeshMaterialSource::Orphan: return "Orphan Override";
		}
		return "Unknown";
	}

	auto FStaticMeshMaterialSlotDetailsModel::SubmitOverrideEdit(FReflectedPropertyView& PropertyView,
		const FReflectedPropertyViewContext& Context, const FGuid& SlotId, DMaterialInterface* Material,
		EPropertyChangeKind Kind, bool bContinuous) const -> bool
	{
		FArrayProperty* Property = FindOverridesProperty(Component);
		if (!Component || !Property || !SlotId.IsValid()) return false;
		FReflectedPropertyEditTarget Target = FReflectedPropertyEditTarget::ForMember(Component, Property);
		Target.LogicalIdentity.resize(sizeof(SlotId));
		std::memcpy(Target.LogicalIdentity.data(), &SlotId, sizeof(SlotId));
		Target.Kind = Kind;
		return PropertyView.SubmitPropertyValueEdit(Context, Target,
			[SlotId, Material](FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
				auto& Array = *static_cast<FArrayProperty*>(ScratchProperty);
				for (uint64 Index = 0; Index < Array.Num(ScratchContainer, ScratchArrayIndex); ++Index)
				{
					auto* Override = static_cast<FStaticMeshMaterialOverride*>(Array.GetMutableElementPtr(ScratchContainer, Index, ScratchArrayIndex));
					if (!Override || Override->SlotId != SlotId) continue;
					if (Material) Override->Material = Material;
					else RemoveScratchOverride(Array, ScratchContainer, ScratchArrayIndex, SlotId);
					return;
				}
				if (!Material) return;
				const uint64 Count = Array.Num(ScratchContainer, ScratchArrayIndex);
				Array.Resize(ScratchContainer, Count + 1, ScratchArrayIndex);
				*static_cast<FStaticMeshMaterialOverride*>(Array.GetMutableElementPtr(ScratchContainer, Count, ScratchArrayIndex))
					= {.SlotId = SlotId, .Material = Material};
			}, bContinuous);
	}

	auto FStaticMeshMaterialSlotDetailsModel::AssignMaterial(FReflectedPropertyView& PropertyView,
		const FReflectedPropertyViewContext& Context, const FStaticMeshMaterialSlotDetailsEntry& Entry,
		DMaterialInterface* Material, bool bContinuous) const -> bool
	{
		if (!Material || Entry.bOrphan || !Component || Component->IsMaterialOverrideOrphan(Entry.SlotId)) return false;
		return SubmitOverrideEdit(PropertyView, Context, Entry.SlotId, Material,
			Entry.bHasOverride ? EPropertyChangeKind::ValueSet : EPropertyChangeKind::ArrayAdd, bContinuous);
	}

	auto FStaticMeshMaterialSlotDetailsModel::ResetOverride(FReflectedPropertyView& PropertyView,
		const FReflectedPropertyViewContext& Context, const FStaticMeshMaterialSlotDetailsEntry& Entry) const -> bool
	{
		if (!Entry.bHasOverride || Entry.bOrphan) return false;
		return SubmitOverrideEdit(PropertyView, Context, Entry.SlotId, nullptr, EPropertyChangeKind::ArrayRemove);
	}

	auto FStaticMeshMaterialSlotDetailsModel::RemoveOrphan(FReflectedPropertyView& PropertyView,
		const FReflectedPropertyViewContext& Context, const FStaticMeshMaterialSlotDetailsEntry& Entry) const -> bool
	{
		if (!Entry.bOrphan) return false;
		return SubmitOverrideEdit(PropertyView, Context, Entry.SlotId, nullptr, EPropertyChangeKind::ArrayRemove);
	}

	auto CreateStaticMeshComponentDetailsCustomization() -> std::shared_ptr<IObjectDetailsCustomization>
	{
		return std::make_shared<FStaticMeshComponentDetailsCustomization>();
	}
}
