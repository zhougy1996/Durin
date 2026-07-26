#include "Components/StaticMeshComponent.h"

#include "AssetSystem.h"
#include "DObject/DurinPropertyTypes.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "Materials/MaterialInterface.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	namespace
	{
		constexpr std::string_view LegacyMaterialFieldName = "Material";
		constexpr std::string_view LegacyMaterialsFieldName = "Materials";

		auto FindOverride(std::span<const FStaticMeshMaterialOverride> Overrides, const FGuid& SlotId)
			-> const FStaticMeshMaterialOverride*
		{
			const auto It = std::ranges::find(Overrides, SlotId, &FStaticMeshMaterialOverride::SlotId);
			return It == Overrides.end() ? nullptr : &*It;
		}

		auto FindLegacyField(
			std::span<const Asset::FAssetLegacyField> Fields,
			std::string_view Name,
			DurinCodeGen::EPropertyGenFlags Kind,
			std::string_view TypeSignature) -> const Asset::FAssetLegacyField*
		{
			const auto It = std::ranges::find_if(Fields, [=](const Asset::FAssetLegacyField& Field) {
				return Field.DeclaringClass == "Durin::DStaticMeshComponent"
					&& Field.Name == Name
					&& Field.Kind == Kind
					&& Field.TypeSignature == TypeSignature;
			});
			return It == Fields.end() ? nullptr : &*It;
		}
	}

	DStaticMeshComponent::DStaticMeshComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		static const bool RegisteredStructureUpgrader = [] {
			Asset::RegisterAssetStructureUpgrader(
				DStaticMeshComponent::StaticClass(),
				"Engine.StaticMeshComponent.LegacyMaterials",
				[](DObject* Object,
					std::span<const Asset::FAssetLegacyField> Fields,
					const Asset::FAssetMigrationContext& Context,
					std::vector<Asset::FAssetCompatibilityIssue>& OutIssues) -> Asset::FAssetResult
				{
					auto* Component = Cast<DStaticMeshComponent>(Object);
					if (Component == nullptr)
						return {Asset::EAssetError::TypeMismatch, "Static-mesh material upgrader received an incompatible object."};

					const Asset::FAssetLegacyField* MaterialField = FindLegacyField(
						Fields,
						LegacyMaterialFieldName,
						DurinCodeGen::EPropertyGenFlags::Object,
						"Object:Durin::DMaterialInterface:true");
					const Asset::FAssetLegacyField* MaterialsField = FindLegacyField(
						Fields,
						LegacyMaterialsFieldName,
						DurinCodeGen::EPropertyGenFlags::Array,
						"Array<Object:Durin::DMaterialInterface:true>");
					if (MaterialField == nullptr && MaterialsField == nullptr) return {};

					DObject* SlotZeroFallbackObject = nullptr;
					if (MaterialField != nullptr)
					{
						Asset::FAssetResult Result = Context.ReadObjectReference(*MaterialField, SlotZeroFallbackObject);
						if (!Result) return Result;
					}
					std::vector<DObject*> LegacyObjects;
					if (MaterialsField != nullptr)
					{
						Asset::FAssetResult Result = Context.ReadObjectReferenceArray(*MaterialsField, LegacyObjects);
						if (!Result) return Result;
					}

					auto ResolveMaterial = [](DObject* Candidate, size_t Index, DMaterialInterface*& OutMaterial)
						-> Asset::FAssetResult
					{
						OutMaterial = nullptr;
						if (Candidate == nullptr) return {};
						OutMaterial = Cast<DMaterialInterface>(Candidate);
						if (OutMaterial == nullptr)
							return {
								Asset::EAssetError::TypeMismatch,
								std::format("Legacy static-mesh material index {} references '{}', which is not a material.",
									Index, Candidate->GetObjectPath())};
						return {};
					};

					const size_t LegacyCount = std::max<size_t>(
						LegacyObjects.size(),
						SlotZeroFallbackObject != nullptr ? 1 : 0);
					uint64 MappedCount = 0;
					uint64 OrphanCount = 0;
					std::vector<std::string> Changes;
					std::unordered_set<FGuid> UsedIds;
					for (const FStaticMeshMaterialOverride& Override : Component->MaterialOverrides)
						UsedIds.insert(Override.SlotId);
					for (size_t Index = 0; Index < LegacyCount; ++Index)
					{
						DObject* Candidate = Index < LegacyObjects.size()
							? LegacyObjects[Index]
							: SlotZeroFallbackObject;
						DMaterialInterface* LegacyMaterial = nullptr;
						Asset::FAssetResult Result = ResolveMaterial(Candidate, Index, LegacyMaterial);
						if (!Result) return Result;
						if (LegacyMaterial == nullptr) continue;

						FGuid SlotId;
						const FStaticMeshMaterialSlotDefinition* Slot = Component->StaticMesh != nullptr
							? Component->StaticMesh->GetMaterialSlot(static_cast<uint32>(Index))
							: nullptr;
						if (Slot != nullptr)
						{
							SlotId = Slot->SlotId;
							++MappedCount;
							Changes.push_back(std::format(
								"Mapped legacy index {} to slot '{}' ({}): {}.",
								Index, Slot->Name.ToString(), SlotId.ToString(), LegacyMaterial->GetObjectPath()));
						}
						else
						{
							do SlotId = FGuid::NewGuid(); while (UsedIds.contains(SlotId));
							++OrphanCount;
							Changes.push_back(std::format(
								"Retained legacy index {} as orphan slot {}: {}.",
								Index, SlotId.ToString(), LegacyMaterial->GetObjectPath()));
						}
						if (UsedIds.insert(SlotId).second)
							Component->MaterialOverrides.push_back({.SlotId = SlotId, .Material = LegacyMaterial});
					}

					std::vector<Asset::FAssetLegacyField> HandledFields;
					if (MaterialField != nullptr) HandledFields.push_back(*MaterialField);
					if (MaterialsField != nullptr) HandledFields.push_back(*MaterialsField);
					std::string Summary;
					if (Changes.empty())
					{
						Summary = "Removed empty legacy Material and Materials fields; no material assignments were migrated.";
					}
					else
					{
						Summary = std::format(
							"Migrated {} material assignment(s) to current slots and retained {} orphan assignment(s). ",
							MappedCount, OrphanCount);
						for (const std::string& Change : Changes) Summary += Change + " ";
						Summary.pop_back();
					}
					OutIssues.push_back({
						.DeclaringClass = "Durin::DStaticMeshComponent",
						.LegacyFields = std::move(HandledFields),
						.Classification = Changes.empty()
							? Asset::EAssetCompatibilityClassification::SafeCleanup
							: Asset::EAssetCompatibilityClassification::Migrated,
						.MigrationSummary = std::move(Summary),
						.MigratedDataCount = MappedCount + OrphanCount,
						.Risk = Asset::EAssetCompatibilityRisk::None});
					return {};
				});
			return true;
		}();
		(void)RegisteredStructureUpgrader;
	}

	auto DStaticMeshComponent::SetStaticMesh(DStaticMesh* InStaticMesh) -> void
	{
		if (StaticMesh == InStaticMesh)
		{
			return;
		}

		StaticMesh = InStaticMesh;
		++MaterialComponentRevision;
		MarkPackageDirty();
		MarkRenderStateDirty();
	}

	auto DStaticMeshComponent::GetStaticMesh() const -> DStaticMesh*
	{
		return StaticMesh.Get();
	}

	auto DStaticMeshComponent::SetMaterial(DMaterialInterface* InMaterial) -> void
	{
		SetMaterial(0, InMaterial);
	}

	auto DStaticMeshComponent::SetMaterial(uint32 SlotIndex, DMaterialInterface* InMaterial) -> void
	{
		const FStaticMeshMaterialSlotDefinition* Slot = StaticMesh != nullptr ? StaticMesh->GetMaterialSlot(SlotIndex) : nullptr;
		if (Slot != nullptr) SetMaterialBySlotId(Slot->SlotId, InMaterial);
	}

	auto DStaticMeshComponent::GetMaterial() const -> DMaterialInterface*
	{
		return GetMaterial(0);
	}

	auto DStaticMeshComponent::GetMaterial(uint32 SlotIndex) const -> DMaterialInterface*
	{
		const FStaticMeshMaterialSlotDefinition* Slot = StaticMesh != nullptr ? StaticMesh->GetMaterialSlot(SlotIndex) : nullptr;
		return Slot != nullptr ? GetMaterialBySlotId(Slot->SlotId) : nullptr;
	}

	auto DStaticMeshComponent::SetMaterialBySlotId(const FGuid& SlotId, DMaterialInterface* InMaterial) -> bool
	{
		if (!SlotId.IsValid() || StaticMesh == nullptr || StaticMesh->FindMaterialSlot(SlotId) == nullptr) return false;
		if (InMaterial == nullptr) return ResetMaterialBySlotId(SlotId);
		if (FStaticMeshMaterialOverride* Override = const_cast<FStaticMeshMaterialOverride*>(FindOverride(MaterialOverrides, SlotId)))
		{
			if (Override->Material == InMaterial) return true;
			Override->Material = InMaterial;
		}
		else MaterialOverrides.push_back({.SlotId = SlotId, .Material = InMaterial});
		++MaterialComponentRevision;
		PendingMaterialDirtyFlags = EMaterialRenderDirtyFlags::ParameterValues | EMaterialRenderDirtyFlags::ParentChain;
		MarkPackageDirty();
		MarkRenderStateDirty();
		return true;
	}

	auto DStaticMeshComponent::ResetMaterialBySlotId(const FGuid& SlotId) -> bool
	{
		if (!SlotId.IsValid() || StaticMesh == nullptr || StaticMesh->FindMaterialSlot(SlotId) == nullptr) return false;
		return RemoveMaterialOverride(SlotId);
	}

	auto DStaticMeshComponent::RemoveMaterialOverride(const FGuid& SlotId) -> bool
	{
		const size_t PreviousSize = MaterialOverrides.size();
		std::erase_if(MaterialOverrides, [&SlotId](const FStaticMeshMaterialOverride& Override) { return Override.SlotId == SlotId; });
		if (MaterialOverrides.size() == PreviousSize) return false;
		++MaterialComponentRevision;
		PendingMaterialDirtyFlags = EMaterialRenderDirtyFlags::ParameterValues | EMaterialRenderDirtyFlags::ParentChain;
		MarkPackageDirty();
		if (StaticMesh != nullptr && StaticMesh->FindMaterialSlot(SlotId) != nullptr) MarkRenderStateDirty();
		return true;
	}

	auto DStaticMeshComponent::GetMaterialBySlotId(const FGuid& SlotId) const -> DMaterialInterface*
	{
		const FStaticMeshMaterialSlotDefinition* Slot = StaticMesh != nullptr ? StaticMesh->FindMaterialSlot(SlotId) : nullptr;
		if (Slot == nullptr) return nullptr;
		if (const FStaticMeshMaterialOverride* Override = FindOverride(MaterialOverrides, SlotId)) return Override->Material.Get();
		return Slot->DefaultMaterial.Get();
	}

	auto DStaticMeshComponent::GetMaterialOverride(const FGuid& SlotId) const -> DMaterialInterface*
	{
		const FStaticMeshMaterialOverride* Override = FindOverride(MaterialOverrides, SlotId);
		return Override != nullptr ? Override->Material.Get() : nullptr;
	}

	auto DStaticMeshComponent::HasMaterialOverride(const FGuid& SlotId) const -> bool
	{
		return FindOverride(MaterialOverrides, SlotId) != nullptr;
	}

	auto DStaticMeshComponent::IsMaterialOverrideOrphan(const FGuid& SlotId) const -> bool
	{
		return HasMaterialOverride(SlotId) && (StaticMesh == nullptr || StaticMesh->FindMaterialSlot(SlotId) == nullptr);
	}

	auto DStaticMeshComponent::GetNumMaterials() const -> uint32
	{
		return StaticMesh != nullptr ? StaticMesh->GetNumMaterialSlots() : 0;
	}

	auto DStaticMeshComponent::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError)) return false;
		return ValidateMaterialOverrides(MaterialOverrides, OutError);
	}

	auto DStaticMeshComponent::PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
		if (!Proposal.MemberProperty || !Proposal.DraftRootProperty || !Proposal.DraftRootContainer) return true;
		const FName Name = Proposal.MemberProperty->NamePrivate;
		if (Name == FName("StaticMesh"))
		{
			if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Object)
			{
				OutError = "The static-mesh object property metadata is unavailable.";
				return false;
			}
			DObject* Value = static_cast<const FObjectProperty*>(Proposal.DraftRootProperty)->GetObjectPropertyValue(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			if (Value && !Cast<DStaticMesh>(Value))
			{
				OutError = "Selected asset is not a static mesh.";
				return false;
			}
			return true;
		}
		if (Name != FName("MaterialOverrides")) return true;
		if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Array)
		{
			OutError = "The static-mesh material array metadata is unavailable.";
			return false;
		}
		auto* ArrayProperty = static_cast<const FArrayProperty*>(Proposal.DraftRootProperty);
		if (!ArrayProperty->GetInner() || ArrayProperty->GetInner()->GetKind() != DurinCodeGen::EPropertyGenFlags::Struct)
		{
			OutError = "The static-mesh material override metadata is unavailable.";
			return false;
		}
		std::vector<FStaticMeshMaterialOverride> Overrides;
		Overrides.reserve(static_cast<size_t>(ArrayProperty->Num(Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex)));
		for (uint64 Index = 0; Index < ArrayProperty->Num(Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex); ++Index)
		{
			const auto* Element = static_cast<const FStaticMeshMaterialOverride*>(
				ArrayProperty->GetElementPtr(Proposal.DraftRootContainer, Index, Proposal.DraftRootArrayIndex));
			if (Element != nullptr) Overrides.push_back(*Element);
		}
		return ValidateMaterialOverrides(Overrides, OutError);
	}

	auto DStaticMeshComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty || (Event.Phase == EPropertyChangePhase::Committed
			&& Event.Origin == EPropertyChangeOrigin::Edit)) return;
		const FName Name = Event.MemberProperty->NamePrivate;
		if (Name == FName("StaticMesh"))
		{
			++MaterialComponentRevision;
			MarkRenderStateDirty();
			return;
		}
		if (Name != FName("MaterialOverrides")) return;
		++MaterialComponentRevision;
		PendingMaterialDirtyFlags = EMaterialRenderDirtyFlags::ParameterValues | EMaterialRenderDirtyFlags::ParentChain;
		MarkRenderStateDirty();
	}

	auto DStaticMeshComponent::CreateSceneProxy() -> std::unique_ptr<PrimitiveSceneProxy>
	{
		if (StaticMesh == nullptr)
		{
			return nullptr;
		}

		FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
		if (RenderData == nullptr || RenderData->LODResources.empty() || RenderData->LODResources[0].Indices.empty())
		{
			return nullptr;
		}

		std::vector<FMaterialRenderUpdate> MaterialUpdates;
		MaterialUpdates.reserve(RenderData->MaterialSlots.size());
		for (uint32 SlotIndex = 0; SlotIndex < RenderData->MaterialSlots.size(); ++SlotIndex)
		{
			DMaterialInterface* SlotMaterial = GetMaterial(SlotIndex);
			FMaterialRenderUpdate& Update = MaterialUpdates.emplace_back();
			Update.SlotIndex = SlotIndex;
			Update.RenderData = SlotMaterial != nullptr ? SlotMaterial->GetRenderData() : FMaterialRenderData{};
			Update.MaterialVersion = SlotMaterial != nullptr ? SlotMaterial->GetRenderStateVersion() : 0;
			Update.ComponentRevision = MaterialComponentRevision;
			Update.DirtyFlags = EMaterialRenderDirtyFlags::ParameterValues | EMaterialRenderDirtyFlags::ParentChain;
		}
		PendingMaterialDirtyFlags = EMaterialRenderDirtyFlags::None;
		return std::make_unique<FStaticMeshSceneProxy>(RenderData, std::move(MaterialUpdates));
	}

	auto DStaticMeshComponent::HandleStaticMeshRenderDataChanged(DStaticMesh* ChangedMesh) -> void
	{
		if (ChangedMesh == nullptr || ChangedMesh != StaticMesh.Get()) return;
		++MaterialComponentRevision;
		MarkRenderStateDirty();
	}

	auto DStaticMeshComponent::BuildMaterialRenderUpdate(EMaterialRenderDirtyFlags DirtyFlags, FMaterialRenderUpdate& OutUpdate) -> bool
	{
		DMaterialInterface* CurrentMaterial = GetMaterial(PendingMaterialSlotIndex);
		OutUpdate.SlotIndex = PendingMaterialSlotIndex;
		OutUpdate.RenderData = CurrentMaterial != nullptr ? CurrentMaterial->GetRenderData() : FMaterialRenderData{};
		OutUpdate.MaterialVersion = CurrentMaterial != nullptr ? CurrentMaterial->GetRenderStateVersion() : 0;
		OutUpdate.ComponentRevision = MaterialComponentRevision;
		OutUpdate.DirtyFlags = PendingMaterialDirtyFlags != EMaterialRenderDirtyFlags::None ? PendingMaterialDirtyFlags : DirtyFlags;
		PendingMaterialDirtyFlags = EMaterialRenderDirtyFlags::None;
		return true;
	}

	auto DStaticMeshComponent::HandleMaterialRenderDataChanged(
		uint32 SlotIndex,
		EMaterialRenderDirtyFlags DirtyFlags
	) -> void
	{
		if (SlotIndex >= GetNumMaterials()) return;
		// Component revisions order updates even when different slots refer to assets with unrelated versions.
		++MaterialComponentRevision;
		PendingMaterialSlotIndex = SlotIndex;
		PendingMaterialDirtyFlags = DirtyFlags;
		MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::MaterialData);
	}

	auto DStaticMeshComponent::ValidateMaterialOverrides(
		std::span<const FStaticMeshMaterialOverride> Overrides,
		std::string& OutError) const -> bool
	{
		std::unordered_set<FGuid> SlotIds;
		for (const FStaticMeshMaterialOverride& Override : Overrides)
		{
			if (!Override.SlotId.IsValid())
			{
				OutError = "A static-mesh component contains a material override with an invalid slot GUID.";
				return false;
			}
			if (!SlotIds.insert(Override.SlotId).second)
			{
				OutError = std::format("A static-mesh component contains duplicate overrides for slot GUID {}.", Override.SlotId.ToString());
				return false;
			}
			if (Override.Material == nullptr)
			{
				OutError = std::format("A static-mesh component contains a null override for slot GUID {}.", Override.SlotId.ToString());
				return false;
			}
			DObject* MaterialObject = reinterpret_cast<DObject*>(Override.Material.Get());
			if (Cast<DMaterialInterface>(MaterialObject) == nullptr)
			{
				OutError = std::format("A static-mesh component contains an incompatible object for slot GUID {}.", Override.SlotId.ToString());
				return false;
			}
		}
		return true;
	}

}
