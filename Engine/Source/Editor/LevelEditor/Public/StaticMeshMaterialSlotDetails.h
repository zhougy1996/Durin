#pragma once

#include "LevelEditorAPI.h"
#include "Editor/ReflectedPropertyView.h"

namespace Durin
{
	class DClass;
	class DMaterialInterface;
	class DStaticMeshComponent;
	class IObjectDetailsCustomization;

	// Identifies where the displayed material for a static-mesh slot originates.
	enum class EStaticMeshMaterialSource : uint8
	{
		ComponentOverride,
		MeshDefault,
		RendererFallback,
		Orphan,
	};

	// Presents one resolved material slot and its override state to details UI.
	struct FStaticMeshMaterialSlotDetailsEntry
	{
		FGuid SlotId;
		uint32 SlotIndex = std::numeric_limits<uint32>::max();
		std::string Label;
		std::string SearchKeywords;
		DMaterialInterface* Material = nullptr;
		EStaticMeshMaterialSource Source = EStaticMeshMaterialSource::RendererFallback;
		bool bHasOverride = false;
		bool bOrphan = false;
	};

	// Resolves material slots and submits undoable component override edits.
	class FStaticMeshMaterialSlotDetailsModel
	{
	public:
		LEVELEDITOR_API explicit FStaticMeshMaterialSlotDetailsModel(DStaticMeshComponent* InComponent);

		auto GetComponent() const -> DStaticMeshComponent* { return Component; }
		auto HasMesh() const -> bool { return bHasMesh; }
		auto GetCurrentEntries() const -> std::span<const FStaticMeshMaterialSlotDetailsEntry> { return CurrentEntries; }
		auto GetOrphanEntries() const -> std::span<const FStaticMeshMaterialSlotDetailsEntry> { return OrphanEntries; }
		LEVELEDITOR_API static auto IsSupportedMaterialClass(const DClass* CandidateClass) -> bool;
		LEVELEDITOR_API static auto GetSourceLabel(EStaticMeshMaterialSource Source) -> std::string_view;

		LEVELEDITOR_API auto AssignMaterial(FReflectedPropertyView& PropertyView, const FReflectedPropertyViewContext& Context,
			const FStaticMeshMaterialSlotDetailsEntry& Entry, DMaterialInterface* Material,
			bool bContinuous = false) const -> bool;
		LEVELEDITOR_API auto ResetOverride(FReflectedPropertyView& PropertyView, const FReflectedPropertyViewContext& Context,
			const FStaticMeshMaterialSlotDetailsEntry& Entry) const -> bool;
		LEVELEDITOR_API auto RemoveOrphan(FReflectedPropertyView& PropertyView, const FReflectedPropertyViewContext& Context,
			const FStaticMeshMaterialSlotDetailsEntry& Entry) const -> bool;

	private:
		LEVELEDITOR_API auto SubmitOverrideEdit(FReflectedPropertyView& PropertyView, const FReflectedPropertyViewContext& Context,
			const FGuid& SlotId, DMaterialInterface* Material, EPropertyChangeKind Kind, bool bContinuous = false) const -> bool;

		// Non-owning component whose current slot state is snapshotted by this model.
		DStaticMeshComponent* Component = nullptr;
		bool bHasMesh = false;
		std::vector<FStaticMeshMaterialSlotDetailsEntry> CurrentEntries;
		std::vector<FStaticMeshMaterialSlotDetailsEntry> OrphanEntries;
	};

	LEVELEDITOR_API auto CreateStaticMeshComponentDetailsCustomization() -> std::shared_ptr<IObjectDetailsCustomization>;
}
