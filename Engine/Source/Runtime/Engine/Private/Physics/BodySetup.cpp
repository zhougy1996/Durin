#include "Physics/BodySetup.h"
#include "StaticMesh/StaticMesh.h"
#include "Physics/PhysicsMeshCompilation.h"
#include "Physics/PhysicsMeshInputTask.h"
#include "Threading/RunnableThread.h"
#include "CoreGlobals.h"

namespace Durin
{
	namespace
	{
		auto NotifyBodyMutation(DBodySetup& Body) -> void
		{
			Body.InvalidatePhysicsData();
			if (auto* Mesh = Cast<DStaticMesh>(Body.GetOuter()); Mesh && Mesh->GetBodySetup() == &Body)
				Mesh->NotifyCollisionSettingsChanged();
		}
	}
	DBodySetup::DBodySetup(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DBodySetup::NotifyPhysicsDataChanged() -> void
	{
		if (auto* Mesh = Cast<DStaticMesh>(GetOuter()); Mesh && Mesh->GetBodySetup() == this)
			Mesh->NotifyCollisionDataChanged();
	}

	auto DBodySetup::InvalidatePhysicsData() -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		++PhysicsMeshRequestGeneration;
		CancelPhysicsMeshCompilation(*this);
		CachedSimpleCollision = {};
		CachedComplexCollision = {};
		CachedGeometry = {};
		++Revision;
		PhysicsMeshError = {};
		PhysicsMeshStatus = CollisionSourceMode == EBodySetupCollisionSourceMode::None
			? EPhysicsMeshBuildStatus::Ready : EPhysicsMeshBuildStatus::Unavailable;
		NotifyPhysicsDataChanged();
	}

	auto DBodySetup::CreatePhysicsMeshesAsync(const IInterface_CollisionDataProvider& Provider,
		bool bPersistDerivedData, FOnAsyncPhysicsCookFinished Completion) -> std::expected<void, FPhysicsCookFailure>
	{
		InvalidatePhysicsData();
		if (CollisionSourceMode == EBodySetupCollisionSourceMode::None)
		{
			if (Completion) Completion({.Status = EPhysicsCookCompletionStatus::Succeeded});
			return {};
		}
		auto Source = Provider.CreatePhysicsMeshInputTask();
		if (!Source)
		{
			FailPhysicsMeshes(PhysicsMeshRequestGeneration, Source.error());
			return std::unexpected(Source.error());
		}
		const auto Submitted = SubmitPhysicsMeshCompilation(*this, std::move(*Source), bPersistDerivedData, std::move(Completion));
		if (!Submitted) FailPhysicsMeshes(PhysicsMeshRequestGeneration, Submitted.error());
		else PhysicsMeshStatus = EPhysicsMeshBuildStatus::Pending;
		return Submitted;
	}

	auto DBodySetup::CreatePhysicsMeshes(const IInterface_CollisionDataProvider& Provider,
		bool bPersistDerivedData) -> std::expected<void, FPhysicsCookFailure>
	{
		if (const auto Submitted = CreatePhysicsMeshesAsync(Provider, bPersistDerivedData); !Submitted) return Submitted;
		FinishPhysicsMeshes();
		if (PhysicsMeshStatus == EPhysicsMeshBuildStatus::Ready) return {};
		if (PhysicsMeshError) return std::unexpected(*PhysicsMeshError);
		if (PhysicsMeshStatus == EPhysicsMeshBuildStatus::Unavailable)
			return std::unexpected(FPhysicsCookFailure::Cancelled());
		return std::unexpected(FPhysicsCookFailure{"Physics mesh creation did not complete.", EPhysicsCookStage::Cook});
	}

	auto DBodySetup::GetCookInfo(FTriMeshCollisionData Data, bool bPersistDerivedData) const -> FCookBodySetupInfo
	{
		return {std::move(Data), CollisionSourceMode, CollisionQueryPolicy, bPersistDerivedData};
	}

	auto DBodySetup::FinishPhysicsMeshes() -> void { FinishPhysicsMeshCompilation(*this); }

	auto DBodySetup::ApplyPhysicsMeshes(uint64 Generation, const FCollisionGeometryRef& Simple,
		const FCollisionGeometryRef& Complex) -> EPhysicsMeshApplyResult
	{
		if (Generation != PhysicsMeshRequestGeneration || PhysicsMeshStatus != EPhysicsMeshBuildStatus::Pending)
			return EPhysicsMeshApplyResult::Superseded;
		if (SetCollisionGeometry(Simple, Complex)) return EPhysicsMeshApplyResult::Applied;
		FailPhysicsMeshes(Generation, FPhysicsCookFailure{"Could not install physics meshes.", EPhysicsCookStage::Installation});
		return EPhysicsMeshApplyResult::Failed;
	}

