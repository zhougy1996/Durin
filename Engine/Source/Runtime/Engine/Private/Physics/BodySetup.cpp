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

	auto DBodySetup::PostLoad() -> void
	{
		Super::PostLoad();
		InvalidateSimpleGeometry();
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
		CachedAggregate = {};
		CachedAggregateRevision = 0;
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
		return {std::move(Data), CollisionSourceMode, bPersistDerivedData};
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
		FSimpleCollisionElement Element;
		Element.Type = EBodySetupShapeType::Box;
		Element.Dimensions = HalfExtent;
		Element.Transform.Translation = InCenter;
		return SetSimpleCollision({Element});
	}

	auto DBodySetup::SetSphere(double Radius, const FVector3& InCenter) -> bool
	{
		FSimpleCollisionElement Element;
		Element.Type = EBodySetupShapeType::Sphere;
		Element.Dimensions = FVector3(Radius);
		Element.Transform.Translation = InCenter;
		return SetSimpleCollision({Element});
	}

	auto DBodySetup::SetCapsule(double Radius, double HalfHeight, const FVector3& InCenter) -> bool
	{
		FSimpleCollisionElement Element;
		Element.Type = EBodySetupShapeType::Capsule;
		Element.Dimensions = FVector3(Radius, Radius, HalfHeight);
		Element.Transform.Translation = InCenter;
		return SetSimpleCollision({Element});
	}

	auto DBodySetup::SetSimpleCollision(std::vector<FSimpleCollisionElement> Elements) -> bool
	{
		const auto Candidate = Elements.empty() ? FCollisionGeometryRef{} : BuildAggregate(Elements);
		if (!Elements.empty() && !Candidate) return false;
		if (std::ranges::equal(SimpleCollision, Elements, [](const auto& Before, const auto& After) {
			return Before.Type == After.Type && Before.Dimensions == After.Dimensions
				&& Before.Transform.Translation == After.Transform.Translation
				&& Before.Transform.Rotation == After.Transform.Rotation && Before.Transform.Scale3D == After.Transform.Scale3D
				&& Before.Vertices == After.Vertices && Before.Indices == After.Indices;
		})) return true;
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

	auto DBodySetup::PopulateCollisionGeometry(FPhysicsBodyDesc& Desc, const FTransform& WorldTransform) const -> bool
	{
		Desc.SimpleGeometry = {};
		Desc.ComplexGeometry = {};
		Desc.TracePolicy = static_cast<EPhysicsCollisionTracePolicy>(CollisionTraceMode);
		Desc.SimpleTransform = WorldTransform;
		Desc.ComplexTransform = WorldTransform;
		if (CollisionQueryPolicy != EBodySetupCollisionQueryPolicy::ComplexOnly)
			BuildSimpleGeometry(Desc.SimpleGeometry);
		if (CollisionQueryPolicy != EBodySetupCollisionQueryPolicy::SimpleOnly) BuildComplexGeometry(Desc.ComplexGeometry);
		return bool(Desc.SimpleGeometry) || bool(Desc.ComplexGeometry);
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
		FCollisionGeometryRef Simple;
		const bool bValid = !SimpleCollision.empty() ? BuildSimpleGeometry(Simple)
			: CollisionSourceMode != EBodySetupCollisionSourceMode::None
				? (CachedSimpleCollision.IsValid() || CachedComplexCollision.IsValid())
				: BuildSimpleGeometry(Simple);
		if (OutDiagnostic) *OutDiagnostic = bValid ? std::string{}
			: CollisionSourceMode != EBodySetupCollisionSourceMode::None
				? "Body setup has no published collision geometry."
				: "Body setup has no valid finite simple collision geometry.";
		return bValid;
	}
}
