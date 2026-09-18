#include "Components/StaticMeshComponent.h"

#include "Components/ComponentMaterialOverride.h"

#include "DObject/DurinPropertyTypes.h"
#include "Engine/Level.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include "Materials/MaterialInterface.h"
#include "Physics/BodySetup.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	DStaticMeshComponent::DStaticMeshComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{}

	auto DStaticMeshComponent::SetStaticMesh(DStaticMesh* InStaticMesh) -> void
	{
		if (StaticMesh == InStaticMesh)
		{
			return;
		}

		StaticMesh = InStaticMesh;
		if (StaticMesh) StaticMesh->RequestRenderDataAndResources();
		AdvanceMaterialBindingRevision();
		MarkPackageDirty();
		MarkRenderStateDirty();
		RecreatePhysicsState();
	}

	auto DStaticMeshComponent::GetStaticMesh() const -> DStaticMesh*
	{
		return StaticMesh.Get();
	}

	auto DStaticMeshComponent::RefreshReloadedAssetBindings() -> void
	{
		if (StaticMesh) StaticMesh->RequestRenderDataAndResources();
		AdvanceMaterialBindingRevision();
		MarkRenderStateDirty();
		RecreatePhysicsState();
	}

	auto DStaticMeshComponent::GetBodySetup() const -> DBodySetup*
	{
		return StaticMesh ? StaticMesh->EnsureQualifiedBoxBodySetup() : nullptr;
	}

	auto DStaticMeshComponent::BuildCollisionShape(
		FCollisionShape& OutShape, FTransform& OutWorldTransform) const -> bool
	{
		DBodySetup* Setup = GetBodySetup();
		FTransform LocalTransform;
		if (!Setup || !Setup->BuildShape(OutShape, LocalTransform)) return false;
		OutWorldTransform = FTransform::Combine(GetWorldTransform(), LocalTransform);
		return IsValidPhysicsTransform(OutWorldTransform);
	}

	auto DStaticMeshComponent::BuildCollisionGeometry(
		FCollisionGeometryRef& OutGeometry, FTransform& OutWorldTransform) const -> bool
	{
		DBodySetup* Setup = GetBodySetup();
		FTransform LocalTransform;
		if (!Setup || !Setup->BuildGeometry(OutGeometry, LocalTransform)) return false;
		OutWorldTransform = FTransform::Combine(GetWorldTransform(), LocalTransform);
		return IsValidPhysicsTransform(OutWorldTransform);
	}

	auto DStaticMeshComponent::GetCollisionStateRevision() const -> uint64
	{
		const DBodySetup* Setup = GetBodySetup();
		return Setup ? Setup->GetRevision() : 0;
	}

	auto DStaticMeshComponent::GetMaterialIndex(FName SlotName) const -> std::optional<uint32>
	{
		return StaticMesh ? StaticMesh->GetMaterialIndex(SlotName) : std::nullopt;
	}

	auto DStaticMeshComponent::GetDefaultMaterial(uint32 SlotIndex) const -> DMaterialInterface*
	{
		const auto* Slot = StaticMesh ? StaticMesh->GetMaterialSlot(SlotIndex) : nullptr;
		return Slot ? Slot->DefaultMaterial.Get() : nullptr;
	}

	auto DStaticMeshComponent::GetNumMaterials() const -> uint32
	{
		return StaticMesh != nullptr ? StaticMesh->GetNumMaterialSlots() : 0;
	}

	auto DStaticMeshComponent::PostLoad() -> void
	{
		if (WasDeprecatedPropertyLoaded(FName("OverrideMaterials_DEPRECATED")))
			MigrateMaterialOverrides(OverrideMaterials_DEPRECATED);
		Super::PostLoad();
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
		return true;
	}

	auto DStaticMeshComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty || (Event.Phase == EPropertyChangePhase::Committed
			&& Event.Origin == EPropertyChangeOrigin::Edit)) return;
		const FName Name = Event.MemberProperty->NamePrivate;
		if (Name == FName("StaticMesh"))
		{
			AdvanceMaterialBindingRevision();
			MarkRenderStateDirty();
			RecreatePhysicsState();
			return;
		}
	}

	auto DStaticMeshComponent::CreateSceneProxy() -> std::unique_ptr<FPrimitiveSceneProxy>
	{
		if (StaticMesh == nullptr)
		{
			return nullptr;
		}

		StaticMesh->RequestRenderDataAndResources();
		const FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
		if (RenderData == nullptr
			|| RenderData->LODResources.empty()
			|| RenderData->LODResources[0].GetNumIndices() == 0)
		{
			return nullptr;
		}

		std::vector<FMaterialRenderProxyRef> MaterialProxies;
		MaterialProxies.reserve(RenderData->MaterialSlots.size());
		for (uint32 SlotIndex = 0; SlotIndex < RenderData->MaterialSlots.size(); ++SlotIndex)
		{
			DMaterialInterface* SlotMaterial = GetMaterial(SlotIndex);
			MaterialProxies.push_back(ComponentMaterialOverride::ResolveRenderProxy(SlotMaterial));
		}
		return std::make_unique<FStaticMeshSceneProxy>(
			RenderData,
			std::move(MaterialProxies),
			GetMaterialBindingRevision());
	}

	auto DStaticMeshComponent::OnRegister() -> void
	{
		if (StaticMesh) StaticMesh->RequestRenderDataAndResources();
		Super::OnRegister();
	}

#if DURIN_WITH_EDITOR
	auto DStaticMeshComponent::GetEditorPickingLocalBounds(
		FBox& OutBounds, EEditorPickingPrimitiveFamily& OutFamily) const -> bool
	{
		const FStaticMeshRenderData* Data = StaticMesh ? StaticMesh->GetRenderData() : nullptr;
		if (!Data || Data->LODResources.empty()) return false;
		OutBounds = Data->LODResources[0].LocalBounds;
		OutFamily = EEditorPickingPrimitiveFamily::StaticMesh;
		return OutBounds.bIsValid && Math::IsFinite(OutBounds.Min) && Math::IsFinite(OutBounds.Max);
	}
#endif

	auto DStaticMeshComponent::HandleStaticMeshRenderDataChanged(DStaticMesh* ChangedMesh) -> void
	{
		if (ChangedMesh == nullptr || ChangedMesh != StaticMesh.Get()) return;
		AdvanceMaterialBindingRevision();
		MarkRenderStateDirty();
		RecreatePhysicsState();
	}

}
