#include "Components/SplineMeshComponent.h"

#include "DObject/DurinPropertyTypes.h"
#include "Engine/FPrimitiveSceneProxy.h"
#include "Engine/Level.h"
#include "IScene.h"
#include "Materials/MaterialInterface.h"
#include "Materials/DefaultMaterialService.h"
#include "Spline/SplineMeshDeformer.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshMaterialBinding.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	namespace
	{
		auto SourceForwardRange(const FBox& Bounds, ESplineMeshAxis Axis) -> std::pair<double, double>
		{
			switch (Axis)
			{
			case ESplineMeshAxis::Y: return {Bounds.Min.y, Bounds.Max.y};
			case ESplineMeshAxis::Z: return {Bounds.Min.z, Bounds.Max.z};
			default: return {Bounds.Min.x, Bounds.Max.x};
			}
		}

		auto MakeCollisionInputIdentity(uint64 SourceRevision, uint64 DeformationRevision) -> uint64
		{
			uint64 Value = SourceRevision + 0x9e3779b97f4a7c15ull;
			Value ^= DeformationRevision + 0x9e3779b97f4a7c15ull + (Value << 6) + (Value >> 2);
			return Value != 0 ? Value : 1;
		}
	}

	DSplineMeshComponent::DSplineMeshComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		RebuildDerivedState();
	}

	auto DSplineMeshComponent::GetDerivedState() const -> std::shared_ptr<const FSplineMeshDerivedState>
	{
		return std::atomic_load_explicit(&DerivedState, std::memory_order_acquire);
	}

	auto DSplineMeshComponent::SetStaticMesh(DStaticMesh* InStaticMesh) -> void
	{
		if (StaticMesh == InStaticMesh) return;
		StaticMesh = InStaticMesh;
		++MaterialComponentRevision;
		RebuildDerivedState();
		MarkPackageDirty();
		MarkRenderStateDirty();
		RecreatePhysicsState();
	}

	auto DSplineMeshComponent::SetSplineMeshParams(const FSplineMeshParams& InParams, std::string* OutError) -> bool
	{
		FSplineMeshParams Candidate = InParams;
		if (const std::optional<FBox> SourceBounds = StaticMesh ? StaticMesh->GetLOD0LocalBounds() : std::nullopt)
		{
			const auto [Minimum, Maximum] = SourceForwardRange(*SourceBounds, Candidate.ForwardAxis);
			Candidate.SourceForwardMin = Minimum;
			Candidate.SourceForwardMax = Maximum;
		}
		FSplineMeshParams Normalized;
		if (!FSplineMeshDeformer::Normalize(Candidate, Normalized, OutError)) return false;
		if (SplineMeshParams == Normalized)
		{
			if (OutError) OutError->clear();
			return true;
		}
		const FSplineMeshParams Previous = SplineMeshParams;
		SplineMeshParams = Normalized;
		if (!RebuildDerivedState(OutError))
		{
			SplineMeshParams = Previous;
			return false;
		}
		MarkPackageDirty();
#if DURIN_WITH_EDITOR
		if (IsRegistered()) NotifyEditorPickingMutation();
#endif
		PushDynamicDataToScene();
		RecreatePhysicsState();
		return true;
	}

	auto DSplineMeshComponent::SetSplineMeshCollisionMode(ESplineMeshCollisionMode InMode) -> void
	{
		if (CollisionMode == InMode) return;
		CollisionMode = InMode;
		RebuildCollisionGeometryForPublishedState();
		MarkPackageDirty();
		RecreatePhysicsState();
	}

	auto DSplineMeshComponent::RebuildCollisionGeometryForPublishedState() -> void
	{
		const auto Published = GetDerivedState();
		if (!Published || !Published->IsValid()) return;
		auto Candidate = std::make_shared<FSplineMeshDerivedState>(*Published);
		Candidate->CollisionGeometry = {};
		if (CollisionMode == ESplineMeshCollisionMode::DeformedTriangleMesh)
		{
			std::vector<FVector3> CollisionPositions;
			CollisionPositions.reserve(Candidate->DeformedLOD0Positions.size());
			for (const FVector3f& Position : Candidate->DeformedLOD0Positions)
				CollisionPositions.emplace_back(Position);
			Candidate->CollisionGeometry = FCollisionGeometryRef::BuildTriangleMesh(
				CollisionPositions, Candidate->LOD0Indices);
		}
		std::atomic_store_explicit(&DerivedState,
			std::shared_ptr<const FSplineMeshDerivedState>(Candidate), std::memory_order_release);
	}

	auto DSplineMeshComponent::RebuildDerivedState(std::string* OutError) -> bool
	{
		auto Candidate = std::make_shared<FSplineMeshDerivedState>();
		Candidate->Params = SplineMeshParams;
		Candidate->DeformationRevision = DeformationRevision;
		if (!StaticMesh)
		{
			Candidate->Status = ESplineMeshDerivedStateStatus::NoStaticMesh;
			Candidate->Diagnostic = "No StaticMesh is assigned.";
			std::atomic_store_explicit(&DerivedState, std::shared_ptr<const FSplineMeshDerivedState>(Candidate), std::memory_order_release);
			if (OutError) OutError->clear();
			return true;
		}

		const FStaticMeshRenderResourceStatus ResourceStatus = StaticMesh->GetRenderResourceStatus();
		Candidate->SourceRenderResourceRevision = ResourceStatus.Revision;
		const FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
		if (!RenderData || RenderData->LODResources.empty())
		{
			Candidate->Status = ESplineMeshDerivedStateStatus::SourceDataUnavailable;
			Candidate->Diagnostic = "StaticMesh CPU render data is unavailable.";
			std::atomic_store_explicit(&DerivedState, std::shared_ptr<const FSplineMeshDerivedState>(Candidate), std::memory_order_release);
			if (OutError) OutError->clear();
			return true;
		}

		const FStaticMeshLODResources& SourceLOD = RenderData->LODResources[0];
		const auto& Positions = SourceLOD.VertexBuffers.PositionVertexBuffer.GetPositions();
		const auto& Indices = SourceLOD.IndexBuffer.GetIndices();
		if (Positions.empty() || Indices.empty() || !SourceLOD.LocalBounds.bIsValid)
		{
			Candidate->Status = ESplineMeshDerivedStateStatus::InvalidSourceData;
			Candidate->Diagnostic = "StaticMesh LOD 0 has no finite indexed geometry.";
			if (OutError) *OutError = Candidate->Diagnostic;
			return false;
		}

		FSplineMeshParams Params = SplineMeshParams;
		const auto [Minimum, Maximum] = SourceForwardRange(SourceLOD.LocalBounds, Params.ForwardAxis);
		Params.SourceForwardMin = Minimum;
		Params.SourceForwardMax = Maximum;
		if (!FSplineMeshDeformer::Normalize(Params, Params, OutError)) return false;

		Candidate->Params = Params;
		Candidate->ConservativeLocalBounds = FSplineMeshDeformer::ComputeConservativeBounds(Params, RenderData->LocalBounds);
		Candidate->DeformedLOD0Positions.reserve(Positions.size());
		for (const FVector3f& Position : Positions)
		{
			const FVector3 Deformed = FSplineMeshDeformer::DeformPosition(Params, FVector3(Position));
			if (!Math::IsFinite(Deformed))
			{
				if (OutError) *OutError = "SplineMesh deformation produced a non-finite position.";
				return false;
			}
			Candidate->DeformedLOD0Positions.emplace_back(Deformed);
		}
		Candidate->LOD0Indices = Indices;
		for (uint32 Index : Indices)
		{
			if (Index >= Candidate->DeformedLOD0Positions.size())
			{
				if (OutError) *OutError = "StaticMesh LOD 0 contains an out-of-range index.";
				return false;
			}
		}
		FStaticMeshLODResources QueryLOD;
		QueryLOD.VertexBuffers.PositionVertexBuffer.Init(Candidate->DeformedLOD0Positions);
		QueryLOD.IndexBuffer.Init(Candidate->LOD0Indices);
		QueryLOD.LocalBounds = Candidate->ConservativeLocalBounds;
		Candidate->EditorAcceleration = BuildStaticMeshRayQueryAcceleration(QueryLOD);
		Candidate->DeformationRevision = DeformationRevision + 1;
		Candidate->CollisionInputIdentity = MakeCollisionInputIdentity(
			Candidate->SourceRenderResourceRevision, Candidate->DeformationRevision);
		if (CollisionMode == ESplineMeshCollisionMode::DeformedTriangleMesh)
		{
			std::vector<FVector3> CollisionPositions;
			CollisionPositions.reserve(Candidate->DeformedLOD0Positions.size());
			for (const FVector3f& Position : Candidate->DeformedLOD0Positions)
				CollisionPositions.emplace_back(Position);
			Candidate->CollisionGeometry = FCollisionGeometryRef::BuildTriangleMesh(
				CollisionPositions, Candidate->LOD0Indices);
		}
		Candidate->Status = ESplineMeshDerivedStateStatus::Valid;
		Candidate->Diagnostic.clear();
		DeformationRevision = Candidate->DeformationRevision;
		SplineMeshParams = Params;
		std::atomic_store_explicit(&DerivedState, std::shared_ptr<const FSplineMeshDerivedState>(Candidate), std::memory_order_release);
		if (OutError) OutError->clear();
		return true;
	}

	auto DSplineMeshComponent::BuildCollisionGeometry(
		FCollisionGeometryRef& OutGeometry, FTransform& OutWorldTransform) const -> bool
	{
		if (CollisionMode != ESplineMeshCollisionMode::DeformedTriangleMesh) return false;
		const auto State = GetDerivedState();
		if (!State || !State->IsValid() || !State->CollisionGeometry.IsValid()) return false;
		OutGeometry = State->CollisionGeometry;
		OutWorldTransform = GetWorldTransform();
		return IsValidPhysicsTransform(OutWorldTransform);
	}

	auto DSplineMeshComponent::GetCollisionStateRevision() const -> uint64
	{
		const auto State = GetDerivedState();
		return CollisionMode == ESplineMeshCollisionMode::DeformedTriangleMesh && State
			? State->CollisionInputIdentity : 0;
	}

	auto DSplineMeshComponent::SetMaterial(DMaterialInterface* InMaterial) -> bool
	{
		return SetMaterial(0, InMaterial);
	}

	auto DSplineMeshComponent::SetMaterial(uint32 SlotIndex, DMaterialInterface* InMaterial) -> bool
	{
		if (!StaticMesh || !StaticMesh->GetMaterialSlot(SlotIndex)) return false;
		if (!InMaterial)
		{
			if (!GetMaterialOverride(SlotIndex)) return true;
			return ResetMaterial(SlotIndex);
		}
		if (SlotIndex >= OverrideMaterials.size()) OverrideMaterials.resize(static_cast<size_t>(SlotIndex) + 1);
		if (OverrideMaterials[SlotIndex] == InMaterial) return true;
		OverrideMaterials[SlotIndex] = InMaterial;
		++MaterialComponentRevision;
		PendingMaterialSlotIndex = SlotIndex;
		MarkPackageDirty();
		MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::MaterialBinding);
		return true;
	}

	auto DSplineMeshComponent::GetMaterial(uint32 SlotIndex) const -> DMaterialInterface*
	{
		const FStaticMeshMaterialSlotDefinition* Slot = StaticMesh ? StaticMesh->GetMaterialSlot(SlotIndex) : nullptr;
		if (!Slot) return nullptr;
		if (DMaterialInterface* Override = GetMaterialOverride(SlotIndex)) return Override;
		return Slot->DefaultMaterial.Get();
	}

	auto DSplineMeshComponent::ResetMaterial(uint32 SlotIndex) -> bool
	{
		if (!StaticMesh || !StaticMesh->GetMaterialSlot(SlotIndex) || !GetMaterialOverride(SlotIndex)) return false;
		OverrideMaterials[SlotIndex] = nullptr;
		TrimTrailingNullOverrides();
		++MaterialComponentRevision;
		PendingMaterialSlotIndex = SlotIndex;
		MarkPackageDirty();
		MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::MaterialBinding);
		return true;
	}

	auto DSplineMeshComponent::ClearMaterialOverrides() -> bool
	{
		if (OverrideMaterials.empty()) return false;
		OverrideMaterials.clear();
		++MaterialComponentRevision;
		PendingMaterialSlotIndex = 0;
		MarkPackageDirty();
		MarkRenderStateDirty();
		return true;
	}

	auto DSplineMeshComponent::GetMaterialOverride(uint32 SlotIndex) const -> DMaterialInterface*
	{
		return SlotIndex < OverrideMaterials.size() ? OverrideMaterials[SlotIndex].Get() : nullptr;
	}

	auto DSplineMeshComponent::GetNumMaterials() const -> uint32
	{
		return StaticMesh ? StaticMesh->GetNumMaterialSlots() : 0;
	}

	auto DSplineMeshComponent::CreateSceneProxy() -> std::unique_ptr<FPrimitiveSceneProxy>
	{
		const auto State = GetDerivedState();
		if (!StaticMesh || !State || !State->IsValid()) return nullptr;
		StaticMesh->InitResources();
		const FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
		if (!StaticMesh->GetRenderResourceStatus().IsReady() || !RenderData
			|| RenderData->LODResources.empty() || RenderData->LODResources[0].GetNumIndices() == 0)
			return nullptr;
		std::vector<FMaterialRenderProxyRef> MaterialProxies;
		MaterialProxies.reserve(RenderData->MaterialSlots.size());
		for (uint32 SlotIndex = 0; SlotIndex < RenderData->MaterialSlots.size(); ++SlotIndex)
		{
			DMaterialInterface* SlotMaterial = GetMaterial(SlotIndex);
			if (!SlotMaterial) RecordMaterialFallbackReason(EMaterialFallbackReason::UnassignedDefault);
			MaterialProxies.push_back(SlotMaterial
				? SlotMaterial->GetMaterialRenderProxy() : GetDefaultMaterialRenderProxy());
		}
		return std::make_unique<FSplineMeshSceneProxy>(RenderData, std::move(MaterialProxies),
			MaterialComponentRevision, FSplineMeshRenderDynamicData{
				.Params = State->Params,
				.LocalBounds = State->ConservativeLocalBounds,
				.Revision = State->DeformationRevision});
	}

	auto DSplineMeshComponent::PushDynamicDataToScene() -> void
	{
		if (!IsRegistered()) return;
		IScene* Scene = GetRenderScene();
		const auto State = GetDerivedState();
		if (!Scene || !State || !State->IsValid()) return;
		Scene->UpdateSplineMeshDynamicData(GetPrimitiveSceneId(), FSplineMeshRenderDynamicData{
			.Params = State->Params,
			.LocalBounds = State->ConservativeLocalBounds,
			.Revision = State->DeformationRevision});
	}

	auto DSplineMeshComponent::TrimTrailingNullOverrides() -> void
	{
		TrimTrailingNullStaticMeshMaterialOverrides(OverrideMaterials);
	}

	auto DSplineMeshComponent::ValidateOverrideMaterials(
		std::span<const TObjectPtr<DMaterialInterface>> Overrides, std::string& OutError) const -> bool
	{
		return ValidateStaticMeshMaterialOverrides(Overrides, "SplineMesh component", OutError);
	}

	auto DSplineMeshComponent::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError) || !ValidateOverrideMaterials(OverrideMaterials, OutError)) return false;
		TrimTrailingNullOverrides();
		return RebuildDerivedState(&OutError);
	}

	auto DSplineMeshComponent::PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
		if (!Proposal.MemberProperty || !Proposal.DraftRootProperty || !Proposal.DraftRootContainer) return true;
		if (Proposal.MemberProperty->NamePrivate == FName("SplineMeshParams")
			&& Proposal.DraftRootProperty == Proposal.MemberProperty)
		{
			auto* Params = Proposal.DraftRootProperty->ContainerPtrToValuePtr<FSplineMeshParams>(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			FSplineMeshParams Candidate = *Params;
			if (const std::optional<FBox> Bounds = StaticMesh ? StaticMesh->GetLOD0LocalBounds() : std::nullopt)
			{
				const auto [Minimum, Maximum] = SourceForwardRange(*Bounds, Candidate.ForwardAxis);
				Candidate.SourceForwardMin = Minimum;
				Candidate.SourceForwardMax = Maximum;
			}
			FSplineMeshParams Normalized;
			if (!FSplineMeshDeformer::Normalize(Candidate, Normalized, &OutError)) return false;
			*Params = Normalized;
			return true;
		}
		if (Proposal.MemberProperty->NamePrivate == FName("StaticMesh"))
		{
			if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Object) return false;
			DObject* Value = static_cast<const FObjectProperty*>(Proposal.DraftRootProperty)->GetObjectPropertyValue(Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			if (Value && !Cast<DStaticMesh>(Value))
			{
				OutError = "Selected asset is not a static mesh.";
				return false;
			}
		}
		return true;
	}

	auto DSplineMeshComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty || (Event.Phase == EPropertyChangePhase::Committed && Event.Origin == EPropertyChangeOrigin::Edit)) return;
		const FName Name = Event.MemberProperty->NamePrivate;
		if (Name == FName("OverrideMaterials"))
		{
			TrimTrailingNullOverrides();
			++MaterialComponentRevision;
			MarkRenderStateDirty();
			return;
		}
		if (Name == FName("CollisionMode"))
		{
			RebuildCollisionGeometryForPublishedState();
			RecreatePhysicsState();
			return;
		}
		if (Name == FName("StaticMesh") || Name == FName("SplineMeshParams"))
		{
			RebuildDerivedState();
			if (Name == FName("StaticMesh")) MarkRenderStateDirty();
#if DURIN_WITH_EDITOR
			else if (IsRegistered()) NotifyEditorPickingMutation();
#endif
			if (Name == FName("SplineMeshParams")) PushDynamicDataToScene();
			RecreatePhysicsState();
		}
	}

