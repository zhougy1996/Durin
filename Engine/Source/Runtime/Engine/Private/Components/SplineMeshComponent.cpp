#include "Components/PropertyEditValidation.h"
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

	auto DSplineMeshComponent::SetSplineMeshParams(const FSplineMeshParams& InParams, bool bUpdateMesh) -> void
	{
		if (SplineMeshParams != InParams)
		{
			SplineMeshParams = InParams;
			bDeformationDirty = true;
			MarkPackageDirty();
		}
		if (bUpdateMesh) UpdateMesh();
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

	auto DSplineMeshComponent::UpdateMesh() -> void
	{
		if (bUpdatingMesh) return;
		const bool bRetryDeformation = !MeshUpdateError.empty();
		if (!IsMeshDirty() && !bRetryDeformation && CollisionBuildError.empty() && QueryBuildError.empty()) return;
		bUpdatingMesh = true;
		if (bSourceDirty && StaticMesh) StaticMesh->RequestRenderDataAndResources();
		const auto Previous = GetDerivedState();
		const bool bRebuildDeformation = bSourceDirty || bDeformationDirty || bRetryDeformation;
		bool bRecreateRenderState = bSourceDirty;
		MeshUpdateError.clear();
		QueryBuildError.clear();
		if (bRebuildDeformation && !RebuildDerivedState(&MeshUpdateError))
		{
			DURIN_ERROR("SplineMesh update '{}': {}", GetObjectPath(), MeshUpdateError);
		}
		RebuildCollisionGeometryForPublishedState();
		PublishedCollisionMode = CollisionMode;
		bSourceDirty = bDeformationDirty = bCollisionDirty = false;
		bUpdatingMesh = false;
		const auto Current = GetDerivedState();
		// Invalid dynamic data cannot remove a scene proxy; recreate it on readiness transitions.
		bRecreateRenderState |= !Previous || !Current || Previous->IsValid() != Current->IsValid();
		if (bRecreateRenderState)
		{
			AdvanceMaterialBindingRevision();
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
	}

	auto DSplineMeshComponent::RebuildCollisionGeometryForPublishedState() -> void
	{
		CollisionBuildError.clear();
		const auto Published = GetDerivedState();
		if (!Published) return;
		auto Candidate = std::make_shared<FSplineMeshDerivedState>(*Published);
		Candidate->CollisionGeometry = {};
		if (Candidate->IsValid() && CollisionMode == ESplineMeshCollisionMode::DeformedTriangleMesh)
		{
			if (!Candidate->DeformedLOD0Positions.empty() || BuildDerivedGeometry(*Candidate, &CollisionBuildError))
			{
				std::vector<FVector3> CollisionPositions;
				CollisionPositions.reserve(Candidate->DeformedLOD0Positions.size());
				for (const FVector3f& Position : Candidate->DeformedLOD0Positions)
					CollisionPositions.emplace_back(Position);
				Candidate->CollisionGeometry = FCollisionGeometryRef::BuildTriangleMesh(
					CollisionPositions, Candidate->LOD0Indices);
				if (!Candidate->CollisionGeometry.IsValid())
					CollisionBuildError = "SplineMesh has no usable collision triangles.";
			}
			else
			{
				Candidate->DeformedLOD0Positions.clear();
				Candidate->LOD0Indices.clear();
				Candidate->EditorAcceleration.reset();
			}
			if (!CollisionBuildError.empty())
				DURIN_ERROR("SplineMesh collision '{}': {}", GetObjectPath(), CollisionBuildError);
		}
		std::atomic_store_explicit(&DerivedState,
			std::shared_ptr<const FSplineMeshDerivedState>(Candidate), std::memory_order_release);
	}

	auto DSplineMeshComponent::RebuildDerivedState(std::string* OutError) -> bool
	{
		auto Candidate = std::make_shared<FSplineMeshDerivedState>();
		Candidate->Params = SplineMeshParams;
		Candidate->DeformationRevision = DeformationRevision;
		const auto Fail = [&](ESplineMeshDerivedStateStatus Status, const std::string& Error) -> bool
		{
			Candidate->Status = Status;
			Candidate->Diagnostic = Error;
			Candidate->ConservativeLocalBounds = {};
			Candidate->DeformationRevision = ++DeformationRevision;
			if (OutError) *OutError = Error;
			std::atomic_store_explicit(&DerivedState,
				std::shared_ptr<const FSplineMeshDerivedState>(Candidate), std::memory_order_release);
			return false;
		};
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
			return Fail(ESplineMeshDerivedStateStatus::InvalidSourceData,
				"StaticMesh LOD 0 has no finite indexed geometry.");
		}

		FSplineMeshParams Params = SplineMeshParams;
		const auto [Minimum, Maximum] = SourceForwardRange(SourceLOD.LocalBounds, Params.ForwardAxis);
		Params.SourceForwardMin = Minimum;
		Params.SourceForwardMax = Maximum;
		const auto Validation = FSplineMeshDeformer::Normalize(Params, Params);
		if (!Validation)
			return Fail(ESplineMeshDerivedStateStatus::InvalidDeformation, FormatSplineMeshValidationError(Validation.Error));

		Candidate->Params = Params;
		Candidate->ConservativeLocalBounds = FSplineMeshDeformer::ComputeConservativeBounds(Params, RenderData->LocalBounds);
		if (!Candidate->ConservativeLocalBounds.bIsValid
			|| !Math::IsFinite(Candidate->ConservativeLocalBounds.Min)
			|| !Math::IsFinite(Candidate->ConservativeLocalBounds.Max))
		{
			return Fail(ESplineMeshDerivedStateStatus::InvalidDeformation,
				"SplineMesh deformation produced invalid bounds.");
		}
		Candidate->DeformationRevision = DeformationRevision + 1;
		Candidate->CollisionInputIdentity = MakeCollisionInputIdentity(
			Candidate->SourceRenderResourceRevision, Candidate->DeformationRevision);
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
			if (!Math::IsFinite(Deformed) || !Math::IsFinite(FVector3f(Deformed)))
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
		if (!QueryBuildError.empty()) return nullptr;
		auto Candidate = std::make_shared<FSplineMeshDerivedState>(*Published);
		if (!BuildDerivedGeometry(*Candidate, &QueryBuildError))
		{
			DURIN_ERROR("SplineMesh query '{}': {}", GetObjectPath(), QueryBuildError);
			return nullptr;
		}
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

	auto DSplineMeshComponent::GetMaterialIndex(FName SlotName) const -> std::optional<uint32>
	{
		return StaticMesh ? StaticMesh->GetMaterialIndex(SlotName) : std::nullopt;
	}

	auto DSplineMeshComponent::GetDefaultMaterial(uint32 SlotIndex) const -> DMaterialInterface*
	{
		const auto* Slot = StaticMesh ? StaticMesh->GetMaterialSlot(SlotIndex) : nullptr;
		return Slot ? Slot->DefaultMaterial.Get() : nullptr;
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
			GetMaterialBindingRevision(), FSplineMeshRenderDynamicData{
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

	auto DSplineMeshComponent::PostLoad() -> void
	{
		Super::PostLoad();
		bSourceDirty = true;
		UpdateMesh();
	}

	auto DSplineMeshComponent::PreEditChangeProperty(FPropertyEditProposal& Proposal) -> FObjectValidationResult
	{
		if (auto Result = Super::PreEditChangeProperty(Proposal); !Result) return Result;
		if (!Proposal.MemberProperty || !Proposal.DraftRootProperty || !Proposal.DraftRootContainer) return {};
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
			const auto Validation = FSplineMeshDeformer::Normalize(Candidate, Normalized);
			if (!Validation)
			{
				return RejectEnginePropertyEdit(*this, Proposal, Validation.Error);
			}
			*Params = Normalized;
			return {};
		}
		if (Proposal.MemberProperty->NamePrivate == FName("StaticMesh"))
		{
			if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Object) return RejectPropertyEdit(*this, Proposal, EPropertyEditRejection::InvalidMetadata);
			DObject* Value = static_cast<const FObjectProperty*>(Proposal.DraftRootProperty)->GetObjectPropertyValue(Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			if (Value && !Cast<DStaticMesh>(Value))
			{
				return RejectPropertyEdit(*this, Proposal, EPropertyEditRejection::IncompatibleObject);
			}
		}
		return {};
	}

	auto DSplineMeshComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty || (Event.Phase == EPropertyChangePhase::Committed && Event.Origin == EPropertyChangeOrigin::Edit)) return;
		const FName Name = Event.MemberProperty->NamePrivate;
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

}
