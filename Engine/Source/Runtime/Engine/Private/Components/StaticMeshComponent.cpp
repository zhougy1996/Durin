#include "Components/PropertyEditValidation.h"
#include "Components/StaticMeshComponent.h"
#include "Rendering/SceneInterface.h"

#include "Components/ComponentMaterialOverride.h"

#include "DObject/DurinPropertyTypes.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include "Materials/MaterialInterface.h"
#include "Physics/BodySetup.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	auto DStaticMeshComponent::SetRenderInstances(std::span<const FStaticMeshRenderInstance> Instances) -> bool
	{
		if (!ValidateStaticMeshRenderInstances(Instances)) return false;
		if (RenderInstances && std::ranges::equal(*RenderInstances, Instances)) return true;
		RenderInstances = std::make_shared<const std::vector<FStaticMeshRenderInstance>>(Instances.begin(), Instances.end());
		if (IsRegistered() && GetRenderScene())
			GetRenderScene()->UpdateStaticMeshRenderInstances(GetPrimitiveComponentId(), RenderInstances);
		return true;
	}

	auto DStaticMeshComponent::SetInstanceCustomData(uint32 Id, uint32 First, std::span<const float> Values) -> bool
	{
		if (!RenderInstances) return false;
		auto Candidate = *RenderInstances;
		const auto Found = std::ranges::find(Candidate, Id, &FStaticMeshRenderInstance::Id);
		if (Found == Candidate.end() || !Found->CustomData.Set(First, Values)) return false;
		return SetRenderInstances(Candidate);
	}

	auto DStaticMeshComponent::RemoveRenderInstance(uint32 Id) -> bool
	{
		if (!RenderInstances) return false;
		auto Candidate = *RenderInstances;
		if (std::erase_if(Candidate, [Id](const auto& Instance) { return Instance.Id == Id; }) == 0) return false;
		return SetRenderInstances(Candidate);
	}

	auto DStaticMeshComponent::ClearRenderInstances() -> void
	{
		if (!RenderInstances) return;
		RenderInstances.reset();
		if (IsRegistered() && GetRenderScene())
			GetRenderScene()->UpdateStaticMeshRenderInstances(GetPrimitiveComponentId(), {});
	}

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

		MarkRenderStateDirty();
		RecreatePhysicsState();
	}

	auto DStaticMeshComponent::GetBodySetup() const -> DBodySetup*
	{
		return StaticMesh ? StaticMesh->EnsureQualifiedBoxBodySetup() : nullptr;
	}

	auto DStaticMeshComponent::PopulateCollisionGeometry(FPhysicsBodyDesc& Desc) const -> bool
	{
		auto* Setup = GetBodySetup();
		return Setup && Setup->PopulateCollisionGeometry(Desc, GetWorldTransform());
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

	auto DStaticMeshComponent::PreEditChangeProperty(FPropertyEditProposal& Proposal) -> std::expected<void, FObjectValidationError>
	{
		if (auto Result = Super::PreEditChangeProperty(Proposal); !Result) return Result;
		if (!Proposal.MemberProperty || !Proposal.DraftRootProperty || !Proposal.DraftRootContainer) return {};
		const FName Name = Proposal.MemberProperty->NamePrivate;
		if (Name == FName("StaticMesh"))
		{
			if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Object)
			{
				return RejectPropertyEdit(*this, Proposal, EPropertyEditRejection::InvalidMetadata);
			}
			DObject* Value = static_cast<const FObjectProperty*>(Proposal.DraftRootProperty)->GetObjectPropertyValue(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			if (Value && !Cast<DStaticMesh>(Value))
			{
				return RejectPropertyEdit(*this, Proposal, EPropertyEditRejection::IncompatibleObject);
			}
			return {};
		}
		return {};
	}

	auto DStaticMeshComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty || (Event.Phase == EPropertyChangePhase::Committed
			&& Event.Origin == EPropertyChangeOrigin::Edit)) return;
		const FName Name = Event.MemberProperty->NamePrivate;
		if (Name == FName("StaticMesh"))
		{
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
			MaterialProxies.push_back(ComponentMaterialOverride::ResolveRenderProxy(SlotMaterial, GetRenderScene() ? GetRenderScene()->GetFeatureLevel() : FSceneInterface::GetDefaultFeatureLevel()));
		}
		return std::make_unique<FStaticMeshSceneProxy>(
			RenderData,
			std::move(MaterialProxies), RenderInstances);
	}
	auto DStaticMeshComponent::CollectPSOPrecacheData(const FPSOPrecacheParams& Params, FComponentPSOPrecacheParamsList& OutParams) -> void
	{
		if (StaticMesh) CollectMeshPSOPrecacheData(Params, OutParams, "LocalVertexFactory");
	}

	auto DStaticMeshComponent::OnRegister() -> void
	{
		if (StaticMesh) StaticMesh->RequestRenderDataAndResources();
		Super::OnRegister();
	}


	auto DStaticMeshComponent::HandleStaticMeshCollisionDataChanged(DStaticMesh* ChangedMesh) -> void
	{
		if (ChangedMesh == StaticMesh.Get()) RecreatePhysicsState();
	}

	auto DStaticMeshComponent::HandleStaticMeshRenderDataChanged(DStaticMesh* ChangedMesh) -> void
	{
		if (ChangedMesh == nullptr || ChangedMesh != StaticMesh.Get()) return;

		MarkRenderStateDirty();
	}

}
