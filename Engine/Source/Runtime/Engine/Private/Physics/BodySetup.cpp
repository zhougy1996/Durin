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
		auto BuildAggregate(std::span<const FSimpleCollisionElement> Elements) -> FCollisionGeometryRef
		{
			if (Elements.empty() || Elements.size() > 64) return {};
			std::vector<FCollisionGeometryChild> Children;
			for (const auto& Element : Elements)
			{
				FCollisionGeometryChild Child;
				Child.LocalTransform = Element.Transform;
				if (!Math::IsFinite(Element.Dimensions) || !IsValidPhysicsTransform(Child.LocalTransform)) return {};
				switch (Element.Type)
				{
				case EBodySetupShapeType::Box: Child.Shape = FCollisionShape::MakeBox(Element.Dimensions); break;
				case EBodySetupShapeType::Sphere: Child.Shape = FCollisionShape::MakeSphere(Element.Dimensions.x); break;
				case EBodySetupShapeType::Capsule: Child.Shape = FCollisionShape::MakeCapsule(Element.Dimensions.x, Element.Dimensions.z); break;
				case EBodySetupShapeType::Convex:
					if (Element.Vertices.size() > 65536 || Element.Indices.size() > 393216) return {};
					Child.Geometry = Element.Indices.empty() ? FCollisionGeometryRef::BuildConvexHull(Element.Vertices)
						: FCollisionGeometryRef::MakeConvexHull(Element.Vertices, Element.Indices);
					if (!Child.Geometry) return {};
					break;
				default: return {};
				}
				Children.push_back(std::move(Child));
			}
			return FCollisionGeometryRef::MakeCompound(Children);
		}
	}
	DBodySetup::DBodySetup(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	FScopedBodySetupUpdate::FScopedBodySetupUpdate(DBodySetup& InBody) : Body(&InBody) { InBody.BeginUpdate(); }
	FScopedBodySetupUpdate::~FScopedBodySetupUpdate()
	{
		if (auto* Owner = Cast<DBodySetup>(ResolveObjectKey(Body))) Owner->EndUpdate();
	}

	auto DBodySetup::BeginUpdate() -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		++UpdateDepth;
	}

	auto DBodySetup::EndUpdate() -> void
	{
		require(UpdateDepth > 0);
		if (--UpdateDepth == 0) FlushChanges();
	}

	auto DBodySetup::MarkChanged(EBodySetupChange Changes) -> void
	{
		PendingChanges |= Changes;
		if (UpdateDepth == 0) FlushChanges();
	}

	auto DBodySetup::FlushChanges() -> void
	{
		const auto Changes = std::exchange(PendingChanges, EBodySetupChange::None);
		if (Changes == EBodySetupChange::None) return;
		++Revision;
		if (auto* Mesh = Cast<DStaticMesh>(GetOuter()); Mesh && Mesh->GetBodySetup() == this)
			Mesh->HandleBodySetupChanged(Changes);
	}

	auto DBodySetup::InvalidateSimpleGeometry() -> void
	{
		++SimpleGeometryRevision;
		CachedGeometry = {};
		CachedAggregate = {};
		CachedGeometryRevision = CachedAggregateRevision = 0;
	}

	auto DBodySetup::InvalidatePhysicsData() -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		// A source edit may invalidate before the owner schedules the replacement.
		// Reuse that invalidation instead of cancelling and notifying a second time.
		if (bPhysicsDataInvalidated) return;
		bPhysicsDataInvalidated = true;
		++PhysicsMeshRequestGeneration;
		CancelPhysicsMeshCompilation(*this);
		CachedSimpleCollision = {};
		CachedComplexCollision = {};
		PhysicsMeshError = {};
		PhysicsMeshStatus = CollisionSourceMode == EBodySetupCollisionSourceMode::None
			? EPhysicsMeshBuildStatus::Ready : EPhysicsMeshBuildStatus::Unavailable;
		MarkChanged(EBodySetupChange::GeneratedGeometry);
	}

	auto DBodySetup::CreatePhysicsMeshesAsync(const IInterface_CollisionDataProvider& Provider,
		bool bPersistDerivedData, FOnAsyncPhysicsCookFinished Completion) -> void
	{
		InvalidatePhysicsData();
		if (CollisionSourceMode == EBodySetupCollisionSourceMode::None)
		{
			if (Completion) Completion({.Status = EPhysicsCookCompletionStatus::Succeeded});
			return;
		}
		bPhysicsDataInvalidated = false;
		auto Source = Provider.CreatePhysicsMeshInputTask();
		if (!Source)
		{
			FailPhysicsMeshes(PhysicsMeshRequestGeneration, std::move(Source.error()));
			const FPhysicsCookCompletionResult Result{.Status = EPhysicsCookCompletionStatus::Failed, .Error = PhysicsMeshError};
			if (Completion) Completion(Result);
			return;
		}
		PhysicsMeshStatus = EPhysicsMeshBuildStatus::Pending;
		SubmitPhysicsMeshCompilation(*this, std::move(*Source), bPersistDerivedData, std::move(Completion));
	}

	auto DBodySetup::CreatePhysicsMeshes(const IInterface_CollisionDataProvider& Provider,
		bool bPersistDerivedData) -> void
	{
		CreatePhysicsMeshesAsync(Provider, bPersistDerivedData);
		FinishPhysicsMeshes();
	}

	auto DBodySetup::GetCookInfo(FTriMeshCollisionData Data, bool bPersistDerivedData) const -> FCookBodySetupInfo
	{
		return {std::move(Data), CollisionSourceMode, CollisionQueryPolicy, bPersistDerivedData};
	}

	auto DBodySetup::IsCompiling() const -> bool { return IsPhysicsMeshCompiling(*this); }
	auto DBodySetup::IsCompilationMutationBlocked() const -> bool
	{
		const auto* Mesh = Cast<DStaticMesh>(GetOuter());
		return bCompilationMutationBlocked || (Mesh && Mesh->IsCompilationMutationBlocked());
	}
	auto DBodySetup::AbortPhysicsMeshAsyncCreation() -> void { CancelPhysicsMeshCompilation(*this); }
	auto DBodySetup::FinishPhysicsMeshes() -> void { FinishPhysicsMeshCompilation(*this); }

	auto DBodySetup::ApplyPhysicsMeshes(uint64 Generation, const FCollisionGeometryRef& Simple,
		const FCollisionGeometryRef& Complex) -> EPhysicsCookCompletionStatus
	{
		if (Generation != PhysicsMeshRequestGeneration || PhysicsMeshStatus != EPhysicsMeshBuildStatus::Pending)
			return EPhysicsCookCompletionStatus::Superseded;
		if (PublishPhysicsMeshes(Simple, Complex, false)) return EPhysicsCookCompletionStatus::Succeeded;
		FailPhysicsMeshes(Generation, MakeAssetBuildDiagnostic("Could not install physics meshes.", "Installation"));
		return EPhysicsCookCompletionStatus::Failed;
	}

	auto DBodySetup::FailPhysicsMeshes(uint64 Generation, std::string Error) -> void
	{
		if (Generation != PhysicsMeshRequestGeneration) return;
		PhysicsMeshStatus = EPhysicsMeshBuildStatus::Failed;
		PhysicsMeshError = MakeAssetBuildDiagnostic(std::move(Error));
	}

	auto DBodySetup::CancelPhysicsMeshes(uint64 Generation) -> void
	{
		if (Generation == PhysicsMeshRequestGeneration && PhysicsMeshStatus == EPhysicsMeshBuildStatus::Pending)
		{
			++PhysicsMeshRequestGeneration;
			PhysicsMeshStatus = EPhysicsMeshBuildStatus::Unavailable;
			bPhysicsDataInvalidated = true;
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
		if (SimpleCollision.empty() && ShapeType == EBodySetupShapeType::Box && Dimensions == HalfExtent && Center == InCenter) return true;
		SimpleCollision.clear();
		ShapeType = EBodySetupShapeType::Box;
		Dimensions = HalfExtent;
		Center = InCenter;
		InvalidateSimpleGeometry();
		MarkChanged(EBodySetupChange::SimpleGeometry);
		MarkPackageDirty();
		return true;
	}

	auto DBodySetup::SetSphere(double Radius, const FVector3& InCenter) -> bool
	{
		const FCollisionShape Candidate = FCollisionShape::MakeSphere(Radius);
		if (!Candidate.IsValid() || !Math::IsFinite(InCenter)) return false;
		if (SimpleCollision.empty() && ShapeType == EBodySetupShapeType::Sphere && Dimensions == FVector3(Radius) && Center == InCenter) return true;
		SimpleCollision.clear();
		ShapeType = EBodySetupShapeType::Sphere;
		Dimensions = FVector3(Radius);
		Center = InCenter;
		InvalidateSimpleGeometry();
		MarkChanged(EBodySetupChange::SimpleGeometry);
		MarkPackageDirty();
		return true;
	}

	auto DBodySetup::SetCapsule(double Radius, double HalfHeight, const FVector3& InCenter) -> bool
	{
		const FCollisionShape Candidate = FCollisionShape::MakeCapsule(Radius, HalfHeight);
		if (!Candidate.IsValid() || !Math::IsFinite(InCenter)) return false;
		if (SimpleCollision.empty() && ShapeType == EBodySetupShapeType::Capsule && Dimensions == FVector3(Radius, Radius, HalfHeight) && Center == InCenter) return true;
		SimpleCollision.clear();
		ShapeType = EBodySetupShapeType::Capsule;
		Dimensions = FVector3(Radius, Radius, HalfHeight);
		Center = InCenter;
		InvalidateSimpleGeometry();
		MarkChanged(EBodySetupChange::SimpleGeometry);
		MarkPackageDirty();
		return true;
	}

	auto DBodySetup::SetSimpleCollision(std::vector<FSimpleCollisionElement> Elements) -> bool
	{
		if (Elements.empty() && SimpleCollision.empty()) return true;
		const auto Candidate = Elements.empty() ? FCollisionGeometryRef{} : BuildAggregate(Elements);
		if (!Elements.empty() && !Candidate) return false;
		// Keep the validated topology so package loading does not run hull discovery again.
		for (uint32 Index = 0; Index < Elements.size(); ++Index)
		{
			auto& Element = Elements[Index];
			Element.Transform = Candidate.GetChild(Index)->LocalTransform;
			if (Element.Type != EBodySetupShapeType::Convex) continue;
			const auto& Hull = Candidate.GetChild(Index)->Geometry;
			Element.Vertices.clear(); Element.Indices.clear();
			for (uint32 Vertex = 0; Vertex < Hull.GetVertexCount(); ++Vertex) Element.Vertices.push_back(*Hull.GetVertex(Vertex));
			for (uint32 Triangle = 0; Triangle < Hull.GetTriangleCount(); ++Triangle)
			{
				const auto* Face = Hull.GetTriangle(Triangle);
				Element.Indices.insert(Element.Indices.end(), {Face->First, Face->Second, Face->Third});
			}
		}
		SimpleCollision = std::move(Elements);
		InvalidateSimpleGeometry();
		CachedAggregate = Candidate;
		CachedAggregateRevision = SimpleGeometryRevision;
		MarkChanged(EBodySetupChange::SimpleGeometry);
		MarkPackageDirty();
		return true;
	}

	auto DBodySetup::SetCollisionTraceMode(EBodySetupCollisionTraceMode Mode) -> void
	{
		require(Mode == EBodySetupCollisionTraceMode::SimpleAndComplex
			|| Mode == EBodySetupCollisionTraceMode::SimpleAsComplex || Mode == EBodySetupCollisionTraceMode::ComplexAsSimple);
		if (CollisionTraceMode == Mode) return;
		CollisionTraceMode = Mode;
		MarkChanged(EBodySetupChange::QuerySettings);
		MarkPackageDirty();
	}

	auto DBodySetup::ConfigureQueryGeometry(FPhysicsBodyDesc& Desc, const FTransform& WorldTransform) const -> void
	{
		Desc.SimpleGeometry = {};
		Desc.ComplexGeometry = {};
		Desc.bHasQueryGeometry = true;
		Desc.TracePolicy = static_cast<EPhysicsCollisionTracePolicy>(CollisionTraceMode);
		Desc.SimpleTransform = WorldTransform;
		Desc.ComplexTransform = WorldTransform;
		if (CollisionQueryPolicy != EBodySetupCollisionQueryPolicy::ComplexOnly)
		{
			BuildSimpleGeometry(Desc.SimpleGeometry);
			if (SimpleCollision.empty() && !CachedSimpleCollision && Desc.SimpleGeometry)
			{
				FTransform Local;
				FCollisionShape Shape;
				if (BuildShape(Shape, Local)) Desc.SimpleTransform = FTransform::Combine(WorldTransform, Local);
			}
		}
		if (CollisionQueryPolicy != EBodySetupCollisionQueryPolicy::SimpleOnly) BuildComplexGeometry(Desc.ComplexGeometry);
	}

	auto DBodySetup::BuildShape(FCollisionShape& OutShape, FTransform& OutLocalTransform) const -> bool
	{
		if (!SimpleCollision.empty()) return false;
		switch (ShapeType)
		{
		case EBodySetupShapeType::Box: OutShape = FCollisionShape::MakeBox(Dimensions); break;
		case EBodySetupShapeType::Sphere: OutShape = FCollisionShape::MakeSphere(Dimensions.x); break;
		case EBodySetupShapeType::Capsule: OutShape = FCollisionShape::MakeCapsule(Dimensions.x, Dimensions.z); break;
		case EBodySetupShapeType::None:
		case EBodySetupShapeType::Convex: return false;
		}
		if (!OutShape.IsValid() || !Math::IsFinite(Center)) return false;
		OutLocalTransform = FTransform();
		OutLocalTransform.Translation = Center;
		return true;
	}

	auto DBodySetup::BuildGeometry(
		FCollisionGeometryRef& OutGeometry, FTransform& OutLocalTransform) const -> bool
	{
		if (CollisionTraceMode == EBodySetupCollisionTraceMode::ComplexAsSimple)
		{
			OutLocalTransform = FTransform();
			return CollisionQueryPolicy != EBodySetupCollisionQueryPolicy::SimpleOnly && BuildComplexGeometry(OutGeometry);
		}
		if (CollisionTraceMode == EBodySetupCollisionTraceMode::SimpleAsComplex)
		{
			OutLocalTransform = FTransform();
			if (SimpleCollision.empty() && !CachedSimpleCollision) OutLocalTransform.Translation = Center;
			return CollisionQueryPolicy != EBodySetupCollisionQueryPolicy::ComplexOnly && BuildSimpleGeometry(OutGeometry);
		}

		if (CollisionSourceMode != EBodySetupCollisionSourceMode::None || !SimpleCollision.empty())
		{
			OutLocalTransform = FTransform();
			if (CollisionQueryPolicy != EBodySetupCollisionQueryPolicy::ComplexOnly
				&& (CachedSimpleCollision || !SimpleCollision.empty() || CollisionSourceMode == EBodySetupCollisionSourceMode::None)
				&& BuildSimpleGeometry(OutGeometry)) return true;
			if (CollisionQueryPolicy != EBodySetupCollisionQueryPolicy::SimpleOnly
				&& BuildComplexGeometry(OutGeometry)) return true;
			return false;
		}
		FCollisionShape Shape;
		if (!BuildShape(Shape, OutLocalTransform)) return false;
		if (!CachedGeometry.IsValid() || CachedGeometryRevision != SimpleGeometryRevision)
		{
			CachedGeometry = FCollisionGeometryRef::MakePrimitive(Shape);
			CachedGeometryRevision = CachedGeometry.IsValid() ? SimpleGeometryRevision : 0;
		}
		OutGeometry = CachedGeometry;
		return OutGeometry.IsValid();
	}

	auto DBodySetup::BuildSimpleGeometry(FCollisionGeometryRef& OutGeometry) const -> bool
	{
		if (!SimpleCollision.empty())
		{
			if (!CachedAggregate || CachedAggregateRevision != SimpleGeometryRevision)
			{
				CachedAggregate = BuildAggregate(SimpleCollision);
				CachedAggregateRevision = SimpleGeometryRevision;
			}
			OutGeometry = CachedAggregate;
			return bool(OutGeometry);
		}
		OutGeometry = CachedSimpleCollision;
		if (OutGeometry) return true;
		FCollisionShape Shape;
		FTransform Local;
		if (!BuildShape(Shape, Local)) return false;
		if (!CachedGeometry || CachedGeometryRevision != SimpleGeometryRevision)
		{
			CachedGeometry = FCollisionGeometryRef::MakePrimitive(Shape);
			CachedGeometryRevision = SimpleGeometryRevision;
		}
		OutGeometry = CachedGeometry;
		return bool(OutGeometry);
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
		const FScopedBodySetupUpdate Update(*this);
		CollisionSourceMode = Mode;
		InvalidatePhysicsData();
		PhysicsMeshStatus = Mode == EBodySetupCollisionSourceMode::None
			? EPhysicsMeshBuildStatus::Ready : EPhysicsMeshBuildStatus::Unavailable;
		MarkChanged(EBodySetupChange::SourceRecipe);
		MarkPackageDirty();
	}

	auto DBodySetup::SetCollisionQueryPolicy(EBodySetupCollisionQueryPolicy Policy) -> void
	{
		require(Policy == EBodySetupCollisionQueryPolicy::SimpleOnly
			|| Policy == EBodySetupCollisionQueryPolicy::ComplexOnly
			|| Policy == EBodySetupCollisionQueryPolicy::SimpleAndComplex);
		if (CollisionQueryPolicy == Policy) return;
		CollisionQueryPolicy = Policy;
		MarkChanged(EBodySetupChange::QuerySettings);
		MarkPackageDirty();
	}

	auto DBodySetup::InstallPhysicsMeshes(
		const FCollisionGeometryRef& Simple,
		const FCollisionGeometryRef& Complex) -> bool
	{
		return PublishPhysicsMeshes(Simple, Complex, true);
	}

	auto DBodySetup::PublishPhysicsMeshes(const FCollisionGeometryRef& Simple,
		const FCollisionGeometryRef& Complex, bool bSupersedeRequest) -> bool
	{
		if (CollisionSourceMode == EBodySetupCollisionSourceMode::None
			|| (CollisionSourceMode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0
				&& (!Simple || Simple.GetKind() != ECollisionGeometryKind::ConvexHull))
			|| (CollisionSourceMode == EBodySetupCollisionSourceMode::TriangleMeshFromLOD0
				&& (!Complex || Complex.GetKind() != ECollisionGeometryKind::TriangleMesh))
			|| (Simple && Simple.GetKind() != ECollisionGeometryKind::ConvexHull)
			|| (Complex && Complex.GetKind() != ECollisionGeometryKind::TriangleMesh)) return false;
		if (bSupersedeRequest)
		{
			++PhysicsMeshRequestGeneration;
			CancelPhysicsMeshCompilation(*this);
		}
		CachedSimpleCollision = Simple;
		CachedComplexCollision = Complex;
#if DURIN_WITH_EDITORONLY_DATA
		++CollisionBuildRevision;
#endif
		bPhysicsDataInvalidated = false;
		PhysicsMeshStatus = EPhysicsMeshBuildStatus::Ready;
		PhysicsMeshError = {};
		MarkChanged(EBodySetupChange::GeneratedGeometry);
		return true;
	}

	auto DBodySetup::IsValid(std::string* OutDiagnostic) const -> bool
	{
		FCollisionShape Shape;
		FTransform Transform;
		FCollisionGeometryRef Simple;
		const bool bValid = !SimpleCollision.empty() ? BuildSimpleGeometry(Simple)
			: CollisionSourceMode != EBodySetupCollisionSourceMode::None
				? (CachedSimpleCollision.IsValid() || CachedComplexCollision.IsValid())
				: BuildShape(Shape, Transform);
		if (OutDiagnostic) *OutDiagnostic = bValid ? std::string{}
			: CollisionSourceMode != EBodySetupCollisionSourceMode::None
				? "Body setup has no published collision geometry."
				: "Body setup has no valid finite simple collision geometry.";
		return bValid;
	}
}
