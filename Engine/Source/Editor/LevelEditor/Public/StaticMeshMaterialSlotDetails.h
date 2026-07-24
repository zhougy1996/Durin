#pragma once

#include "LevelEditorAPI.h"
#include "Editor/ReflectedPropertyView.h"

namespace Durin
{
	class DClass;
	class DMaterialInterface;
	class DStaticMeshComponent;
	class IObjectDetailsCustomization;

	enum class EStaticMeshMaterialSource : uint8
	{
		ComponentOverride,
		MeshDefault,
		RendererFallback,
		Orphan,
	};

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

	class LEVELEDITOR_API FStaticMeshMaterialSlotDetailsModel
	{
	public:
		explicit FStaticMeshMaterialSlotDetailsModel(DStaticMeshComponent* InComponent);

		auto GetComponent() const -> DStaticMeshComponent* { return Component; }
		auto HasMesh() const -> bool { return bHasMesh; }
		auto GetCurrentEntries() const -> std::span<const FStaticMeshMaterialSlotDetailsEntry> { return CurrentEntries; }
		auto GetOrphanEntries() const -> std::span<const FStaticMeshMaterialSlotDetailsEntry> { return OrphanEntries; }
		static auto IsSupportedMaterialClass(const DClass* CandidateClass) -> bool;
		static auto GetSourceLabel(EStaticMeshMaterialSource Source) -> std::string_view;

		auto AssignMaterial(FReflectedPropertyView& PropertyView, const FReflectedPropertyViewContext& Context,
			const FStaticMeshMaterialSlotDetailsEntry& Entry, DMaterialInterface* Material,
			bool bContinuous = false) const -> bool;
		auto ResetOverride(FReflectedPropertyView& PropertyView, const FReflectedPropertyViewContext& Context,
			const FStaticMeshMaterialSlotDetailsEntry& Entry) const -> bool;
		auto RemoveOrphan(FReflectedPropertyView& PropertyView, const FReflectedPropertyViewContext& Context,
			const FStaticMeshMaterialSlotDetailsEntry& Entry) const -> bool;

	private:
		auto SubmitOverrideEdit(FReflectedPropertyView& PropertyView, const FReflectedPropertyViewContext& Context,
			const FGuid& SlotId, DMaterialInterface* Material, EPropertyChangeKind Kind, bool bContinuous = false) const -> bool;

		DStaticMeshComponent* Component = nullptr;
		bool bHasMesh = false;
		std::vector<FStaticMeshMaterialSlotDetailsEntry> CurrentEntries;
		std::vector<FStaticMeshMaterialSlotDetailsEntry> OrphanEntries;
	};

	LEVELEDITOR_API auto CreateStaticMeshComponentDetailsCustomization() -> std::shared_ptr<IObjectDetailsCustomization>;
}