#if DURIN_WITH_EDITOR
	auto DSplineMeshComponent::GetEditorPickingLocalBounds(FBox& OutBounds, EEditorPickingPrimitiveFamily& OutFamily) const -> bool
	{
		const auto State = GetDerivedState();
		if (!State || !State->IsValid()) return false;
		OutBounds = State->ConservativeLocalBounds;
		OutFamily = EEditorPickingPrimitiveFamily::SplineMesh;
		return OutBounds.bIsValid && Math::IsFinite(OutBounds.Min) && Math::IsFinite(OutBounds.Max);
	}
#endif

	auto DSplineMeshComponent::HandleStaticMeshRenderDataChanged(DStaticMesh* ChangedMesh) -> void
	{
		if (!ChangedMesh || ChangedMesh != StaticMesh.Get()) return;
		++MaterialComponentRevision;
		RebuildDerivedState();
		MarkRenderStateDirty();
		RecreatePhysicsState();
	}

	auto DSplineMeshComponent::BuildMaterialRenderProxyBindingUpdate(
		FMaterialRenderProxyBindingUpdate& OutUpdate) -> bool
	{
		DMaterialInterface* CurrentMaterial = GetMaterial(PendingMaterialSlotIndex);
		if (!CurrentMaterial) RecordMaterialFallbackReason(EMaterialFallbackReason::UnassignedDefault);
		OutUpdate.SlotIndex = PendingMaterialSlotIndex;
		OutUpdate.MaterialProxy = CurrentMaterial
			? CurrentMaterial->GetMaterialRenderProxy()
			: GetDefaultMaterialRenderProxy();
		OutUpdate.ComponentRevision = MaterialComponentRevision;
		return true;
	}
}
