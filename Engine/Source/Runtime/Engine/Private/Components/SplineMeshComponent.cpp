#include "Components/SplineMeshComponent.h"
#include "Logging/LogMacros.h"

#include "Components/ComponentMaterialOverride.h"

#include "DObject/DurinPropertyTypes.h"
#include "Rendering/SplineMeshSceneProxy.h"
#include "Engine/Level.h"
#include "SceneInterface.h"
#include "Materials/MaterialInterface.h"
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

	auto DSplineMeshComponent::SetStaticMesh(DStaticMesh* InStaticMesh, bool bUpdateMesh) -> void
	{
		if (StaticMesh != InStaticMesh)
		{
			StaticMesh = InStaticMesh;
			bSourceDirty = true;
			MarkPackageDirty();
		}
		if (bUpdateMesh) UpdateMesh();
	}

	auto DSplineMeshComponent::SetSplineMeshParams(const FSplineMeshParams& InParams, std::string* OutError) -> bool
	{
		return SetSplineMeshParams(InParams, true, OutError);
	}

	auto DSplineMeshComponent::SetSplineMeshParams(
		const FSplineMeshParams& InParams, bool bUpdateMesh, std::string* OutError) -> bool
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
		if (SplineMeshParams == Normalized) return !bUpdateMesh || UpdateMesh(OutError);
		const FSplineMeshParams Previous = SplineMeshParams;
		const bool bWasDeformationDirty = bDeformationDirty;
		SplineMeshParams = Normalized;
		bDeformationDirty = true;
		if (bUpdateMesh && !UpdateMesh(OutError))
		{
			SplineMeshParams = Previous;
			bDeformationDirty = bWasDeformationDirty;
			return false;
		}
		MarkPackageDirty();
		return true;
	}

	auto DSplineMeshComponent::SetSplineMeshCollisionMode(ESplineMeshCollisionMode InMode, bool bUpdateMesh) -> void
	{
		if (CollisionMode != InMode)
		{
			CollisionMode = InMode;
			bCollisionDirty = true;
			MarkPackageDirty();
		}
		if (bUpdateMesh) UpdateMesh();
	}

	auto DSplineMeshComponent::UpdateMesh(std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		if (bUpdatingMesh || !IsMeshDirty()) return true;
		bUpdatingMesh = true;
		if (bSourceDirty && StaticMesh) StaticMesh->RequestRenderDataAndResources();
		const bool bRebuildDeformation = bSourceDirty || bDeformationDirty;
		const bool bRecreateRenderState = bSourceDirty;
		const bool bSuccess = bRebuildDeformation
			? RebuildDerivedState(OutError) : RebuildCollisionGeometryForPublishedState();
		bUpdatingMesh = false;
		if (!bSuccess)
		{
			if (OutError && OutError->empty()) *OutError = "SplineMesh collision geometry is unavailable.";
			return false;
		}
		PublishedCollisionMode = CollisionMode;
		bSourceDirty = bDeformationDirty = bCollisionDirty = false;
		if (bRecreateRenderState)
		{
			++MaterialComponentRevision;
			MarkRenderStateDirty();
		}
		else if (bRebuildDeformation)
		{
#if DURIN_WITH_EDITOR
			if (IsRegistered()) NotifyEditorPickingMutation();
#endif
			PushDynamicDataToScene();
		}
		RecreatePhysicsState();
		return true;
	}

	auto DSplineMeshComponent::RebuildCollisionGeometryForPublishedState() -> bool
	{
		const auto Published = CollisionMode == ESplineMeshCollisionMode::DeformedTriangleMesh
			? GetDerivedStateForQueries() : GetDerivedState();
		if (!Published) return false;
		if (!Published->IsValid()) return true;
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
		return true;
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
		if (!Candidate->ConservativeLocalBounds.bIsValid
			|| !Math::IsFinite(Candidate->ConservativeLocalBounds.Min)
			|| !Math::IsFinite(Candidate->ConservativeLocalBounds.Max))
		{
			if (OutError) *OutError = "SplineMesh deformation produced invalid bounds.";
			return false;
		}
		if (CollisionMode == ESplineMeshCollisionMode::DeformedTriangleMesh
			&& !BuildDerivedGeometry(*Candidate, OutError)) return false;
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

	auto DSplineMeshComponent::BuildDerivedGeometry(
		FSplineMeshDerivedState& Candidate, std::string* OutError) const -> bool
	{
		const FStaticMeshRenderData* RenderData = StaticMesh ? StaticMesh->GetRenderData() : nullptr;
		if (!RenderData || RenderData->LODResources.empty()
			|| StaticMesh->GetRenderResourceStatus().Revision != Candidate.SourceRenderResourceRevision)
		{
			if (OutError) *OutError = "SplineMesh source geometry is unavailable or has changed.";
			return false;
		}
		const auto& SourceLOD = RenderData->LODResources[0];
		const auto& Positions = SourceLOD.VertexBuffers.PositionVertexBuffer.GetPositions();
		const auto& Indices = SourceLOD.IndexBuffer.GetIndices();
		Candidate.DeformedLOD0Positions.reserve(Positions.size());
		for (const FVector3f& Position : Positions)
		{
			const FVector3 Deformed = FSplineMeshDeformer::DeformPosition(Candidate.Params, FVector3(Position));
			if (!Math::IsFinite(Deformed))
			{
				if (OutError) *OutError = "SplineMesh deformation produced a non-finite position.";
				return false;
			}
			Candidate.DeformedLOD0Positions.emplace_back(Deformed);
		}
		Candidate.LOD0Indices = Indices;
		for (uint32 Index : Indices)
		{
			if (Index >= Candidate.DeformedLOD0Positions.size())
			{
				if (OutError) *OutError = "StaticMesh LOD 0 contains an out-of-range index.";
				return false;
			}
		}
		FStaticMeshLODResources QueryLOD;
		QueryLOD.VertexBuffers.PositionVertexBuffer.Init(Candidate.DeformedLOD0Positions);
		QueryLOD.IndexBuffer.Init(Candidate.LOD0Indices);
		QueryLOD.LocalBounds = Candidate.ConservativeLocalBounds;
		Candidate.EditorAcceleration = BuildStaticMeshRayQueryAcceleration(QueryLOD);
		return true;
	}

	auto DSplineMeshComponent::GetDerivedStateForQueries() -> std::shared_ptr<const FSplineMeshDerivedState>
	{
		const auto Published = GetDerivedState();
		if (!Published || !Published->IsValid()) return Published;
		if (!Published->DeformedLOD0Positions.empty()) return Published;
		// A pending mesh replacement must not deform the new source with the old snapshot.
		if (bSourceDirty) return nullptr;
		auto Candidate = std::make_shared<FSplineMeshDerivedState>(*Published);
		if (!BuildDerivedGeometry(*Candidate, nullptr)) return nullptr;
		std::atomic_store_explicit(&DerivedState,
			std::shared_ptr<const FSplineMeshDerivedState>(Candidate), std::memory_order_release);
		return Candidate;
	}

	auto DSplineMeshComponent::BuildCollisionGeometry(
		FCollisionGeometryRef& OutGeometry, FTransform& OutWorldTransform) const -> bool
	{
		if (PublishedCollisionMode != ESplineMeshCollisionMode::DeformedTriangleMesh) return false;
		const auto State = GetDerivedState();
		if (!State || !State->IsValid() || !State->CollisionGeometry.IsValid()) return false;
		OutGeometry = State->CollisionGeometry;
		OutWorldTransform = GetWorldTransform();
		return IsValidPhysicsTransform(OutWorldTransform);
	}

	auto DSplineMeshComponent::GetCollisionStateRevision() const -> uint64
	{
		const auto State = GetDerivedState();
		return PublishedCollisionMode == ESplineMeshCollisionMode::DeformedTriangleMesh && State
			? State->CollisionInputIdentity : 0;
	}

	auto DSplineMeshComponent::SetMaterial(DMaterialInterface* InMaterial) -> bool
	{
		return SetMaterial(0, InMaterial);
	}

	auto DSplineMeshComponent::SetMaterial(uint32 SlotIndex, DMaterialInterface* InMaterial) -> bool
	{
		const auto Result = ComponentMaterialOverride::Set(
			OverrideMaterials, SlotIndex, StaticMesh && StaticMesh->GetMaterialSlot(SlotIndex),
			InMaterial, MaterialComponentRevision, PendingMaterialSlotIndex);
		if (Result == ComponentMaterialOverride::EMutationResult::InvalidSlot) return false;
		if (Result == ComponentMaterialOverride::EMutationResult::Unchanged) return true;
		MarkPackageDirty();
		MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::MaterialBinding);
		return true;
	}

	auto DSplineMeshComponent::GetMaterial(uint32 SlotIndex) const -> DMaterialInterface*
	{
		const FMeshMaterialSlotDefinition* Slot = StaticMesh ? StaticMesh->GetMaterialSlot(SlotIndex) : nullptr;
		if (!Slot) return nullptr;
		return ComponentMaterialOverride::Resolve(OverrideMaterials, SlotIndex, Slot->DefaultMaterial.Get());
	}

	auto DSplineMeshComponent::ResetMaterial(uint32 SlotIndex) -> bool
	{
		const auto Result = ComponentMaterialOverride::Set(
			OverrideMaterials, SlotIndex, StaticMesh && StaticMesh->GetMaterialSlot(SlotIndex),
			nullptr, MaterialComponentRevision, PendingMaterialSlotIndex);
		if (Result != ComponentMaterialOverride::EMutationResult::Changed) return false;
		MarkPackageDirty();
		MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::MaterialBinding);
		return true;
	}

	auto DSplineMeshComponent::ClearMaterialOverrides() -> bool
	{
		if (!ComponentMaterialOverride::Clear(
			OverrideMaterials, MaterialComponentRevision, PendingMaterialSlotIndex)) return false;
		MarkPackageDirty();
		MarkRenderStateDirty();
		return true;
	}

	auto DSplineMeshComponent::GetMaterialOverride(uint32 SlotIndex) const -> DMaterialInterface*
	{
		return ComponentMaterialOverride::Get(OverrideMaterials, SlotIndex);
	}

	auto DSplineMeshComponent::GetNumMaterials() const -> uint32
	{
		return StaticMesh ? StaticMesh->GetNumMaterialSlots() : 0;
	}

	auto DSplineMeshComponent::CreateSceneProxy() -> std::unique_ptr<FPrimitiveSceneProxy>
	{
		if (!StaticMesh) return nullptr;
		StaticMesh->RequestRenderDataAndResources();
		if (IsMeshDirty()) return nullptr;
		const auto State = GetDerivedState();
		if (!State || !State->IsValid()) return nullptr;
		const FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
		if (!RenderData
			|| RenderData->LODResources.empty() || RenderData->LODResources[0].GetNumIndices() == 0)
			return nullptr;
		std::vector<FMaterialRenderProxyRef> MaterialProxies;
		MaterialProxies.reserve(RenderData->MaterialSlots.size());
		for (uint32 SlotIndex = 0; SlotIndex < RenderData->MaterialSlots.size(); ++SlotIndex)
		{
			DMaterialInterface* SlotMaterial = GetMaterial(SlotIndex);
			MaterialProxies.push_back(ComponentMaterialOverride::ResolveRenderProxy(SlotMaterial));
		}
		return std::make_unique<FSplineMeshSceneProxy>(RenderData, std::move(MaterialProxies),
			MaterialComponentRevision, FSplineMeshRenderDynamicData{
				.Params = State->Params,
				.LocalBounds = State->ConservativeLocalBounds,
				.Revision = State->DeformationRevision});
	}

	auto DSplineMeshComponent::OnRegister() -> void
	{
		if (StaticMesh) StaticMesh->RequestRenderDataAndResources();
		const auto State = GetDerivedState();
		if (StaticMesh && (!State || !State->IsValid()
			|| State->SourceRenderResourceRevision != StaticMesh->GetRenderResourceStatus().Revision))
			bSourceDirty = true;
		UpdateMesh();
		Super::OnRegister();
	}

	auto DSplineMeshComponent::PushDynamicDataToScene() -> void
	{
		if (!IsRegistered()) return;
		FSceneInterface* Scene = GetRenderScene();
		const auto State = GetDerivedState();
		if (!Scene || !State || !State->IsValid()) return;
		Scene->UpdateSplineMeshDynamicData(GetPrimitiveComponentId(), FSplineMeshRenderDynamicData{
			.Params = State->Params,
			.LocalBounds = State->ConservativeLocalBounds,
			.Revision = State->DeformationRevision});
	}

	auto DSplineMeshComponent::ValidateOverrideMaterials(
		std::span<const TObjectPtr<DMaterialInterface>> Overrides, std::string& OutError) const -> bool
	{
		return ValidateStaticMeshMaterialOverrides(Overrides, "SplineMesh component", OutError);
	}

	auto DSplineMeshComponent::PostLoad() -> void
	{
		std::string Error;
		Super::PostLoad();
		if (!ValidateOverrideMaterials(OverrideMaterials, Error))
		{
			DURIN_ERROR("PostLoad '{}': {}; clearing material overrides.", GetObjectPath(), Error);
			OverrideMaterials.clear();
		}
		ComponentMaterialOverride::TrimTrailingNulls(OverrideMaterials);
		bSourceDirty = true;
		if (!UpdateMesh(&Error))
		{
			DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
			return;
		}
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
			ComponentMaterialOverride::TrimTrailingNulls(OverrideMaterials);
			++MaterialComponentRevision;
			MarkRenderStateDirty();
			return;
		}
		if (Name == FName("CollisionMode"))
		{
			bCollisionDirty = true;
			UpdateMesh();
			return;
		}
		if (Name == FName("StaticMesh") || Name == FName("SplineMeshParams"))
		{
			bSourceDirty |= Name == FName("StaticMesh");
			bDeformationDirty = true;
			UpdateMesh();
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
		const bool bHadPendingEdits = IsMeshDirty();
		bSourceDirty = true;
		if (!bHadPendingEdits) UpdateMesh();
	}

	auto DSplineMeshComponent::BuildMaterialRenderProxyBindingUpdate(
		FMaterialRenderProxyBindingUpdate& OutUpdate) -> bool
	{
		ComponentMaterialOverride::BuildRenderProxyBindingUpdate(
			PendingMaterialSlotIndex, GetMaterial(PendingMaterialSlotIndex),
			MaterialComponentRevision, OutUpdate);
		return true;
	}
}