	auto DBodySetup::FailPhysicsMeshes(uint64 Generation, FPhysicsCookFailure Error) -> void
	{
		if (Generation != PhysicsMeshRequestGeneration) return;
		PhysicsMeshStatus = Error.IsCancelled() ? EPhysicsMeshBuildStatus::Unavailable : EPhysicsMeshBuildStatus::Failed;
		PhysicsMeshError = Error.IsCancelled() ? std::nullopt : std::optional{std::move(Error)};
	}

	auto DBodySetup::CancelPhysicsMeshes(uint64 Generation) -> void
	{
		if (Generation == PhysicsMeshRequestGeneration && PhysicsMeshStatus == EPhysicsMeshBuildStatus::Pending)
		{
			++PhysicsMeshRequestGeneration;
			PhysicsMeshStatus = EPhysicsMeshBuildStatus::Unavailable;
		}
	}

	auto DBodySetup::BeginDestroy() -> void
	{
		++PhysicsMeshRequestGeneration;
		CancelPhysicsMeshCompilation(*this);
		Super::BeginDestroy();
	}

		auto DBodySetup::SetBox(const FVector3& HalfExtent, const FVector3& InCenter) -> bool
	{
		const FCollisionShape Candidate = FCollisionShape::MakeBox(HalfExtent);
		if (!Candidate.IsValid() || !Math::IsFinite(InCenter)) return false;
		if (ShapeType == EBodySetupShapeType::Box && Dimensions == HalfExtent && Center == InCenter) return true;
		ShapeType = EBodySetupShapeType::Box;
		Dimensions = HalfExtent;
		Center = InCenter;
		++Revision;
		NotifyBodyMutation(*this);
		MarkPackageDirty();
		return true;
	}

	auto DBodySetup::SetSphere(double Radius, const FVector3& InCenter) -> bool
	{
		const FCollisionShape Candidate = FCollisionShape::MakeSphere(Radius);
		if (!Candidate.IsValid() || !Math::IsFinite(InCenter)) return false;
		if (ShapeType == EBodySetupShapeType::Sphere && Dimensions == FVector3(Radius) && Center == InCenter) return true;
		ShapeType = EBodySetupShapeType::Sphere;
		Dimensions = FVector3(Radius);
		Center = InCenter;
		++Revision;
		NotifyBodyMutation(*this);
		MarkPackageDirty();
		return true;
	}

	auto DBodySetup::SetCapsule(double Radius, double HalfHeight, const FVector3& InCenter) -> bool
	{
		const FCollisionShape Candidate = FCollisionShape::MakeCapsule(Radius, HalfHeight);
		if (!Candidate.IsValid() || !Math::IsFinite(InCenter)) return false;
		if (ShapeType == EBodySetupShapeType::Capsule && Dimensions == FVector3(Radius, Radius, HalfHeight) && Center == InCenter) return true;
		ShapeType = EBodySetupShapeType::Capsule;
		Dimensions = FVector3(Radius, Radius, HalfHeight);
		Center = InCenter;
		++Revision;
		NotifyBodyMutation(*this);
		MarkPackageDirty();
		return true;
	}

	auto DBodySetup::BuildShape(FCollisionShape& OutShape, FTransform& OutLocalTransform) const -> bool
	{
		switch (ShapeType)
		{
		case EBodySetupShapeType::Box: OutShape = FCollisionShape::MakeBox(Dimensions); break;
		case EBodySetupShapeType::Sphere: OutShape = FCollisionShape::MakeSphere(Dimensions.x); break;
		case EBodySetupShapeType::Capsule: OutShape = FCollisionShape::MakeCapsule(Dimensions.x, Dimensions.z); break;
		case EBodySetupShapeType::None: return false;
		}
		if (!OutShape.IsValid() || !Math::IsFinite(Center)) return false;
		OutLocalTransform = FTransform();
		OutLocalTransform.Translation = Center;
		return true;
	}

