#include "Components/PropertyEditValidation.h"
#include "Components/SplineMeshComponent.h"
#include "Logging/LogMacros.h"

#include "Components/ComponentMaterialOverride.h"

#include "DObject/DurinPropertyTypes.h"
#include "Rendering/SplineMeshSceneProxy.h"
#include "Rendering/SceneInterface.h"
#include "Materials/MaterialInterface.h"
#include "Spline/SplineMeshDeformer.h"
#include "StaticMesh/StaticMesh.h"
#include "Physics/BodySetup.h"
#include "DObject/DObjectGlobals.h"
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

		auto DeformSimpleCollision(const DBodySetup& Source, const FSplineMeshParams& Params)
			-> std::expected<std::vector<FSimpleCollisionElement>, std::string>
		{
			FCollisionGeometryRef Geometry;
			if (!Source.BuildSimpleGeometry(Geometry)) return std::vector<FSimpleCollisionElement>{};
			std::vector<FCollisionGeometryChild> Children;
			if (Geometry.GetKind() == ECollisionGeometryKind::ConvexHull)
				Children.push_back({.Geometry = Geometry});
			else for (uint32 Index = 0; Index < Geometry.GetChildCount(); ++Index)
				Children.push_back(*Geometry.GetChild(Index));
			if (Source.GetSimpleCollision().empty() && !Source.GetResidentSimpleGeometry())
				for (auto& Child : Children) Child.LocalTransform.Translation += Source.GetCenter();
			std::vector<FSimpleCollisionElement> Result;
			for (const auto& Child : Children)
			{
				FSimpleCollisionElement Element;
				const FMatrix Matrix = Child.LocalTransform.ToMatrix();
				if (Child.Geometry || Child.Shape.GetType() == ECollisionShapeType::Box)
				{
					Element.Type = EBodySetupShapeType::Convex;
					const uint32 Count = Child.Geometry ? Child.Geometry.GetVertexCount() : 8;
					for (uint32 Index = 0; Index < Count; ++Index)
					{
						const FVector3 Extent = Child.Shape.GetBoxHalfExtent();
						const FVector3 Point = Child.Geometry ? *Child.Geometry.GetVertex(Index)
							: FVector3((Index & 1) ? Extent.x : -Extent.x,
								(Index & 2) ? Extent.y : -Extent.y, (Index & 4) ? Extent.z : -Extent.z);
						Element.Vertices.push_back(FSplineMeshDeformer::DeformPosition(Params, FVector3(Matrix * FVector4(Point, 1.0))));
					}
				}
				else
				{
					const FVector3 Center = Child.LocalTransform.Translation;
					const uint32 Forward = static_cast<uint32>(Params.ForwardAxis);
					const auto Sample = FSplineMeshDeformer::Evaluate(Params,
						(Center[Forward] - Params.SourceForwardMin) / (Params.SourceForwardMax - Params.SourceForwardMin));
					const double SliceScale = std::max({1.0, std::abs(Sample.Scale.x), std::abs(Sample.Scale.y)});
					const double SourceScale = std::max({Child.LocalTransform.Scale3D.x,
						Child.LocalTransform.Scale3D.y, Child.LocalTransform.Scale3D.z});
					const double Radius = Child.Shape.GetSphereRadius() * SourceScale * SliceScale;
					Element.Transform.Translation = FSplineMeshDeformer::DeformPosition(Params, Center);
					Element.Type = EBodySetupShapeType::Sphere;
					Element.Dimensions = FVector3(Radius);
					if (Child.Shape.GetType() == ECollisionShapeType::Capsule)
					{
						// UE Sphyl.Length excludes caps; our HalfHeight includes them.
						const double SourceRadius = Child.Shape.GetCapsuleRadius()
							* std::max(Child.LocalTransform.Scale3D.x, Child.LocalTransform.Scale3D.y);
						const double HalfCylinder = std::max(SourceRadius,
							Child.Shape.GetCapsuleHalfHeight() * Child.LocalTransform.Scale3D.z) - SourceRadius;
						const double CapsuleRadius = SourceRadius * SliceScale;
						const FVector3 Axis = Math::RotateVector(Child.LocalTransform.Rotation, FVectorConstants::Up);
						const FVector3 SliceAxis = Sample.Frame.Forward * Axis[Forward]
							+ Sample.Frame.Side * (Axis[(Forward + 1) % 3] * Sample.Scale.x)
							+ Sample.Frame.Up * (Axis[(Forward + 2) % 3] * Sample.Scale.y);
						const double Length = Math::Length(SliceAxis);
						if (!std::isfinite(Length))
							return std::unexpected("SplineMesh capsule produced a non-finite axis.");
						const FVector3 Z = Length > 1.e-12 ? SliceAxis / Length : Sample.Frame.Up;
						const FVector3 X = Math::NormalizeOr(Math::Cross(std::abs(Z.z) < 0.9 ? FVectorConstants::Up : FVectorConstants::Right, Z), FVectorConstants::Forward);
						const FVector3 Y = Math::Cross(Z, X);
						FMatrix Rotation(1.0);
						Rotation[0] = FVector4(X, 0.0); Rotation[1] = FVector4(Y, 0.0); Rotation[2] = FVector4(Z, 0.0);
						Element.Transform.Rotation = Math::QuaternionFromMatrix(Rotation);
						Element.Type = EBodySetupShapeType::Capsule;
						Element.Dimensions = FVector3(CapsuleRadius, CapsuleRadius, CapsuleRadius + HalfCylinder * Length);
					}
				}
				Result.push_back(std::move(Element));
			}
			return Result;
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
		if (!IsMeshDirty() && !bRetryDeformation && CollisionBuildError.empty()) return;
		bUpdatingMesh = true;
		if (bSourceDirty && StaticMesh) StaticMesh->RequestRenderDataAndResources();
		const auto Previous = GetDerivedState();
		const bool bSourceChanged = std::exchange(bSourceDirty, false);
		const bool bRebuildDeformation = std::exchange(bDeformationDirty, false) || bSourceChanged || bRetryDeformation;
		const bool bRebuildSimple = std::exchange(bSimpleCollisionDirty, false) || bRebuildDeformation || !CollisionBuildError.empty();
		const bool bRebuildComplex = std::exchange(bComplexCollisionDirty, false) || bRebuildDeformation || !CollisionBuildError.empty();
		bCollisionDirty = false;
		bool bRecreateRenderState = bSourceChanged;
		MeshUpdateError.clear();
		if (bRebuildDeformation && !RebuildDerivedState(&MeshUpdateError))
		{
			DURIN_ERROR("SplineMesh update '{}': {}", GetObjectPath(), MeshUpdateError);
		}
		PublishedCollisionSourceRevision = StaticMesh ? StaticMesh->GetCollisionSourceRevision() : 0;
		RebuildCollisionGeometryForPublishedState(bRebuildSimple, bRebuildComplex);
		PublishedCollisionMode = CollisionMode;
		bUpdatingMesh = false;
		const auto Current = GetDerivedState();
		// Invalid dynamic data cannot remove a scene proxy; recreate it on readiness transitions.
		bRecreateRenderState |= !Previous || !Current || Previous->IsValid() != Current->IsValid();
		if (bRecreateRenderState)
		{
			MarkRenderStateDirty();
		}
		else if (bRebuildDeformation)
		{
			PushDynamicDataToScene();
		}
		RecreatePhysicsState();
		if (IsMeshDirty()) UpdateMesh();
	}

	auto DSplineMeshComponent::RebuildCollisionGeometryForPublishedState(bool bRebuildSimple, bool bRebuildComplex) -> void
	{
		CollisionBuildError.clear();
		const auto State = GetDerivedState();
		if (!State || !State->IsValid() || CollisionMode == ESplineMeshCollisionMode::Disabled)
		{
			if (BodySetup)
			{
				const FScopedPackageDirtySuppression SuppressDirty;
				const FScopedBodySetupUpdate Update(*BodySetup);
				BodySetup->SetSimpleCollision({});
				BodySetup->InvalidatePhysicsData();
			}
			return;
		}
		const FScopedPackageDirtySuppression SuppressDirty;
		if (!BodySetup || BodySetup->GetOuter() != this)
			BodySetup = NewObject<DBodySetup>(this, "SplineMeshBodySetup", EObjectFlags::Transient);
		const FScopedBodySetupUpdate Update(*BodySetup);
		const bool bSimple = CollisionMode != ESplineMeshCollisionMode::DeformedTriangleMesh;
		const auto TraceMode = bSimple && StaticMesh && StaticMesh->GetBodySetup()
			? StaticMesh->GetBodySetup()->GetCollisionTraceMode() : EBodySetupCollisionTraceMode::SimpleAndComplex;
		const bool bComplex = TraceMode == EBodySetupCollisionTraceMode::ComplexAsSimple
			|| (CollisionMode != ESplineMeshCollisionMode::DeformedSimpleCollision
				&& TraceMode != EBodySetupCollisionTraceMode::SimpleAsComplex);
		BodySetup->SetCollisionSourceMode(bComplex ? EBodySetupCollisionSourceMode::TriangleMeshFromLOD0 : EBodySetupCollisionSourceMode::None);
		BodySetup->SetCollisionQueryPolicy(bSimple ? EBodySetupCollisionQueryPolicy::SimpleAndComplex : EBodySetupCollisionQueryPolicy::ComplexOnly);
		BodySetup->SetCollisionTraceMode(TraceMode);
		// Prepare all authored simple inputs before cooking generated geometry. The scope
		// publishes one revision, with no intermediate complex product to save/reinstall.
		const bool bUseSimple = bSimple && StaticMesh && StaticMesh->GetBodySetup()
			&& TraceMode != EBodySetupCollisionTraceMode::ComplexAsSimple;
		if (bUseSimple && (bRebuildSimple || BodySetup->GetSimpleCollision().empty()))
		{
			auto Elements = DeformSimpleCollision(*StaticMesh->GetBodySetup(), State->Params);
			if (!Elements) CollisionBuildError = Elements.error();
			else if (!BodySetup->SetSimpleCollision(std::move(*Elements)))
				CollisionBuildError = "SplineMesh simple collision deformation is invalid.";
		}
		else if (!bUseSimple) BodySetup->SetSimpleCollision({});
		if (CollisionBuildError.empty() && bComplex && (bRebuildComplex || !BodySetup->GetResidentComplexGeometry()))
		{
			BodySetup->CreatePhysicsMeshes(*this, false);
			if (const auto& Error = BodySetup->GetPhysicsMeshBuildError(); Error) CollisionBuildError = *Error;
			else if (!BodySetup->GetResidentComplexGeometry()) CollisionBuildError = "SplineMesh has no usable collision triangles.";
		}
		else if (!bComplex) BodySetup->InvalidatePhysicsData();
		if (!CollisionBuildError.empty())
		{
			BodySetup->SetSimpleCollision({});
			BodySetup->InvalidatePhysicsData();
		}
		if (!CollisionBuildError.empty())
			DURIN_ERROR("SplineMesh collision '{}': {}", GetObjectPath(), CollisionBuildError);
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
		Candidate->Status = ESplineMeshDerivedStateStatus::Valid;
		Candidate->Diagnostic.clear();
		DeformationRevision = Candidate->DeformationRevision;
		SplineMeshParams = Params;
		std::atomic_store_explicit(&DerivedState, std::shared_ptr<const FSplineMeshDerivedState>(Candidate), std::memory_order_release);
		if (OutError) OutError->clear();
		return true;
	}

	auto DSplineMeshComponent::ContainsPhysicsTriMeshData() const -> bool
	{
		const auto State = GetDerivedState();
		return StaticMesh && State && State->IsValid() && (!bSourceDirty || bUpdatingMesh)
			&& StaticMesh->GetCollisionSourceRevision() == PublishedCollisionSourceRevision
			&& StaticMesh->GetRenderResourceStatus().Revision == State->SourceRenderResourceRevision
			&& StaticMesh->ContainsPhysicsTriMeshData();
	}

	auto DSplineMeshComponent::GetPhysicsTriMeshData() const -> std::expected<FTriMeshCollisionData, std::string>
	{
		if (!ContainsPhysicsTriMeshData())
			return std::unexpected("SplineMesh source geometry is unavailable or has changed.");
		const auto State = GetDerivedState();
		auto Data = StaticMesh->GetPhysicsTriMeshData();
		if (!Data) return std::unexpected(Data.error());
		for (auto& Position : Data->Positions)
		{
			const FVector3 Deformed = FSplineMeshDeformer::DeformPosition(State->Params, FVector3(Position));
			if (!Math::IsFinite(Deformed) || !Math::IsFinite(FVector3f(Deformed)))
				return std::unexpected("SplineMesh deformation produced a non-finite position.");
			Position = FVector3f(Deformed);
		}
		for (uint32 Index : Data->Indices)
			if (Index >= Data->Positions.size())
				return std::unexpected("SplineMesh collision source contains an out-of-range index.");
		return Data;
	}

	auto DSplineMeshComponent::ConfigureCollisionQueries(FPhysicsBodyDesc& Desc) const -> void
	{
		if (auto* Setup = GetBodySetup()) Setup->ConfigureQueryGeometry(Desc, GetWorldTransform());
	}

	auto DSplineMeshComponent::BuildCollisionGeometry(
		FCollisionGeometryRef& OutGeometry, FTransform& OutWorldTransform) const -> bool
	{
		if (PublishedCollisionMode == ESplineMeshCollisionMode::Disabled || !BodySetup) return false;
		FTransform LocalTransform;
		if (!BodySetup->BuildGeometry(OutGeometry, LocalTransform)) return false;
		OutWorldTransform = FTransform::Combine(GetWorldTransform(), LocalTransform);
		return IsValidPhysicsTransform(OutWorldTransform);
	}

	auto DSplineMeshComponent::GetCollisionStateRevision() const -> uint64
	{
		return PublishedCollisionMode != ESplineMeshCollisionMode::Disabled && BodySetup
			? BodySetup->GetRevision() : 0;
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
		return std::make_unique<FSplineMeshSceneProxy>(RenderData, std::move(MaterialProxies), FSplineMeshRenderDynamicData{
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

	auto DSplineMeshComponent::PreEditChangeProperty(FPropertyEditProposal& Proposal) -> std::expected<void, FObjectValidationError>
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


	auto DSplineMeshComponent::HandleStaticMeshRenderDataChanged(DStaticMesh* ChangedMesh) -> void
	{
		if (!ChangedMesh || ChangedMesh != StaticMesh.Get()) return;
		const bool bHadPendingEdits = IsMeshDirty();
		bSourceDirty = true;
		if (!bHadPendingEdits) UpdateMesh();
	}

	auto DSplineMeshComponent::HandleStaticMeshCollisionSourceChanged(DStaticMesh* ChangedMesh, EBodySetupChange Changes) -> void
	{
		if (!ChangedMesh || ChangedMesh != StaticMesh.Get()) return;
		const bool bHadPendingEdits = IsMeshDirty();
		const auto State = GetDerivedState();
		if (!State || State->SourceRenderResourceRevision != ChangedMesh->GetRenderResourceStatus().Revision)
			bSourceDirty = true;
		bSimpleCollisionDirty |= EnumHasAnyFlags(Changes, EBodySetupChange::SimpleGeometry | EBodySetupChange::SourceRecipe);
		bComplexCollisionDirty |= EnumHasAnyFlags(Changes, EBodySetupChange::GeneratedGeometry);
		bCollisionDirty = true;
		if (!bHadPendingEdits) UpdateMesh();
	}

}