	auto DBodySetup::BuildGeometry(
		FCollisionGeometryRef& OutGeometry, FTransform& OutLocalTransform) const -> bool
	{
		if (CollisionSourceMode != EBodySetupCollisionSourceMode::None)
		{
			OutLocalTransform = FTransform();
			if (CollisionQueryPolicy != EBodySetupCollisionQueryPolicy::ComplexOnly
				&& BuildSimpleGeometry(OutGeometry)) return true;
			if (CollisionQueryPolicy != EBodySetupCollisionQueryPolicy::SimpleOnly
				&& BuildComplexGeometry(OutGeometry)) return true;
			return false;
		}
		FCollisionShape Shape;
		if (!BuildShape(Shape, OutLocalTransform)) return false;
		if (!CachedGeometry.IsValid() || CachedGeometryRevision != Revision)
		{
			CachedGeometry = FCollisionGeometryRef::MakePrimitive(Shape);
			CachedGeometryRevision = CachedGeometry.IsValid() ? Revision : 0;
		}
		OutGeometry = CachedGeometry;
		return OutGeometry.IsValid();
	}

	auto DBodySetup::BuildSimpleGeometry(FCollisionGeometryRef& OutGeometry) const -> bool
	{
		OutGeometry = CachedSimpleCollision;
		if (OutGeometry) return true;
		if (CollisionSourceMode != EBodySetupCollisionSourceMode::None) return false;
		FTransform LocalTransform;
		return BuildGeometry(OutGeometry, LocalTransform);
	}

	auto DBodySetup::BuildComplexGeometry(FCollisionGeometryRef& OutGeometry) const -> bool
	{
		OutGeometry = CachedComplexCollision;
		return OutGeometry.IsValid();
	}

	auto DBodySetup::SetCollisionSourceMode(EBodySetupCollisionSourceMode Mode) -> void
	{
		require(Mode == EBodySetupCollisionSourceMode::None
			|| Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0
			|| Mode == EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
		if (CollisionSourceMode == Mode) return;
		CollisionSourceMode = Mode;
		CachedSimpleCollision = {};
		CachedComplexCollision = {};
		++Revision;
		NotifyBodyMutation(*this);
		MarkPackageDirty();
	}

	auto DBodySetup::SetCollisionQueryPolicy(EBodySetupCollisionQueryPolicy Policy) -> void
	{
		require(Policy == EBodySetupCollisionQueryPolicy::SimpleOnly
			|| Policy == EBodySetupCollisionQueryPolicy::ComplexOnly
			|| Policy == EBodySetupCollisionQueryPolicy::SimpleAndComplex);
		if (CollisionQueryPolicy == Policy) return;
		CollisionQueryPolicy = Policy;
		++Revision;
		NotifyBodyMutation(*this);
		MarkPackageDirty();
	}

	auto DBodySetup::SetCollisionGeometry(
		const FCollisionGeometryRef& Simple,
		const FCollisionGeometryRef& Complex) -> bool
	{
		if (CollisionSourceMode == EBodySetupCollisionSourceMode::None
			|| (CollisionSourceMode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0
				&& (!Simple || Simple.GetKind() != ECollisionGeometryKind::ConvexHull))
			|| (CollisionSourceMode == EBodySetupCollisionSourceMode::TriangleMeshFromLOD0
				&& (!Complex || Complex.GetKind() != ECollisionGeometryKind::TriangleMesh))
			|| (Simple && Simple.GetKind() != ECollisionGeometryKind::ConvexHull)
			|| (Complex && Complex.GetKind() != ECollisionGeometryKind::TriangleMesh)) return false;
		CachedSimpleCollision = Simple;
		CachedComplexCollision = Complex;
		++CollisionBuildRevision;
		++Revision;
		PhysicsMeshStatus = EPhysicsMeshBuildStatus::Ready;
		PhysicsMeshError = {};
		NotifyPhysicsDataChanged();
		return true;
	}

	auto DBodySetup::ClearCollisionGeometry() -> void
	{
		InvalidatePhysicsData();
	}

	auto DBodySetup::IsValid(std::string* OutDiagnostic) const -> bool
	{
		FCollisionShape Shape;
		FTransform Transform;
		const bool bValid = CollisionSourceMode != EBodySetupCollisionSourceMode::None
			? (CachedSimpleCollision.IsValid() || CachedComplexCollision.IsValid())
			: BuildShape(Shape, Transform);
		if (OutDiagnostic) *OutDiagnostic = bValid ? std::string{}
			: CollisionSourceMode != EBodySetupCollisionSourceMode::None
				? "Body setup has no published collision geometry."
				: "Body setup has no valid finite simple collision geometry.";
		return bValid;
	}
}
