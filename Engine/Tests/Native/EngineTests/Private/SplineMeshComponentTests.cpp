#include "NativeAssetBuildTestSupport.h"
#include "StaticMeshTestAccess.h"
#include "Components/PropertyEditValidation.h"
#include "Asset/AssetCompilingManager.h"
#include "Components/SplineMeshComponent.h"
#include "Components/SplineComponent.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "Actors/SplineMeshActor.h"
#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Archive.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/Property.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineTestSupport.h"
#include "Materials/Material.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/MountPathTestSupport.h"
#include "Modules/ModuleManager.h"
#include "Physics/BodySetup.h"
#include "Spline/SplineMeshDeformer.h"
#include "Physics/PhysicsMeshInputTask.h"
#include "NativeTestSupport.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "AssetForge/Builtins/StaticMeshImport.h"
#include "StaticMesh/StaticMeshFactoryTestSupport.h"

#include <gtest/gtest.h>

#include "NativeDObjectTestSupport.h"

namespace
{
	using namespace Durin;

	auto GetSplineCollision(DSplineMeshComponent* Component) -> FCollisionGeometryRef
	{
		const auto* Body = Component->GetBodySetup();
		return Body ? Body->GetResidentComplexGeometry() : FCollisionGeometryRef{};
	}

	auto CreateAuthoredDebugTriangle(DObject* Outer, std::string_view Name)
		-> DStaticMesh*
	{
		auto* Mesh = NewObject<DStaticMesh>(Outer, FName(Name));
		FMeshDescription Imported;
		Imported.PolygonGroups.push_back({
			.Name = "Default", .SourceMaterialIndex = 0, .SourceName = "Default"});
		FMeshDescriptionSection& Section = Imported.Sections.emplace_back();
		Section.Name = "Triangle";
		Section.Positions = {
			FVector3f(-0.5f, -0.5f, 0.0f),
			FVector3f(0.5f, -0.5f, 0.0f),
			FVector3f(0.0f, 0.5f, 0.0f)};
		Section.Indices = {0, 1, 2};
		Section.SourceMaterialIndex = 0;
		if (const auto Built = Durin::FStaticMeshTestAccess::Build(Mesh, std::move(Imported)); !Built)
		{
			ADD_FAILURE() << Built.error();
			return nullptr;
		}
		return Mesh;
	}

	auto MakeComponentWithMesh() -> std::pair<DSplineMeshComponent*, DStaticMesh*>
	{
		InitializeDObjectSystem();
		if (!FAssetCompilingManager::Get().IsAcceptingRequests())
			EXPECT_TRUE(InitializeAssetCompilingManager());
		auto* Component = NewObject<DSplineMeshComponent>(nullptr, "SplineMeshComponent");
		auto* Mesh = DStaticMesh::CreateDebugTriangle();
		EXPECT_NE(Component, nullptr);
		EXPECT_NE(Mesh, nullptr);
		Component->SetStaticMesh(Mesh);
		return {Component, Mesh};
	}

	auto BuildCollisionSourceTriangle(DStaticMesh* Mesh, float Minimum, float Size, float Height = 0) -> void
	{
		FMeshDescription Geometry;
		Geometry.PolygonGroups.push_back({.Name = "Default", .SourceMaterialIndex = 0, .SourceName = "Default"});
		auto& Section = Geometry.Sections.emplace_back();
		Section.Name = "Proxy";
		Section.Positions = {{Minimum, Minimum, Height}, {Minimum + Size, Minimum, Height},
			{Minimum, Minimum + Size, Height}};
		Section.Indices = {0, 1, 2};
		Section.SourceMaterialIndex = 0;
		const auto Built = FStaticMeshTestAccess::Build(Mesh, std::move(Geometry));
		ASSERT_TRUE(Built) << Built.error();
	}

	auto BuildCollisionLODs(DStaticMesh* Mesh, bool bTetrahedron = false, float Size = 4) -> void
	{
		FMeshDescription Primary;
		Primary.PolygonGroups.push_back({"Default", 0, "Default"});
		auto& Section = Primary.Sections.emplace_back();
		Section.Name = "Primary";
		Section.Positions = {{10, 10, 0}, {10 + Size, 10, 0}, {10, 10 + Size, 0}, {10 + Size, 10 + Size, 0}};
		Section.Indices = {0, 1, 2, 1, 3, 2};
		if (bTetrahedron)
		{
			Section.Positions.back() = {10, 10, Size};
			Section.Indices = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
		}
		auto Lower = Primary;
		Lower.Sections.front().Positions = {{11, 11, 0.5f}, {12, 11, 0.5f}, {11, 12, 0.5f}};
		Lower.Sections.front().Indices = {0, 1, 2};
		FStaticMeshSource Source;
		ASSERT_TRUE(Source.InitializeLODs({std::move(Primary), std::move(Lower)}));
		const auto Built = Mesh->BuildFromSource(EStaticMeshBuildMode::Synchronous,
			{.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(Source)});
		ASSERT_TRUE(Built) << Built.error();
	}
}

TEST(FSplineMeshCollisionTests, CollisionLODUsesPrimaryCoordinatesAndClampsWithoutChangingRenderSelection)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	BuildCollisionLODs(Mesh);
	EXPECT_EQ(Mesh->GetLODForCollision(), 0);
	EXPECT_EQ(Component->GetBodySetup(), nullptr);
	EXPECT_EQ(Mesh->GetBodySetup(), nullptr);
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh);
	ASSERT_TRUE(GetSplineCollision(Component));
	EXPECT_EQ(GetSplineCollision(Component).GetTriangleCount(), 2u);
	const auto State = Component->GetDerivedState();
	const auto RenderRevision = Mesh->GetRenderResourceStatus().Revision;
	Mesh->SetLODForCollision(1);
	const auto Input = Mesh->GetPhysicsTriMeshData();
	ASSERT_TRUE(Input);
	EXPECT_EQ(Input->Positions.front(), FVector3f(-0.375f, -0.375f, 0.1875f));
	EXPECT_EQ(GetSplineCollision(Component).GetTriangleCount(), 1u);
	EXPECT_EQ(Component->GetDerivedState(), State);
	EXPECT_EQ(Mesh->GetRenderResourceStatus().Revision, RenderRevision);
	EXPECT_DOUBLE_EQ(State->Params.SourceForwardMin, -0.75);
	EXPECT_DOUBLE_EQ(State->Params.SourceForwardMax, 0.75);
	Mesh->SetLODForCollision(-10);
	EXPECT_EQ(GetSplineCollision(Component).GetTriangleCount(), 2u);
	Mesh->SetLODForCollision(99);
	EXPECT_EQ(GetSplineCollision(Component).GetTriangleCount(), 1u);
	EXPECT_EQ(Component->GetDerivedState(), State);
}

TEST(FSplineMeshCollisionTests, CollisionLODDefersBodyReplacementAndRejectsPriorCookGenerations)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	BuildCollisionLODs(Mesh);
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh);
	const auto Previous = GetSplineCollision(Component);
	const auto Generation = Component->GetBodySetup()->GetPhysicsMeshRequestGeneration();
	auto Input = Component->CreatePhysicsMeshInputTask();
	ASSERT_TRUE(Input);
	auto Params = Component->GetSplineMeshParams();
	Params.EndPosition.z += 2;
	Component->SetSplineMeshParams(Params, false);
	Mesh->SetLODForCollision(1);
	EXPECT_EQ(GetSplineCollision(Component).GetIdentity(), Previous.GetIdentity());
	EXPECT_FALSE(Component->ContainsPhysicsTriMeshData());
	Component->UpdateMesh();
	EXPECT_EQ(GetSplineCollision(Component).GetTriangleCount(), 1u);
	EXPECT_EQ(Component->GetBodySetup()->ApplyPhysicsMeshes(Generation, {}, Previous), EPhysicsCookCompletionStatus::Superseded);
	const auto Captured = (*Input)->Execute({.MaximumWorkingSetBytes = (*Input)->GetWorkingSetBytes()});
	ASSERT_TRUE(Captured);
	EXPECT_EQ(Captured->Indices.size(), 6u);
	EXPECT_EQ(Component->GetPhysicsTriMeshData()->Indices.size(), 3u);
}

TEST(FSplineMeshCollisionTests, ProxyDelegatesLODChoiceAndKeepsPrimaryNormalizationInOneHop)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	BuildCollisionLODs(Mesh);
	auto* Proxy = NewObject<DStaticMesh>(nullptr, "MultiLODCollisionProxy");
	BuildCollisionLODs(Proxy, false, 8);
	Mesh->SetLODForCollision(1);
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh);
	ASSERT_TRUE(Mesh->SetComplexCollisionMesh(Proxy));
	EXPECT_EQ(GetSplineCollision(Component).GetTriangleCount(), 2u);
	const auto State = Component->GetDerivedState();
	Proxy->SetLODForCollision(1);
	EXPECT_EQ(GetSplineCollision(Component).GetTriangleCount(), 1u);
	EXPECT_EQ(Mesh->GetPhysicsTriMeshData()->Positions.front(), FVector3f(-0.375f, -0.375f, 0.1875f));
	EXPECT_NE(Mesh->GetPhysicsTriMeshData()->Positions, Proxy->GetPhysicsTriMeshData()->Positions);
	EXPECT_EQ(Component->GetDerivedState(), State);
	ASSERT_TRUE(Proxy->SetComplexCollisionMesh(NewObject<DStaticMesh>(nullptr, "IgnoredNestedLODProxy")));
	EXPECT_EQ(GetSplineCollision(Component).GetTriangleCount(), 1u);
	EXPECT_EQ(Component->GetDerivedState(), State);
}

TEST(FSplineMeshCollisionTests, ConvexCookingRemainsOnLOD0WhenComplexCollisionUsesLowerLOD)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	BuildCollisionLODs(Mesh, true);
	Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::ConvexHullFromLOD0);
	Mesh->GetBodySetup()->FinishPhysicsMeshes();
	ASSERT_TRUE(Mesh->GetBodySetup()->GetResidentSimpleGeometry());
	const auto Hull = Mesh->GetBodySetup()->GetResidentSimpleGeometry();
	Mesh->SetLODForCollision(1);
	Mesh->GetBodySetup()->FinishPhysicsMeshes();
	EXPECT_EQ(Mesh->GetBodySetup()->GetResidentSimpleGeometry().GetIdentity(), Hull.GetIdentity());
	Mesh->RebuildCollision();
	Mesh->GetBodySetup()->FinishPhysicsMeshes();
	EXPECT_EQ(Mesh->GetBodySetup()->GetResidentSimpleGeometry().GetVertexCount(), 4u);
	Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	Mesh->GetBodySetup()->FinishPhysicsMeshes();
	EXPECT_EQ(Mesh->GetBodySetup()->GetResidentComplexGeometry().GetTriangleCount(), 1u);
}

TEST(FSplineMeshCollisionTests, ComplexProxyUsesPrimarySourceCoordinatesAndPreservesDefaultAndSimpleCollision)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	BuildCollisionSourceTriangle(Mesh, 10, 4);
	auto* Proxy = NewObject<DStaticMesh>(nullptr, "OffsetCollisionProxy");
	BuildCollisionSourceTriangle(Proxy, 11, 1);
	const auto Original = Mesh->GetPhysicsTriMeshData();
	ASSERT_TRUE(Original);
	Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::None);
	Mesh->SetCollisionQueryPolicy(EBodySetupCollisionQueryPolicy::SimpleOnly);
	ASSERT_TRUE(Mesh->GetBodySetup()->SetBox({1, 2, 3}, {4, 5, 6}));
	const auto RenderRevision = Mesh->GetRenderResourceStatus().Revision;
	const auto SourceRevision = Mesh->GetCollisionSourceRevision();
	ASSERT_TRUE(Mesh->SetComplexCollisionMesh(Proxy));
	EXPECT_GT(Mesh->GetCollisionSourceRevision(), SourceRevision);
	EXPECT_EQ(Mesh->GetRenderResourceStatus().Revision, RenderRevision);
	EXPECT_EQ(Component->GetSplineMeshCollisionMode(), ESplineMeshCollisionMode::Disabled);
	EXPECT_EQ(Component->GetBodySetup(), nullptr);
	FPhysicsBodyDesc Desc;
	ASSERT_TRUE(Mesh->GetBodySetup()->PopulateCollisionGeometry(Desc, FTransform{}));
	ASSERT_TRUE(Desc.SimpleGeometry);
	const auto* Child = Desc.SimpleGeometry.GetChild(0);
	ASSERT_NE(Child, nullptr);
	EXPECT_EQ(Child->LocalTransform.Translation, FVector3(4, 5, 6));
	const auto Data = Mesh->GetPhysicsTriMeshData();
	ASSERT_TRUE(Data);
	ASSERT_EQ(Data->Positions.size(), 3u);
	EXPECT_EQ(Data->Positions[0], FVector3f(-0.375f, -0.375f, 0));
	EXPECT_EQ(Data->Positions[1], FVector3f(0, -0.375f, 0));
	EXPECT_EQ(Data->Positions[2], FVector3f(-0.375f, 0, 0));
	EXPECT_NE(Data->Positions, Proxy->GetPhysicsTriMeshData()->Positions);
	EXPECT_FALSE(Mesh->SetComplexCollisionMesh(Mesh));
	EXPECT_EQ(Mesh->GetComplexCollisionMesh(), Proxy);
	ASSERT_TRUE(Mesh->SetComplexCollisionMesh(nullptr));
	EXPECT_EQ(Mesh->GetPhysicsTriMeshData()->Positions, Original->Positions);
}

TEST(FSplineMeshCollisionTests, ProxyChangesRebuildCollisionWithoutChangingRenderAndRespectDeferredEdits)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	BuildCollisionSourceTriangle(Mesh, 10, 4);
	auto* Proxy = NewObject<DStaticMesh>(nullptr, "MutableCollisionProxy");
	BuildCollisionSourceTriangle(Proxy, 11, 1);
	ASSERT_TRUE(Mesh->SetComplexCollisionMesh(Proxy));
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh);
	ASSERT_TRUE(GetSplineCollision(Component));
	const auto RenderState = Component->GetDerivedState();
	Mesh->RebuildCollision();
	EXPECT_EQ(Component->GetDerivedState(), RenderState);
	ASSERT_TRUE(GetSplineCollision(Component));
	const auto FirstGeometry = GetSplineCollision(Component);
	const auto Generation = Component->GetBodySetup()->GetPhysicsMeshRequestGeneration();
	auto Detached = Component->CreatePhysicsMeshInputTask();
	ASSERT_TRUE(Detached);
	BuildCollisionSourceTriangle(Proxy, 11, 1, 0.5f);
	EXPECT_EQ(Component->GetDerivedState(), RenderState);
	EXPECT_NE(GetSplineCollision(Component).GetIdentity(), FirstGeometry.GetIdentity());
	EXPECT_EQ(Component->GetBodySetup()->ApplyPhysicsMeshes(Generation, {}, FirstGeometry),
		EPhysicsCookCompletionStatus::Superseded);
	const auto BeforeDeferred = GetSplineCollision(Component);
	auto Params = Component->GetSplineMeshParams();
	Params.EndPosition.y += 2;
	Component->SetSplineMeshParams(Params, false);
	BuildCollisionSourceTriangle(Proxy, 11, 1, 1.0f);
	EXPECT_EQ(GetSplineCollision(Component).GetIdentity(), BeforeDeferred.GetIdentity());
	EXPECT_FALSE(Component->ContainsPhysicsTriMeshData());
	EXPECT_TRUE(Component->IsMeshDirty());
	Component->UpdateMesh();
	EXPECT_NE(GetSplineCollision(Component).GetIdentity(), BeforeDeferred.GetIdentity());
	EXPECT_TRUE(Component->GetCollisionBuildError().empty());
	const auto OldData = (*Detached)->Execute({.MaximumWorkingSetBytes = (*Detached)->GetWorkingSetBytes()});
	ASSERT_TRUE(OldData);
	EXPECT_NE(OldData->Positions, Component->GetPhysicsTriMeshData()->Positions);
}

TEST(FSplineMeshCollisionTests, ProxySelectionIsOneHopAndInvalidProxyNeverFallsBackToVisualMesh)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	auto* Proxy = DStaticMesh::CreateDebugTriangle();
	auto* Empty = NewObject<DStaticMesh>(nullptr, "EmptyCollisionProxy");
	const auto ProxyOwnData = Proxy->GetPhysicsTriMeshData();
	ASSERT_TRUE(ProxyOwnData);
	ASSERT_TRUE(Proxy->SetComplexCollisionMesh(Empty));
	ASSERT_TRUE(Mesh->SetComplexCollisionMesh(Proxy));
	EXPECT_EQ(Mesh->GetPhysicsTriMeshData()->Positions, ProxyOwnData->Positions);
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh);
	ASSERT_TRUE(GetSplineCollision(Component));
	ASSERT_TRUE(Mesh->SetComplexCollisionMesh(Empty));
	EXPECT_FALSE(Mesh->ContainsPhysicsTriMeshData());
	EXPECT_FALSE(Mesh->GetPhysicsTriMeshData());
	EXPECT_FALSE(GetSplineCollision(Component));
	EXPECT_FALSE(Component->GetCollisionBuildError().empty());
	ASSERT_TRUE(Mesh->SetComplexCollisionMesh(nullptr));
	EXPECT_TRUE(GetSplineCollision(Component));
	EXPECT_TRUE(Component->GetCollisionBuildError().empty());
}

TEST(FSplineMeshCollisionTests, ComplexProxyNeverFeedsConvexCookingAndTriangleBodiesTrackProxyChanges)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	FMeshDescription Geometry;
	Geometry.PolygonGroups.push_back({.Name = "Default", .SourceMaterialIndex = 0, .SourceName = "Default"});
	auto& Section = Geometry.Sections.emplace_back();
	Section.Name = "Tetrahedron";
	Section.Positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	Section.Indices = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
	Section.SourceMaterialIndex = 0;
	ASSERT_TRUE(FStaticMeshTestAccess::Build(Mesh, std::move(Geometry)));
	auto* Proxy = DStaticMesh::CreateDebugTriangle();
	Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::ConvexHullFromLOD0);
	Mesh->GetBodySetup()->FinishPhysicsMeshes();
	const auto OriginalHull = Mesh->GetBodySetup()->GetResidentSimpleGeometry();
	ASSERT_TRUE(OriginalHull);
	ASSERT_TRUE(Mesh->SetComplexCollisionMesh(Proxy));
	Mesh->GetBodySetup()->FinishPhysicsMeshes();
	const auto Hull = Mesh->GetBodySetup()->GetResidentSimpleGeometry();
	ASSERT_TRUE(Hull);
	ASSERT_EQ(Hull.GetVertexCount(), 4u);
	for (uint32 Index = 0; Index < 4; ++Index)
		EXPECT_EQ(*Hull.GetVertex(Index), *OriginalHull.GetVertex(Index));
	Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	Mesh->GetBodySetup()->FinishPhysicsMeshes();
	ASSERT_TRUE(Mesh->GetBodySetup()->GetResidentComplexGeometry());
	EXPECT_EQ(Mesh->GetBodySetup()->GetResidentComplexGeometry().GetTriangleCount(), 1u);
	const auto OldGeneration = Mesh->GetBodySetup()->GetPhysicsMeshRequestGeneration();
	const auto OldGeometry = Mesh->GetBodySetup()->GetResidentComplexGeometry();
	BuildCollisionSourceTriangle(Proxy, 0, 0.5f, 0.25f);
	Mesh->GetBodySetup()->FinishPhysicsMeshes();
	EXPECT_NE(Mesh->GetBodySetup()->GetResidentComplexGeometry().GetIdentity(), OldGeometry.GetIdentity());
	EXPECT_EQ(Mesh->GetBodySetup()->ApplyPhysicsMeshes(OldGeneration, {}, OldGeometry),
		EPhysicsCookCompletionStatus::Superseded);
}

TEST(FSplineMeshCollisionTests, PrimaryNormalizationChangesRemapProxyAndMirroredGeometrySupportsQueries)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	BuildCollisionSourceTriangle(Mesh, 10, 4);
	auto* Proxy = NewObject<DStaticMesh>(nullptr, "RenormalizedProxy");
	BuildCollisionSourceTriangle(Proxy, 11, 1);
	ASSERT_TRUE(Mesh->SetComplexCollisionMesh(Proxy));
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh);
	BuildCollisionSourceTriangle(Mesh, 10, 8);
	const auto Input = Mesh->GetPhysicsTriMeshData();
	ASSERT_TRUE(Input);
	EXPECT_EQ(Input->Positions.front(), FVector3f(-0.5625f, -0.5625f, 0));
	auto Params = Component->GetSplineMeshParams();
	Params.StartScale = {-1, 1};
	Params.EndScale = {-1, 1};
	Params.EndPosition = {2, 0.5, 0.25};
	Component->SetSplineMeshParams(Params);
	const auto Deformed = Component->GetPhysicsTriMeshData();
	ASSERT_TRUE(Deformed);
	FPhysicsBodyDesc CollisionDesc; auto& Collision = CollisionDesc.ComplexGeometry; auto& Transform = CollisionDesc.ComplexTransform;
	ASSERT_TRUE(Component->BuildPhysicsBodyDesc(CollisionDesc));
	const FVector3 A(Deformed->Positions[0]), B(Deformed->Positions[1]), C(Deformed->Positions[2]);
	const auto Center = (A + B + C) / 3.0;
	const auto Normal = Math::NormalizeOr(Math::Cross(B - A, C - A), FVectorConstants::Up);
	FPhysicsQueryHit Hit;
	EXPECT_EQ(CollisionGeometry::Raycast(Center + Normal, Center - Normal, Collision, Transform,
		CollisionGeometry::ECollisionQueryAlgorithm::Production, Hit), CollisionGeometry::ECollisionQueryStatus::Hit);
	FTransform Query;
	Query.Translation = Center;
	const auto Sphere = FCollisionShape::MakeSphere(0.01);
	EXPECT_EQ(CollisionGeometry::Overlap(Sphere, Query, Collision, Transform,
		CollisionGeometry::ECollisionQueryAlgorithm::Production, Hit), CollisionGeometry::ECollisionQueryStatus::Hit);
	Query.Translation += Normal;
	EXPECT_EQ(CollisionGeometry::Sweep(Sphere, Query, Normal * -2.0, Collision, Transform,
		CollisionGeometry::ECollisionQueryAlgorithm::Production, Hit), CollisionGeometry::ECollisionQueryStatus::Hit);
}

TEST(FSplineMeshComponentTests, DefaultObjectPublishesDiagnosticStateAndReflectsAuthoredFields)
{
	InitializeDObjectSystem();
	auto* Component = NewObject<DSplineMeshComponent>(nullptr, "DefaultSplineMeshComponent");
	ASSERT_NE(Component, nullptr);
	const auto State = Component->GetDerivedState();
	ASSERT_NE(State, nullptr);
	EXPECT_EQ(State->Status, ESplineMeshDerivedStateStatus::NoStaticMesh);
	EXPECT_FALSE(Component->GetDerivedState()->IsValid());
	EXPECT_NE(Component->GetClass()->FindPropertyByName(FName("StaticMesh")), nullptr);
	EXPECT_NE(Component->GetClass()->FindPropertyByName(FName("SplineMeshParams")), nullptr);
	EXPECT_NE(Component->GetClass()->FindPropertyByName(FName("OverrideMaterials")), nullptr);
}

TEST(FSplineMeshComponentTests, PlanarStaticMeshPublishesZeroThicknessLocalBounds)
{
	InitializeDObjectSystem();
	DStaticMesh* Mesh = DStaticMesh::CreateDebugTriangle();
	ASSERT_NE(Mesh, nullptr);
	const std::optional<FBox> Bounds = Mesh->GetLOD0LocalBounds();
	ASSERT_TRUE(Bounds.has_value());
	EXPECT_DOUBLE_EQ(Bounds->Min.z, 0.0);
	EXPECT_DOUBLE_EQ(Bounds->Max.z, 0.0);
	EXPECT_FALSE(Mesh->GetLOD0VolumetricBounds().has_value());
}

TEST(FSplineMeshComponentTests, BuiltInSplineBoxProvidesLongitudinalDeformationSections)
{
	InitializeDObjectSystem();
	if (!Durin::FAssetCompilingManager::Get().IsAcceptingRequests())
		ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
	Testing::FScopedMountRegistryFixture MountRegistry;
	FMountPaths::InitDefaultMountPoints();
	ASSERT_TRUE(RefreshAssetRegistry());
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/Engine/Models/SplineBox", Path));
	DStaticMesh* Mesh = nullptr;
	const auto LoadResult = LoadObject<DStaticMesh>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path));
	Mesh = LoadResult.value_or(nullptr);
	ASSERT_TRUE(LoadResult) << (LoadResult ? std::string{} : LoadResult.error().Message);
	ASSERT_NE(Mesh, nullptr);
	Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Mesh);
	const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	ASSERT_NE(RenderData, nullptr);
	ASSERT_FALSE(RenderData->LODResources.empty());
	const auto Positions = RenderData->LODResources[0]
		.VertexBuffers.PositionVertexBuffer.GetPositions();
	std::set<float> LongitudinalSections;
	for (const FVector3f& Position : Positions) LongitudinalSections.insert(Position.x);
	EXPECT_EQ(LongitudinalSections.size(), 17u);
	EXPECT_FLOAT_EQ(*LongitudinalSections.begin(), -0.75f);
	EXPECT_FLOAT_EQ(*LongitudinalSections.rbegin(), 0.75f);
	EXPECT_TRUE(UnloadPackage(
		Path, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FSplineMeshComponentTests, RenderUpdatesDoNotBuildCollisionOrRetainProviderGeometry)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	const auto Initial = Component->GetDerivedState();
	ASSERT_TRUE(Initial && Initial->IsValid());
	EXPECT_EQ(Component->GetBodySetup(), nullptr);
	const auto Data = Component->GetPhysicsTriMeshData();
	ASSERT_TRUE(Data);
	EXPECT_EQ(Data->Positions.size(), 3u);
	EXPECT_EQ(Component->GetDerivedState(), Initial);
	EXPECT_EQ(Component->GetBodySetup(), nullptr);

	auto Params = Component->GetSplineMeshParams();
	Params.EndPosition.y += 20.0;
	Component->SetSplineMeshParams(Params);
	const auto Updated = Component->GetDerivedState();
	ASSERT_TRUE(Updated && Updated->IsValid());
	EXPECT_NE(Updated, Initial);
	const auto UpdatedData = Component->GetPhysicsTriMeshData();
	ASSERT_TRUE(UpdatedData);
	EXPECT_NE(UpdatedData->Positions, Data->Positions);
	EXPECT_EQ(Component->GetBodySetup(), nullptr);
	Component->SetSplineMeshParams(Params);
	EXPECT_EQ(Component->GetDerivedState(), Updated);
	Component->SetStaticMesh(nullptr);
	EXPECT_FALSE(Component->ContainsPhysicsTriMeshData());
}

TEST(FSplineMeshComponentTests, DeferredEditsPublishOnceAndCleanUpdatesDoNothing)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	const auto Initial = Component->GetDerivedState();
	auto Params = Component->GetSplineMeshParams();
	Params.EndPosition.y += 5.0;
	Component->SetSplineMeshParams(Params, false);
	Params.EndPosition.y += 10.0;
	Component->SetSplineMeshParams(Params, false);
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh, false);
	EXPECT_TRUE(Component->IsMeshDirty());
	EXPECT_EQ(Component->GetDerivedState(), Initial);
	EXPECT_EQ(Component->GetSplineMeshParams(), Params);
	Component->UpdateMesh();
	const auto Updated = Component->GetDerivedState();
	ASSERT_TRUE(Updated && GetSplineCollision(Component).IsValid());
	EXPECT_NE(Updated, Initial);
	EXPECT_EQ(Updated->Params, Params);
	EXPECT_FALSE(Component->IsMeshDirty());
	Component->UpdateMesh();
	EXPECT_EQ(Component->GetDerivedState(), Updated);
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::Disabled, false);
	FPhysicsBodyDesc CollisionDesc;
	EXPECT_TRUE(Component->BuildPhysicsBodyDesc(CollisionDesc));
	Component->UpdateMesh();
	EXPECT_FALSE(Component->BuildPhysicsBodyDesc(CollisionDesc));
	EXPECT_EQ(Component->GetDerivedState(), Updated);
}

TEST(FSplineMeshComponentTests, DeferredSourceAndParamsUseOnlyTheFinalMesh)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	const auto Initial = Component->GetDerivedState();
	auto* Replacement = DStaticMesh::CreateDebugTriangle();
	Component->SetStaticMesh(Replacement, false);
	EXPECT_EQ(Component->GetDerivedState(), Initial);
	EXPECT_FALSE(Component->GetPhysicsTriMeshData());
	auto Params = Component->GetSplineMeshParams();
	Params.EndPosition.z += 3.0;
	Component->SetSplineMeshParams(Params, false);
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh, false);
	Component->UpdateMesh();
	EXPECT_NE(Component->GetDerivedState(), Initial);
	EXPECT_TRUE(GetSplineCollision(Component).IsValid());
	EXPECT_EQ(Component->GetStaticMesh(), Replacement);
	Params.EndPosition.z += 2.0;
	Component->SetSplineMeshParams(Params, false);
	Component->SetStaticMesh(Replacement);
	EXPECT_FALSE(Component->IsMeshDirty());
	EXPECT_EQ(Component->GetDerivedState()->Params, Params);
}

TEST(FSplineMeshComponentTests, FailedDeferredUpdateInvalidatesSnapshotAndCanBeCorrected)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	const auto Initial = Component->GetDerivedState();
	auto Params = Component->GetSplineMeshParams();
	Params.StartOffset = FVector2(std::numeric_limits<double>::max());
	Params.EndOffset = Params.StartOffset;
	Component->SetSplineMeshParams(Params, false);
	Component->UpdateMesh();
	EXPECT_FALSE(Component->GetMeshUpdateError().empty());
	EXPECT_FALSE(Component->IsMeshDirty());
	EXPECT_NE(Component->GetDerivedState(), Initial);
	EXPECT_FALSE(Component->GetDerivedState()->IsValid());
	Component->SetSplineMeshParams(Initial->Params, false);
	Component->UpdateMesh();
	EXPECT_TRUE(Component->GetMeshUpdateError().empty());
	EXPECT_FALSE(Component->IsMeshDirty());
}

TEST(FSplineMeshComponentTests, PublishesNormalizedExactLOD0AndConservativeBounds)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	const auto Initial = Component->GetDerivedState();
	const auto InitialData = Component->GetPhysicsTriMeshData();
	ASSERT_TRUE(InitialData);
	ASSERT_TRUE(Initial && Initial->IsValid()) << (Initial ? Initial->Diagnostic : "missing state");
	EXPECT_EQ(InitialData->Positions.size(), 3u);
	EXPECT_EQ(InitialData->Indices, (std::vector<uint32>{0, 1, 2}));
	EXPECT_NEAR(Initial->Params.SourceForwardMin, -0.65, 1.e-6);
	EXPECT_NEAR(Initial->Params.SourceForwardMax, 0.65, 1.e-6);
	for (const FVector3f& Position : InitialData->Positions)
	{
		EXPECT_GE(Position.x, Initial->ConservativeLocalBounds.Min.x - 1.e-5);
		EXPECT_LE(Position.x, Initial->ConservativeLocalBounds.Max.x + 1.e-5);
		EXPECT_GE(Position.y, Initial->ConservativeLocalBounds.Min.y - 1.e-5);
		EXPECT_LE(Position.y, Initial->ConservativeLocalBounds.Max.y + 1.e-5);
		EXPECT_GE(Position.z, Initial->ConservativeLocalBounds.Min.z - 1.e-5);
		EXPECT_LE(Position.z, Initial->ConservativeLocalBounds.Max.z + 1.e-5);
	}

	FSplineMeshParams Params = Component->GetSplineMeshParams();
	Params.EndPosition = {0.0, 120.0, 20.0};
	Params.EndTangent = {0.0, 100.0, 0.0};
	Component->SetSplineMeshParams(Params, false);
	Component->UpdateMesh();
	ASSERT_TRUE(Component->GetMeshUpdateError().empty());
	const auto Curved = Component->GetDerivedState();
	ASSERT_TRUE(Curved && Curved->IsValid());
	EXPECT_NE(Curved, Initial);
	EXPECT_EQ(Initial->Params.EndPosition, FVector3(5.0, 0.0, 0.0));
}

TEST(FSplineMeshComponentTests, InvalidUpdateDropsPublishedStateAndKeepsAuthoredEdits)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	const FSplineMeshParams BeforeParams = Component->GetSplineMeshParams();
	const auto BeforeState = Component->GetDerivedState();
	FSplineMeshParams Invalid = BeforeParams;
	Invalid.StartPosition.x = std::numeric_limits<double>::quiet_NaN();
	Component->SetSplineMeshParams(Invalid);
	EXPECT_TRUE(std::isnan(Component->GetSplineMeshParams().StartPosition.x));
	EXPECT_FALSE(Component->IsMeshDirty());
	Component->UpdateMesh();
	EXPECT_FALSE(Component->GetMeshUpdateError().empty());
	EXPECT_TRUE(std::isnan(Component->GetSplineMeshParams().StartPosition.x));
	EXPECT_NE(Component->GetDerivedState(), BeforeState);
	EXPECT_FALSE(Component->GetDerivedState()->IsValid());
}

TEST(FSplineMeshComponentTests, ExactGeometryFailureKeepsRenderStateAndCanRetry)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	const auto OriginalParams = Component->GetSplineMeshParams();
	auto Params = OriginalParams;
	Params.StartOffset = FVector2(1.e40);
	Params.EndOffset = Params.StartOffset;
	Component->SetSplineMeshParams(Params);
	ASSERT_TRUE(Component->GetDerivedState()->IsValid());
	EXPECT_FALSE(Component->GetPhysicsTriMeshData());
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh);
	EXPECT_TRUE(Component->GetDerivedState()->IsValid());
	EXPECT_FALSE(Component->GetCollisionBuildError().empty());
	EXPECT_FALSE(GetSplineCollision(Component).IsValid());
	Component->SetSplineMeshParams(OriginalParams);
	EXPECT_TRUE(Component->GetCollisionBuildError().empty());
	EXPECT_TRUE(GetSplineCollision(Component).IsValid());
}

TEST(FSplineMeshComponentTests, SourcePublicationRecoversUnavailableState)
{
	InitializeDObjectSystem();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests())
		ASSERT_TRUE(InitializeAssetCompilingManager());
	auto* World = NewObject<DWorld>(nullptr, "SplineSourceRecoveryWorld");
	ASSERT_TRUE(World->InitializeSubsystems());
	auto* Level = NewObject<DLevel>(World, "SplineSourceRecoveryLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	auto* Actor = Level->SpawnActor<AActor>("SplineSourceRecoveryActor");
	auto* Component = Cast<DSplineMeshComponent>(Actor->AddInstanceComponent(
		DSplineMeshComponent::StaticClass(), "SplineMesh"));
	ASSERT_TRUE(Component->IsRegistered());
	auto* Pending = NewObject<DStaticMesh>(nullptr, "PendingSplineSource");
	Component->SetStaticMesh(Pending);
	EXPECT_FALSE(Component->IsMeshDirty());
	EXPECT_EQ(Component->GetDerivedState()->Status, ESplineMeshDerivedStateStatus::SourceDataUnavailable);
	EXPECT_TRUE(Component->GetMeshUpdateError().empty());
	FMeshDescription Imported;
	Imported.PolygonGroups.push_back({.Name = "Default", .SourceMaterialIndex = 0, .SourceName = "Default"});
	auto& Section = Imported.Sections.emplace_back();
	Section.Positions = {FVector3f(-1, -1, 0), FVector3f(1, -1, 0), FVector3f(0, 1, 0)};
	Section.Indices = {0, 1, 2};
	Section.SourceMaterialIndex = 0;
	std::string Error;
	const auto SynchronousBuild1 = Durin::FStaticMeshTestAccess::Build(Pending, std::move(Imported));
	ASSERT_TRUE(SynchronousBuild1) << SynchronousBuild1.error();
	EXPECT_TRUE(Component->GetDerivedState()->IsValid());
	EXPECT_FALSE(Component->IsMeshDirty());
	MarkObjectHierarchyAsGarbage(World);
	CollectGarbage();
}

TEST(FSplineMeshComponentTests, ReflectedInvalidDraftIsRejectedBeforeMutation)
{
	InitializeDObjectSystem();
	auto [Component, Mesh] = MakeComponentWithMesh();
	auto* Draft = NewObject<DSplineMeshComponent>(nullptr, "SplineMeshInvalidDraft");
	DClass* Class = DSplineMeshComponent::StaticClass();
	FProperty* ParamsProperty = Class->FindPropertyByName(FName("SplineMeshParams"));
	ASSERT_NE(ParamsProperty, nullptr);
	auto* DraftParams = ParamsProperty->ContainerPtrToValuePtr<FSplineMeshParams>(Draft);
	*DraftParams = Component->GetSplineMeshParams();
	DraftParams->EndRollRadians = std::numeric_limits<double>::infinity();
	FPropertyEditProposal Proposal{
		.MemberProperty = ParamsProperty,
		.LeafProperty = ParamsProperty,
		.DraftRootProperty = ParamsProperty,
		.DraftRootContainer = Draft,
		.DraftLeafContainer = Draft};
	const auto Validation = Component->PreEditChangeProperty(Proposal);
	EXPECT_EQ(Validation.error().Code, EObjectValidationError::PropertyRejected);
	EXPECT_EQ(Validation.error().PropertyName, "SplineMeshParams");
	const auto Cause = std::dynamic_pointer_cast<const FEnginePropertyEditCause>(Validation.error().Cause);
	ASSERT_TRUE(Cause);
	const auto* SplineError = std::get_if<FSplineMeshValidationError>(&Cause->Error);
	ASSERT_NE(SplineError, nullptr);
	EXPECT_EQ(SplineError->Code, ESplineMeshValidationError::NonFiniteParameters);
	DraftParams->EndRollRadians = 0;
	EXPECT_TRUE(std::isinf(SplineError->Params.EndRollRadians));
	EXPECT_TRUE(Component->GetMeshUpdateError().empty());
	EXPECT_TRUE(Math::IsFinite(Component->GetSplineMeshParams().EndPosition));
	EXPECT_TRUE(std::isfinite(Component->GetSplineMeshParams().EndRollRadians));
}

TEST(FSplineMeshComponentTests, DuplicateRebuildsIndependentEquivalentSnapshot)
{
	auto [Source, Mesh] = MakeComponentWithMesh();
	FSplineMeshParams Params = Source->GetSplineMeshParams();
	Params.EndPosition = {75.0, 25.0, 10.0};
	Source->SetSplineMeshParams(Params);
	Source->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh);
	auto* Duplicate = Cast<DSplineMeshComponent>(
		DuplicateObject(Source, nullptr, "SplineMeshDuplicate").value());
	ASSERT_NE(Duplicate, nullptr);
	EXPECT_EQ(Duplicate->GetStaticMesh(), Mesh);
	EXPECT_EQ(Duplicate->GetSplineMeshParams(), Source->GetSplineMeshParams());
	const auto SourceState = Source->GetDerivedState();
	const auto DuplicateState = Duplicate->GetDerivedState();
	ASSERT_TRUE(SourceState && DuplicateState && DuplicateState->IsValid());
	EXPECT_NE(SourceState, DuplicateState);
	EXPECT_EQ(DuplicateState->Params, SourceState->Params);
	const auto SourceData = Source->GetPhysicsTriMeshData();
	const auto DuplicateData = Duplicate->GetPhysicsTriMeshData();
	ASSERT_TRUE(SourceData && DuplicateData);
	EXPECT_EQ(DuplicateData->Positions, SourceData->Positions);
	EXPECT_EQ(DuplicateData->Indices, SourceData->Indices);
	ASSERT_NE(Source->GetBodySetup(), nullptr);
	ASSERT_NE(Duplicate->GetBodySetup(), nullptr);
	EXPECT_NE(Source->GetBodySetup(), Duplicate->GetBodySetup());
	EXPECT_EQ(Duplicate->GetBodySetup()->GetOuter(), Duplicate);
	EXPECT_TRUE(GetSplineCollision(Duplicate));
}

TEST(FSplineMeshComponentTests, MaterialOverridesUseStaticMeshSlotRules)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	auto* Material = NewObject<DMaterial>(nullptr, "SplineMeshMaterial");
	ASSERT_NE(Material, nullptr);
	EXPECT_EQ(Component->GetNumMaterials(), 1u);
	EXPECT_TRUE(Component->SetMaterial(0, Material));
	EXPECT_EQ(Component->GetMaterial(), Material);
	EXPECT_FALSE(Component->SetMaterial(1, Material));
	EXPECT_TRUE(Component->ResetMaterial(0));
	EXPECT_EQ(Component->GetMaterial(), nullptr);
	EXPECT_TRUE(Component->GetOverrideMaterials().empty());

	// All slot operations must work through the geometry-independent base API.
	DMeshComponent* Base = Component;
	Mesh->SetMaterialSlotDefaultMaterial(0, Material);
	EXPECT_EQ(Base->GetDefaultMaterial(0), Material);
	EXPECT_EQ(Base->GetMaterial(), Material);
	ASSERT_EQ(Base->GetMaterialIndex(FName("Default")), std::optional<uint32>(0));
	EXPECT_FALSE(Base->SetMaterialByName(FName("Missing"), Material));
	ASSERT_TRUE(Base->SetMaterialByName(FName("Default"), Material));
	EXPECT_TRUE(Base->HasMaterialOverride(0));
	EXPECT_EQ(Base->GetMaterialByName(FName("Default")), Material);
	auto* Duplicate = Cast<DSplineMeshComponent>(DuplicateObject(Component, nullptr, "MaterialOverrideDuplicate").value());
	ASSERT_NE(Duplicate, nullptr);
	EXPECT_EQ(Duplicate->GetMaterialOverride(0), Material);
	Component->SetStaticMesh(nullptr);
	EXPECT_EQ(Base->GetNumMaterials(), 0u);
	EXPECT_EQ(Base->GetMaterial(), nullptr);
	EXPECT_EQ(Base->GetMaterialOverride(0), Material);
	EXPECT_FALSE(Base->SetMaterial(Material));
	Component->SetStaticMesh(Mesh);
	EXPECT_EQ(Base->GetMaterial(), Material);
	EXPECT_TRUE(Base->ClearMaterialOverrides());
	EXPECT_FALSE(Base->ClearMaterialOverrides());
	EXPECT_EQ(Base->GetMaterial(), Material);
	EXPECT_TRUE(Duplicate->HasMaterialOverride(0));
}

TEST(FSplineMeshCollisionTests, UsesExactDerivedTriangleMeshAndRevisionsEveryInputMutation)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh);
	const auto FirstState = Component->GetDerivedState();
	ASSERT_TRUE(FirstState && FirstState->IsValid());
	const auto FirstGeometry = GetSplineCollision(Component);
	const auto FirstBodyRevision = Component->GetBodySetup()->GetRevision();
	const auto FirstData = Component->GetPhysicsTriMeshData();
	ASSERT_TRUE(FirstData);
	ASSERT_TRUE(FirstGeometry.IsValid());
	EXPECT_EQ(FirstGeometry.GetKind(), ECollisionGeometryKind::TriangleMesh);
	EXPECT_EQ(FirstGeometry.GetTriangleCount(), 1u);
	FPhysicsBodyDesc CollisionDesc; auto& Geometry = CollisionDesc.ComplexGeometry; auto& Transform = CollisionDesc.ComplexTransform;
	ASSERT_TRUE(Component->BuildPhysicsBodyDesc(CollisionDesc));
	EXPECT_EQ(Geometry.GetIdentity(), FirstGeometry.GetIdentity());

	const FVector3 A(FirstData->Positions[0]);
	const FVector3 B(FirstData->Positions[1]);
	const FVector3 C(FirstData->Positions[2]);
	const FVector3 Center = (A + B + C) / 3.0;
	const FVector3 Normal = Math::NormalizeOr(Math::Cross(B - A, C - A), FVectorConstants::Up);
	FPhysicsQueryHit Hit;
	EXPECT_EQ(CollisionGeometry::Raycast(Center + Normal * 10.0, Center - Normal * 10.0,
		Geometry, Transform, CollisionGeometry::ECollisionQueryAlgorithm::Production, Hit),
		CollisionGeometry::ECollisionQueryStatus::Hit);
	const FCollisionShape QuerySphere = FCollisionShape::MakeSphere(0.05);
	FTransform OverlapTransform;
	OverlapTransform.Translation = Center;
	EXPECT_EQ(CollisionGeometry::Overlap(QuerySphere, OverlapTransform, Geometry, Transform,
		CollisionGeometry::ECollisionQueryAlgorithm::Production, Hit),
		CollisionGeometry::ECollisionQueryStatus::Hit);
	FTransform SweepTransform;
	SweepTransform.Translation = Center + Normal;
	EXPECT_EQ(CollisionGeometry::Sweep(QuerySphere, SweepTransform, Normal * -2.0, Geometry, Transform,
		CollisionGeometry::ECollisionQueryAlgorithm::Production, Hit),
		CollisionGeometry::ECollisionQueryStatus::Hit);

	FSplineMeshParams Params = Component->GetSplineMeshParams();
	Params.EndPosition = {2.0, 1.0, 0.5};
	Params.EndTangent = {1.0, 1.0, 0.0};
	Component->SetSplineMeshParams(Params);
	const auto CurvedState = Component->GetDerivedState();
	ASSERT_TRUE(CurvedState && GetSplineCollision(Component).IsValid());
	EXPECT_GT(Component->GetBodySetup()->GetRevision(), FirstBodyRevision);
	EXPECT_NE(GetSplineCollision(Component).GetIdentity(), Geometry.GetIdentity());

	Params.StartScale = {-1.0, 1.0};
	Params.EndScale = {-1.0, 1.0};
	Component->SetSplineMeshParams(Params);
	EXPECT_TRUE(Component->BuildPhysicsBodyDesc(CollisionDesc));
	Params.StartScale = {0.0, 0.0};
	Params.EndScale = {0.0, 0.0};
	Params.EndPosition = Params.StartPosition;
	Params.StartTangent = FVector3(0.0);
	Params.EndTangent = FVector3(0.0);
	Component->SetSplineMeshParams(Params);
	EXPECT_TRUE(Component->GetDerivedState()->IsValid());
	EXPECT_FALSE(Component->BuildPhysicsBodyDesc(CollisionDesc));
	EXPECT_FALSE(Component->GetCollisionBuildError().empty());
	EXPECT_TRUE(Component->GetMeshUpdateError().empty());
	EXPECT_FALSE(Component->IsMeshDirty());
	Component->UpdateMesh();
	EXPECT_TRUE(Component->GetDerivedState()->IsValid());
	EXPECT_FALSE(Component->GetCollisionBuildError().empty());
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::Disabled);
	EXPECT_TRUE(Component->GetCollisionBuildError().empty());
	EXPECT_FALSE(Component->BuildPhysicsBodyDesc(CollisionDesc));
}

TEST(FSplineMeshCollisionTests, ComponentsOwnTransientBodiesAndIgnoreSourceCollisionPolicy)
{
	auto [First, Mesh] = MakeComponentWithMesh();
	Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::None);
	Mesh->SetCollisionQueryPolicy(EBodySetupCollisionQueryPolicy::SimpleOnly);
	auto* SourceBody = Mesh->GetBodySetup();
	ASSERT_NE(SourceBody, nullptr);
	const uint64 SourceRevision = SourceBody->GetRevision();
	auto* Second = NewObject<DSplineMeshComponent>(nullptr, "SecondSplineMesh");
	Second->SetStaticMesh(Mesh);
	EXPECT_EQ(First->GetBodySetup(), nullptr);
	EXPECT_EQ(Second->GetBodySetup(), nullptr);
	First->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh);
	Second->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh);
	auto* FirstBody = First->GetBodySetup();
	auto* SecondBody = Second->GetBodySetup();
	ASSERT_NE(FirstBody, nullptr);
	ASSERT_NE(SecondBody, nullptr);
	EXPECT_NE(FirstBody, SecondBody);
	EXPECT_NE(FirstBody, SourceBody);
	EXPECT_EQ(FirstBody->GetOuter(), First);
	EXPECT_EQ(SecondBody->GetOuter(), Second);
	EXPECT_TRUE(FirstBody->HasAnyObjectFlags(EObjectFlags::Transient));
	EXPECT_TRUE(GetSplineCollision(First));
	const auto SecondGeometry = GetSplineCollision(Second);
	ASSERT_TRUE(SecondGeometry);
	const uint64 SecondRevision = SecondBody->GetRevision();
	const uint64 FirstRevision = FirstBody->GetRevision();
	First->UpdateMesh();
	EXPECT_EQ(FirstBody->GetRevision(), FirstRevision);
	auto Params = First->GetSplineMeshParams();
	Params.EndPosition.y += 10.0;
	First->SetSplineMeshParams(Params);
	EXPECT_NE(GetSplineCollision(First).GetIdentity(), SecondGeometry.GetIdentity());
	EXPECT_EQ(GetSplineCollision(Second).GetIdentity(), SecondGeometry.GetIdentity());
	EXPECT_EQ(SecondBody->GetRevision(), SecondRevision);
	EXPECT_EQ(SourceBody->GetRevision(), SourceRevision);
	EXPECT_EQ(SourceBody->GetCollisionSourceMode(), EBodySetupCollisionSourceMode::None);
	EXPECT_EQ(SourceBody->GetCollisionQueryPolicy(), EBodySetupCollisionQueryPolicy::SimpleOnly);
	First->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::Disabled);
	EXPECT_FALSE(GetSplineCollision(First));
	EXPECT_TRUE(GetSplineCollision(Second));
}

TEST(FSplineMeshCollisionTests, DetachedInputSurvivesDeferredEditsAndOwnerDestruction)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	const auto Before = Component->GetPhysicsTriMeshData();
	ASSERT_TRUE(Before);
	auto Input = Component->CreatePhysicsMeshInputTask();
	ASSERT_TRUE(Input);
	auto Params = Component->GetSplineMeshParams();
	Params.EndPosition.z += 15.0;
	Component->SetSplineMeshParams(Params, false);
	const auto Deferred = Component->GetPhysicsTriMeshData();
	ASSERT_TRUE(Deferred);
	EXPECT_EQ(Deferred->Positions, Before->Positions);
	Component->UpdateMesh();
	const auto Updated = Component->GetPhysicsTriMeshData();
	ASSERT_TRUE(Updated);
	EXPECT_NE(Updated->Positions, Before->Positions);
	Component->SetStaticMesh(DStaticMesh::CreateDebugTriangle(), false);
	EXPECT_FALSE(Component->ContainsPhysicsTriMeshData());
	EXPECT_FALSE(Component->GetPhysicsTriMeshData());
	MarkObjectHierarchyAsGarbage(Component);
	MarkAsGarbage(Mesh);
	CollectGarbage();
	std::optional<std::expected<FTriMeshCollisionData, std::string>> Result;
	std::jthread Worker([&] { Result = (*Input)->Execute({}); });
	Worker.join();
	ASSERT_TRUE(Result && *Result);
	EXPECT_EQ((*Result)->Positions, Before->Positions);
	EXPECT_EQ((*Result)->Indices, Before->Indices);
}

TEST(FSplineMeshCollisionTests, AppliedDeformationRejectsPriorBodyGeneration)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh);
	auto* Body = Component->GetBodySetup();
	ASSERT_NE(Body, nullptr);
	const uint64 OldGeneration = Body->GetPhysicsMeshRequestGeneration();
	const auto OldGeometry = GetSplineCollision(Component);
	ASSERT_TRUE(OldGeometry);
	auto Params = Component->GetSplineMeshParams();
	Params.EndPosition.y += 7.0;
	Component->SetSplineMeshParams(Params);
	const auto CurrentGeometry = GetSplineCollision(Component);
	ASSERT_TRUE(CurrentGeometry);
	EXPECT_GT(Body->GetPhysicsMeshRequestGeneration(), OldGeneration);
	EXPECT_EQ(Body->ApplyPhysicsMeshes(OldGeneration, {}, OldGeometry), EPhysicsCookCompletionStatus::Superseded);
	EXPECT_EQ(GetSplineCollision(Component).GetIdentity(), CurrentGeometry.GetIdentity());
	EXPECT_TRUE(Component->GetCollisionBuildError().empty());
}

TEST(FSplineMeshCollisionTests, RegisteredMutationReplacesBodiesWithoutStaleHandles)
{
	InitializeDObjectSystem();
	if (!Durin::FAssetCompilingManager::Get().IsAcceptingRequests())
		ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
	auto* World = NewObject<DWorld>(nullptr, "SplineMeshCollisionWorld");
	EXPECT_TRUE(World->InitializeSubsystems());
	auto* Level = NewObject<DLevel>(World, "SplineMeshCollisionLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	auto* Actor = Level->SpawnActor<AActor>("SplineMeshCollisionActor");
	auto* Component = Cast<DSplineMeshComponent>(Actor->AddInstanceComponent(
		DSplineMeshComponent::StaticClass(), "SplineMesh"));
	ASSERT_NE(Component, nullptr);
	Component->SetStaticMesh(DStaticMesh::CreateDebugTriangle(Level));
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh);
	Component->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	FPhysicsScene& PhysicsScene = World->GetPhysicsScene();
	ASSERT_EQ(PhysicsScene.GetBodyCount(), 1u);
	const FPhysicsActorHandle FirstHandle = Component->GetPhysicsActorHandle();
	ASSERT_TRUE(FirstHandle.IsValid());
	EXPECT_TRUE(PhysicsScene.ContainsBody(FirstHandle));
	const uint64 FirstRevision = Component->GetPublishedBodySetupRevision();
	FTransform NonUniformActorTransform;
	NonUniformActorTransform.Scale3D = {2.0, 0.5, 1.5};
	ASSERT_TRUE(Actor->SetActorTransform(NonUniformActorTransform));
	EXPECT_EQ(PhysicsScene.GetBodyCount(), 1u);
	EXPECT_TRUE(Component->GetPhysicsActorHandle().IsValid());

	FSplineMeshParams Params = Component->GetSplineMeshParams();
	Params.EndPosition = {2.0, 1.0, 0.25};
	Params.EndTangent = {1.0, 1.0, 0.0};
	const auto DeformationBeforeBatch = Component->GetDerivedState();
	Component->SetSplineMeshParams(Params, false);
	Params.EndPosition.z += 0.25;
	Component->SetSplineMeshParams(Params, false);
	EXPECT_EQ(Component->GetPhysicsActorHandle(), FirstHandle);
	EXPECT_EQ(Component->GetPublishedBodySetupRevision(), FirstRevision);
	Component->UpdateMesh();
	EXPECT_NE(Component->GetDerivedState(), DeformationBeforeBatch);
	ASSERT_EQ(PhysicsScene.GetBodyCount(), 1u);
	EXPECT_FALSE(PhysicsScene.ContainsBody(FirstHandle));
	EXPECT_TRUE(Component->GetPhysicsActorHandle().IsValid());
	EXPECT_NE(Component->GetPhysicsActorHandle(), FirstHandle);
	EXPECT_NE(Component->GetPublishedBodySetupRevision(), FirstRevision);

	const auto ValidParams = Component->GetSplineMeshParams();
	Params.StartPosition.x = std::numeric_limits<double>::quiet_NaN();
	Component->SetSplineMeshParams(Params);
	EXPECT_FALSE(Component->IsMeshDirty());
	EXPECT_FALSE(Component->GetDerivedState()->IsValid());
	EXPECT_EQ(PhysicsScene.GetBodyCount(), 0u);
	EXPECT_FALSE(Component->GetPhysicsActorHandle().IsValid());
	Component->SetSplineMeshParams(ValidParams);
	EXPECT_TRUE(Component->GetMeshUpdateError().empty());
	EXPECT_EQ(PhysicsScene.GetBodyCount(), 1u);

	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::Disabled);
	EXPECT_EQ(PhysicsScene.GetBodyCount(), 0u);
	EXPECT_FALSE(Component->GetPhysicsActorHandle().IsValid());
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh);
	EXPECT_EQ(PhysicsScene.GetBodyCount(), 1u);
	Component->SetStaticMesh(nullptr);
	EXPECT_EQ(PhysicsScene.GetBodyCount(), 0u);
	MarkObjectHierarchyAsGarbage(World);
	CollectGarbage();
}

TEST(FSplineMeshComponentTests, LevelPackageRoundTripsAuthoredFieldsAndRebuildsDerivedState)
{
	InitializeDObjectSystem();
	if (!Durin::FAssetCompilingManager::Get().IsAcceptingRequests())
		ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
	const std::filesystem::path Root = Testing::GetTestWorkDirectory() / "SplineMeshComponentAssets";
	static std::unordered_set<std::filesystem::path> InitializedRoots;
	if (InitializedRoots.insert(Root).second)
	{
		Testing::RemoveTestWorkDirectory(Root);
		Testing::RegisterMountPointForTests("/SplineMeshComponentTests/", Root.generic_string() + "/");
	}
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/SplineMeshComponentTests/RoundTrip", Path));
	DLevel* Level = nullptr;
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "Triangle.obj";
	Durin::Testing::TFactoryImportResult<Durin::DStaticMesh> MeshImport = AssetForge::Builtins::ImportStaticMeshForTest(
		Source.generic_string(), "/SplineMeshComponentTests/SourceMesh");
	ASSERT_TRUE(MeshImport) << MeshImport.Message;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Level));
	auto* Actor = Level->SpawnActor<AActor>("SplineMeshActor");
	auto* Component = Cast<DSplineMeshComponent>(Actor->AddInstanceComponent(
		DSplineMeshComponent::StaticClass(), FName("SplineMesh")));
	ASSERT_NE(Component, nullptr);
	Component->SetStaticMesh(MeshImport.Asset);
	FSplineMeshParams Params = Component->GetSplineMeshParams();
	Params.EndPosition = {80.0, 30.0, 15.0};
	Params.EndRollRadians = 0.75;
	Params.EndScale = {2.0, 0.5};
	Component->SetSplineMeshParams(Params);
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh);
	ASSERT_TRUE(SavePackage(Level->GetPackage()));
	ASSERT_TRUE(UnloadPackage(Path));

	DObject* LoadedObject = nullptr;
	{
		auto LoadedValue = LoadObject<DObject>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path));
		LoadedObject = LoadedValue.value_or(nullptr);
		ASSERT_TRUE(LoadedValue);
	}
	auto* LoadedLevel = Cast<DLevel>(LoadedObject);
	ASSERT_NE(LoadedLevel, nullptr);
	auto* Loaded = LoadedLevel->FindActorByName("SplineMeshActor")
		->FindComponentByClass<DSplineMeshComponent>();
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->GetSplineMeshParams().EndPosition, Params.EndPosition);
	EXPECT_DOUBLE_EQ(Loaded->GetSplineMeshParams().EndRollRadians, Params.EndRollRadians);
	EXPECT_EQ(Loaded->GetSplineMeshParams().EndScale, Params.EndScale);
	const auto State = Loaded->GetDerivedState();
	ASSERT_TRUE(State && State->IsValid()) << (State ? State->Diagnostic : "missing state");
	EXPECT_EQ(State->Params, Loaded->GetSplineMeshParams());
	ASSERT_TRUE(Loaded->GetPhysicsTriMeshData());
	EXPECT_EQ(Loaded->GetSplineMeshCollisionMode(), ESplineMeshCollisionMode::DeformedTriangleMesh);
	ASSERT_NE(Loaded->GetBodySetup(), nullptr);
	EXPECT_EQ(Loaded->GetBodySetup()->GetOuter(), Loaded);
	EXPECT_TRUE(Loaded->GetBodySetup()->HasAnyObjectFlags(EObjectFlags::Transient));
	EXPECT_TRUE(GetSplineCollision(Loaded));
	EXPECT_FALSE(Loaded->GetPackage()->IsDirty());
	EXPECT_TRUE(UnloadPackage(Path));
}

TEST(FSplineMeshActorTests, ReconcilesStableGuidSegmentsFromSplineMutations)
{
	InitializeDObjectSystem();
	if (!Durin::FAssetCompilingManager::Get().IsAcceptingRequests())
		ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
	const std::filesystem::path Root = Testing::GetTestWorkDirectory() / "SplineMeshActorAssets";
	static std::unordered_set<std::filesystem::path> InitializedRoots;
	if (InitializedRoots.insert(Root).second)
	{
		Testing::RemoveTestWorkDirectory(Root);
		Testing::RegisterMountPointForTests("/SplineMeshActorTests/", Root.generic_string() + "/");
	}
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/SplineMeshActorTests/Reconciliation", Path));
	DLevel* Level = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Level));
	auto* Actor = Level->SpawnActor<ASplineMeshActor>("SplineMeshActor");
	ASSERT_NE(Actor, nullptr);
	auto* InitialMesh = CreateAuthoredDebugTriangle(Level, "InitialStaticMesh");
	ASSERT_NE(InitialMesh, nullptr);
	Actor->SetPathMesh(InitialMesh);
	FSplinePoint A({0.0, 0.0, 0.0});
	FSplinePoint B({100.0, 0.0, 0.0});
	FSplinePoint C({200.0, 0.0, 0.0});
	Actor->GetSplineComponent()->SetSplinePoints({A, B, C});
	auto Segments = Actor->FindComponentsByClass<DSplineMeshComponent>();
	ASSERT_EQ(Segments.size(), 2u);
	auto* First = Cast<DSplineMeshComponent>(Segments[0]);
	auto* Second = Cast<DSplineMeshComponent>(Segments[1]);
	ASSERT_NE(First, nullptr);
	ASSERT_NE(Second, nullptr);
	EXPECT_EQ(First->GetCreationMethod(), EComponentCreationMethod::Generated);
	EXPECT_TRUE(First->HasAnyObjectFlags(EObjectFlags::Transient));
	EXPECT_EQ(First->GetAttachParent(), Actor->GetSplineComponent());

	B.Position.y = 25.0;
	ASSERT_TRUE(Actor->GetSplineComponent()->UpdateSplinePoint(1, B));
	Segments = Actor->FindComponentsByClass<DSplineMeshComponent>();
	ASSERT_EQ(Segments.size(), 2u);
	EXPECT_EQ(std::ranges::find(Segments, First), Segments.end());
	EXPECT_EQ(std::ranges::find(Segments, Second), Segments.end());
	EXPECT_EQ(Cast<DSplineMeshComponent>(Segments.front())->GetSplineMeshParams().EndPosition, B.Position);

	FSplinePoint Inserted({50.0, 10.0, 0.0});
	ASSERT_TRUE(Actor->GetSplineComponent()->InsertSplinePoint(1, Inserted));
	Segments = Actor->FindComponentsByClass<DSplineMeshComponent>();
	ASSERT_EQ(Segments.size(), 3u);
	EXPECT_EQ(std::ranges::find(Segments, First), Segments.end());
	EXPECT_EQ(std::ranges::find(Segments, Second), Segments.end());
	ASSERT_TRUE(Actor->GetSplineComponent()->RemoveSplinePoint(1));
	EXPECT_EQ(Actor->FindComponentsByClass<DSplineMeshComponent>().size(), 2u);
	Actor->SetPathCollisionEnabled(true);
	Actor->SetPathVisible(false);
	Segments = Actor->FindComponentsByClass<DSplineMeshComponent>();
	for (DActorComponent* Owned : Segments)
	{
		auto* Segment = Cast<DSplineMeshComponent>(Owned);
		ASSERT_NE(Segment, nullptr);
		EXPECT_FALSE(Segment->IsVisible());
		EXPECT_EQ(Segment->GetCollisionEnabled(), ECollisionEnabled::QueryOnly);
		EXPECT_EQ(Segment->GetSplineMeshCollisionMode(), ESplineMeshCollisionMode::DeformedTriangleMesh);
		FPhysicsBodyDesc CollisionDesc;
		EXPECT_TRUE(Segment->BuildPhysicsBodyDesc(CollisionDesc));
	}
	auto* ReplacementMesh = CreateAuthoredDebugTriangle(Level, "ReplacementStaticMesh");
	ASSERT_NE(ReplacementMesh, nullptr);
	Actor->SetPathMesh(ReplacementMesh);
	MarkAsGarbage(InitialMesh);
	CollectGarbage();
	Segments = Actor->FindComponentsByClass<DSplineMeshComponent>();
	EXPECT_EQ(std::ranges::find(Segments, First), Segments.end());
	EXPECT_EQ(std::ranges::find(Segments, Second), Segments.end());
	for (DActorComponent* Owned : Segments)
		EXPECT_EQ(Cast<DSplineMeshComponent>(Owned)->GetStaticMesh(), ReplacementMesh);
	FTransform ActorTransform;
	ActorTransform.Translation = {25.0, 50.0, 10.0};
	ASSERT_TRUE(Actor->SetActorTransform(ActorTransform));
	EXPECT_EQ(Actor->FindComponentsByClass<DSplineMeshComponent>(), Segments);

	Level->GetPackage()->ClearDirty();
	ASSERT_TRUE(Actor->RerunConstructionScripts());
	EXPECT_FALSE(Level->GetPackage()->IsDirty());
	EXPECT_EQ(Actor->FindComponentsByClass<DSplineMeshComponent>().size(), 2u);
	std::unordered_map<DObject*, DObject*> Duplicates;
	auto* Duplicate = Cast<ASplineMeshActor>(DuplicateObject(
		Actor, Level, "SplineMeshActorDuplicate", &Duplicates).value());
	ASSERT_NE(Duplicate, nullptr);
	EXPECT_EQ(Duplicate->GetSplineComponent()->GetSplinePoints(),
		Actor->GetSplineComponent()->GetSplinePoints());
	const auto DuplicateSegments = Duplicate->FindComponentsByClass<DSplineMeshComponent>();
	ASSERT_EQ(DuplicateSegments.size(), 2u);
	for (DActorComponent* Segment : DuplicateSegments)
		EXPECT_EQ(Segment->GetCreationMethod(), EComponentCreationMethod::Generated);
	Level->GetPackage()->MarkDirty();
	const FAssetWriteResult SaveResult = SavePackage(Level->GetPackage());
	EXPECT_TRUE(SaveResult) << SaveResult.Message;
	EXPECT_TRUE(UnloadPackage(Path));
	DObject* LoadedObject = nullptr;
	{
		auto LoadedValue = LoadObject<DObject>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path));
		LoadedObject = LoadedValue.value_or(nullptr);
		ASSERT_TRUE(LoadedValue);
	}
	auto* LoadedLevel = Cast<DLevel>(LoadedObject);
	ASSERT_NE(LoadedLevel, nullptr);
	auto* LoadedActor = Cast<ASplineMeshActor>(LoadedLevel->FindActorByName("SplineMeshActor"));
	ASSERT_NE(LoadedActor, nullptr);
	EXPECT_EQ(LoadedActor->GetSplineComponent()->GetNumSplinePoints(), 3u);
	EXPECT_EQ(LoadedActor->FindComponentsByClass<DSplineMeshComponent>().size(), 2u);
	EXPECT_TRUE(UnloadPackage(Path));
}

TEST(FSplineMeshActorTests, ClosedLoopReorderAndEmptyCurvesPreserveGuidOwnership)
{
	InitializeDObjectSystem();
	if (!Durin::FAssetCompilingManager::Get().IsAcceptingRequests())
		ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
	auto* World = NewObject<DWorld>(nullptr, "SplineMeshTopologyWorld");
	EXPECT_TRUE(World->InitializeSubsystems());
	auto* Level = NewObject<DLevel>(World, "SplineMeshTopologyLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	auto* Actor = Level->SpawnActor<ASplineMeshActor>("SplineMeshActor");
	ASSERT_NE(Actor, nullptr);
	Actor->SetPathMesh(DStaticMesh::CreateDebugTriangle());
	FSplinePoint A({0.0, 0.0, 0.0});
	FSplinePoint B({100.0, 0.0, 0.0});
	FSplinePoint C({200.0, 0.0, 0.0});
	Actor->GetSplineComponent()->SetSplinePoints({A, B, C});
	Actor->GetSplineComponent()->SetClosedLoop(true);
	auto Segments = Actor->FindComponentsByClass<DSplineMeshComponent>();
	ASSERT_EQ(Segments.size(), 3u);
	std::unordered_map<FGuid, DActorComponent*> IdentityByStartGuid;
	for (DActorComponent* SegmentObject : Segments)
	{
		auto* Segment = Cast<DSplineMeshComponent>(SegmentObject);
		ASSERT_NE(Segment, nullptr);
		const FVector3 Start = Segment->GetSplineMeshParams().StartPosition;
		const FSplinePoint* Point = Start == A.Position ? &A : Start == B.Position ? &B : &C;
		IdentityByStartGuid.emplace(Point->Id, Segment);
	}
	ASSERT_TRUE(Actor->GetSplineComponent()->MoveSplinePoint(0, 2));
	Segments = Actor->FindComponentsByClass<DSplineMeshComponent>();
	ASSERT_EQ(Segments.size(), 3u);
	for (DActorComponent* SegmentObject : Segments)
	{
		auto* Segment = Cast<DSplineMeshComponent>(SegmentObject);
		const FVector3 Start = Segment->GetSplineMeshParams().StartPosition;
		const FSplinePoint* Point = Start == A.Position ? &A : Start == B.Position ? &B : &C;
		EXPECT_NE(IdentityByStartGuid.at(Point->Id), Segment);
		FActorGeneratedComponentKey SourceKey;
		ASSERT_TRUE(Actor->GetGeneratedComponentKey(Segment, SourceKey));
		EXPECT_EQ(SourceKey.Id, Point->Id);
		EXPECT_TRUE(IdentityByStartGuid.at(Point->Id)->IsPendingKill());
	}
	Actor->GetSplineComponent()->SetClosedLoop(false);
	EXPECT_EQ(Actor->FindComponentsByClass<DSplineMeshComponent>().size(), 2u);
	Actor->GetSplineComponent()->SetSplinePoints({A});
	EXPECT_TRUE(Actor->FindComponentsByClass<DSplineMeshComponent>().empty());
	Actor->GetSplineComponent()->SetSplinePoints({});
	EXPECT_TRUE(Actor->FindComponentsByClass<DSplineMeshComponent>().empty());
	MarkObjectHierarchyAsGarbage(World);
	MarkAsGarbage(Actor->GetPathMesh());
	CollectGarbage();
}

TEST(FSplineComponentMutationTests, ListenerRemovalDuringPublicationIsSafe)
{
	InitializeDObjectSystem();
	if (!Durin::FAssetCompilingManager::Get().IsAcceptingRequests())
		ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
	auto* Spline = NewObject<DSplineComponent>(nullptr, "SplineMutationListeners");
	uint32 FirstCalls = 0;
	uint32 RemovedCalls = 0;
	uint64 RemovedId = 0;
	Spline->AddSplineMutationListener([&](uint64, ESplineChangeFlags,
		std::shared_ptr<const FSplineEvaluationData>) {
		++FirstCalls;
		Spline->RemoveSplineMutationListener(RemovedId);
	});
	RemovedId = Spline->AddSplineMutationListener([&](uint64, ESplineChangeFlags,
		std::shared_ptr<const FSplineEvaluationData>) { ++RemovedCalls; });
	Spline->AddSplinePoint(FSplinePoint({0.0, 0.0, 0.0}));
	EXPECT_EQ(FirstCalls, 1u);
	EXPECT_EQ(RemovedCalls, 0u);
	MarkAsGarbage(Spline);
	CollectGarbage();
}

TEST(FSplineMeshCollisionTests, DeformsSimpleBoxesAndConvexesIndependentlyAndPreservesDeferredState)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	auto* Source = NewObject<DBodySetup>(Mesh, "SimpleSource");
	ASSERT_TRUE(Mesh->SetBodySetup(Source));
	FSimpleCollisionElement Box; Box.Type = EBodySetupShapeType::Box; Box.Dimensions = {0.05, 0.1, 0.1};
	Box.Transform.Translation = {-0.3, 0, 0};
	FSimpleCollisionElement Hull; Hull.Type = EBodySetupShapeType::Convex;
	Hull.Vertices = {{-0.05, -0.1, -0.1}, {0.05, -0.1, -0.1}, {0, 0.1, -0.1}, {0, 0, 0.1}};
	Hull.Transform.Translation = {0.3, 0, 0};
	ASSERT_TRUE(Source->SetSimpleCollision({Box, Hull}));
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedSimpleCollision);
	ASSERT_TRUE(Component->GetCollisionBuildError().empty()) << Component->GetCollisionBuildError();
	auto* Body = Component->GetBodySetup(); ASSERT_NE(Body, nullptr);
	const auto& Elements = Body->GetSimpleCollision(); ASSERT_EQ(Elements.size(), 2u);
	EXPECT_EQ(Elements[0].Type, EBodySetupShapeType::Convex); EXPECT_EQ(Elements[0].Vertices.size(), 8u);
	EXPECT_EQ(Elements[1].Type, EBodySetupShapeType::Convex); EXPECT_EQ(Elements[1].Vertices.size(), 4u);
	EXPECT_EQ(Elements[1].Transform.Translation, FVector3(0));
	for (const auto& Vertex : Hull.Vertices)
	{
		const auto Expected = FSplineMeshDeformer::DeformPosition(Component->GetDerivedState()->Params, Vertex + Hull.Transform.Translation);
		EXPECT_TRUE(std::ranges::any_of(Elements[1].Vertices, [&](const FVector3& Actual) { return Math::Length(Actual - Expected) < 1.e-9; }));
	}
	FCollisionGeometryRef Before; ASSERT_TRUE(Body->BuildSimpleGeometry(Before));
	FPhysicsQueryHit Hit;
	EXPECT_EQ(CollisionGeometry::Raycast({2.5, 0, 1}, {2.5, 0, -1}, Before, FTransform(), CollisionGeometry::ECollisionQueryAlgorithm::Production, Hit), CollisionGeometry::ECollisionQueryStatus::Miss);
	auto Params = Component->GetSplineMeshParams(); Params.EndPosition.z = 1;
	Component->SetSplineMeshParams(Params, false);
	Box.Dimensions.y = 0.2; ASSERT_TRUE(Source->SetSimpleCollision({Box, Hull}));
	FCollisionGeometryRef Deferred; ASSERT_TRUE(Body->BuildSimpleGeometry(Deferred)); EXPECT_EQ(Deferred.GetIdentity(), Before.GetIdentity());
	Component->UpdateMesh(); ASSERT_TRUE(Body->BuildSimpleGeometry(Deferred)); EXPECT_NE(Deferred.GetIdentity(), Before.GetIdentity());
	EXPECT_TRUE(Component->GetCollisionBuildError().empty());
	Params.StartPosition.x = std::numeric_limits<double>::quiet_NaN(); Component->SetSplineMeshParams(Params);
	FPhysicsBodyDesc CollisionDesc; auto& Transform = CollisionDesc.SimpleTransform; EXPECT_FALSE(Component->BuildPhysicsBodyDesc(CollisionDesc));
}

TEST(FSplineMeshCollisionTests, SphereAndCapsuleUseSliceScaleAndCylinderLengthIncludingMirroredAxes)
{
	for (const auto Forward : {ESplineMeshAxis::X, ESplineMeshAxis::Y, ESplineMeshAxis::Z})
	{
		auto [Component, Mesh] = MakeComponentWithMesh();
		// The debug triangle is planar in Z; supply a volumetric render source for all forward axes.
		BuildCollisionLODs(Mesh, true);
		auto* Source = NewObject<DBodySetup>(Mesh, "SphereCapsuleSource"); ASSERT_TRUE(Mesh->SetBodySetup(Source));
		FSimpleCollisionElement Sphere; Sphere.Type = EBodySetupShapeType::Sphere; Sphere.Dimensions = FVector3(0.1);
		FSimpleCollisionElement Capsule; Capsule.Type = EBodySetupShapeType::Capsule; Capsule.Dimensions = {0.2, 0.2, 0.7};
		ASSERT_TRUE(Source->SetSimpleCollision({Sphere, Capsule}));
		auto Params = Component->GetSplineMeshParams(); Params.ForwardAxis = Forward;
		Params.StartScale = Params.EndScale = {-2, 3}; Params.StartRollRadians = Params.EndRollRadians = 0.4;
		Component->SetSplineMeshParams(Params);
		Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedSimpleCollision);
		ASSERT_TRUE(Component->GetCollisionBuildError().empty()) << Component->GetCollisionBuildError();
		const auto& Elements = Component->GetBodySetup()->GetSimpleCollision(); ASSERT_EQ(Elements.size(), 2u);
		EXPECT_EQ(Elements[0].Type, EBodySetupShapeType::Sphere); EXPECT_NEAR(Elements[0].Dimensions.x, 0.3, 1.e-9);
		EXPECT_EQ(Elements[1].Type, EBodySetupShapeType::Capsule); EXPECT_NEAR(Elements[1].Dimensions.x, 0.6, 1.e-9);
		const double AxisScale = Forward == ESplineMeshAxis::Z ? 1 : Forward == ESplineMeshAxis::X ? 3 : 2;
		EXPECT_NEAR(Elements[1].Dimensions.z, 0.6 + 0.5 * AxisScale, 1.e-9);
		EXPECT_EQ(Elements[0].Transform.Translation, FSplineMeshDeformer::DeformPosition(Component->GetDerivedState()->Params, FVector3(0)));
		EXPECT_TRUE(IsValidPhysicsTransform(Elements[1].Transform));
	}
}

TEST(FSplineMeshCollisionTests, CombinedCollisionRoutesWorldQueriesAndRefreshesSourceSettings)
{
	InitializeDObjectSystem();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests()) ASSERT_TRUE(InitializeAssetCompilingManager());
	auto* World = NewObject<DWorld>(nullptr, "SimpleComplexWorld"); ASSERT_TRUE(World->InitializeSubsystems());
	auto* Level = NewObject<DLevel>(World, "SimpleComplexLevel"); ASSERT_TRUE(World->SetCurrentLevel(Level));
	auto* Actor = Level->SpawnActor<AActor>("Owner");
	auto* Component = Cast<DSplineMeshComponent>(Actor->AddInstanceComponent(DSplineMeshComponent::StaticClass(), "Spline"));
	auto* Mesh = DStaticMesh::CreateDebugTriangle(Level); Component->SetStaticMesh(Mesh);
	auto* Source = NewObject<DBodySetup>(Mesh, "Body"); ASSERT_TRUE(Mesh->SetBodySetup(Source));
	FSimpleCollisionElement Sphere; Sphere.Type = EBodySetupShapeType::Sphere; Sphere.Dimensions = FVector3(0.2); Sphere.Transform.Translation = {0, 4, 0};
	ASSERT_TRUE(Source->SetSimpleCollision({Sphere}));
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedSimpleAndComplex);
	Component->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ASSERT_TRUE(Component->GetCollisionBuildError().empty()) << Component->GetCollisionBuildError();
	ASSERT_EQ(World->GetPhysicsScene().GetBodyCount(), 1u);
	FHitResult Hit; FCollisionQueryParams Simple; Simple.Complexity = EPhysicsQueryComplexity::Simple;
	FCollisionQueryParams Complex; Complex.Complexity = EPhysicsQueryComplexity::Complex;
	const FVector3 Center = FSplineMeshDeformer::DeformPosition(Component->GetDerivedState()->Params, Sphere.Transform.Translation);
	EXPECT_TRUE(World->LineTraceSingleByChannel(Hit, Center + FVector3(0, 0, 1), Center - FVector3(0, 0, 1), ECollisionChannel::Visibility, Simple));
	EXPECT_FALSE(World->LineTraceSingleByChannel(Hit, Center + FVector3(0, 0, 1), Center - FVector3(0, 0, 1), ECollisionChannel::Visibility, Complex));
	Source->SetCollisionTraceMode(EBodySetupCollisionTraceMode::SimpleAsComplex);
	EXPECT_FALSE(Component->GetBodySetup()->GetResidentComplexGeometry());
	auto* EmptyProxy = NewObject<DStaticMesh>(Level, "UnusedComplexProxy");
	ASSERT_TRUE(Mesh->SetComplexCollisionMesh(EmptyProxy));
	EXPECT_TRUE(Component->GetCollisionBuildError().empty());
	EXPECT_TRUE(World->LineTraceSingleByChannel(Hit, Center + FVector3(0, 0, 1), Center - FVector3(0, 0, 1), ECollisionChannel::Visibility, Complex));
	ASSERT_TRUE(Mesh->SetComplexCollisionMesh(nullptr));
	Source->SetCollisionTraceMode(EBodySetupCollisionTraceMode::ComplexAsSimple);
	EXPECT_TRUE(Component->GetBodySetup()->GetSimpleCollision().empty());
	EXPECT_FALSE(World->LineTraceSingleByChannel(Hit, Center + FVector3(0, 0, 1), Center - FVector3(0, 0, 1), ECollisionChannel::Visibility, Simple));
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedSimpleCollision);
	EXPECT_TRUE(Component->GetBodySetup()->GetResidentComplexGeometry());
	EXPECT_TRUE(Component->GetBodySetup()->GetSimpleCollision().empty());
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh);
	EXPECT_TRUE(Component->GetBodySetup()->GetResidentComplexGeometry());
	EXPECT_TRUE(Component->GetBodySetup()->GetSimpleCollision().empty());
	MarkObjectHierarchyAsGarbage(World); CollectGarbage();
}

TEST(FSplineMeshCollisionTests, CapsulePreservesSourceScaleConventionAndCanCollapseToSphere)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	auto* Source = NewObject<DBodySetup>(Mesh, "ScaledCapsule"); ASSERT_TRUE(Mesh->SetBodySetup(Source));
	FSimpleCollisionElement Capsule; Capsule.Type = EBodySetupShapeType::Capsule; Capsule.Dimensions = {0.2, 0.2, 0.7};
	Capsule.Transform.Scale3D = {2, 0.5, 4};
	Capsule.Transform.Rotation = Math::MakeQuaternionFromAxisAngleDegrees(90, FVectorConstants::Forward) * 4.0;
	ASSERT_TRUE(Source->SetSimpleCollision({Capsule}));
	EXPECT_NEAR(Math::Length(Source->GetSimpleCollision()[0].Transform.Rotation), 1, 1.e-12);
	auto Params = Component->GetSplineMeshParams(); Params.StartScale = Params.EndScale = {-2, 3};
	Component->SetSplineMeshParams(Params); Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedSimpleCollision);
	ASSERT_TRUE(Component->GetCollisionBuildError().empty()) << Component->GetCollisionBuildError();
	const auto& Scaled = Component->GetBodySetup()->GetSimpleCollision(); ASSERT_EQ(Scaled.size(), 1u);
	EXPECT_NEAR(Scaled[0].Dimensions.x, 1.2, 1.e-9); EXPECT_NEAR(Scaled[0].Dimensions.z, 6.0, 1.e-9);
	const FVector3 Axis = Math::RotateVector(Scaled[0].Transform.Rotation, FVectorConstants::Up);
	EXPECT_NEAR(std::abs(Axis.y), 1.0, 1.e-9);
	Params.StartScale = Params.EndScale = {0, 0}; Component->SetSplineMeshParams(Params);
	ASSERT_TRUE(Component->GetCollisionBuildError().empty()) << Component->GetCollisionBuildError();
	const auto& Collapsed = Component->GetBodySetup()->GetSimpleCollision(); ASSERT_EQ(Collapsed.size(), 1u);
	EXPECT_NEAR(Collapsed[0].Dimensions.x, 0.4, 1.e-9); EXPECT_NEAR(Collapsed[0].Dimensions.z, 0.4, 1.e-9);
	FPhysicsBodyDesc CollisionDesc; auto& Transform = CollisionDesc.ComplexTransform; EXPECT_TRUE(Component->BuildPhysicsBodyDesc(CollisionDesc));
}

TEST(FSplineMeshCollisionTests, DomainUpdatesPublishOnceAndReuseUnaffectedGeometry)
{
	InitializeDObjectSystem();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests()) ASSERT_TRUE(InitializeAssetCompilingManager());
	auto* World = NewObject<DWorld>(nullptr, "CollisionTransactionWorld"); ASSERT_TRUE(World->InitializeSubsystems());
	auto* Level = NewObject<DLevel>(World, "CollisionTransactionLevel"); ASSERT_TRUE(World->SetCurrentLevel(Level));
	auto* Mesh = DStaticMesh::CreateDebugTriangle(Level);
	auto* Source = NewObject<DBodySetup>(Mesh, "Source"); ASSERT_TRUE(Mesh->SetBodySetup(Source));
	Source->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	Source->FinishPhysicsMeshes(); ASSERT_TRUE(Source->GetResidentComplexGeometry());
	ASSERT_TRUE(Source->SetSphere(0.2, {0, 4, 0}));
	auto* Actor = Level->SpawnActor<AActor>("Owner");
	auto* Component = Cast<DSplineMeshComponent>(Actor->AddInstanceComponent(DSplineMeshComponent::StaticClass(), "Spline"));
	Component->SetStaticMesh(Mesh);
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedSimpleAndComplex);
	Component->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ASSERT_TRUE(Component->GetCollisionBuildError().empty());
	auto* Body = Component->GetBodySetup(); ASSERT_NE(Body, nullptr);
	const auto Render = Component->GetDerivedState();
	const auto Registrations = [&] { return World->GetPhysicsScene().CaptureQueryDiagnostics().Mutations.AddSuccesses; };
	const auto SourceComplex = Source->GetResidentComplexGeometry();
	const auto SourceGeneration = Source->GetPhysicsMeshRequestGeneration();
	const auto SourceRevision = Source->GetRevision();
	const auto Complex = Body->GetResidentComplexGeometry();
	const auto Revision = Body->GetRevision();
	const auto BuildRevision = Body->GetCollisionBuildRevision();
	const auto Registration = Registrations();
	FCollisionGeometryRef Simple; ASSERT_TRUE(Body->BuildSimpleGeometry(Simple));
	{
		const FScopedBodySetupUpdate Update(*Source);
		ASSERT_TRUE(Source->SetSphere(0.3, {0, 4, 0}));
		Source->SetCollisionQueryPolicy(EBodySetupCollisionQueryPolicy::ComplexOnly);
		EXPECT_EQ(Body->GetRevision(), Revision);
		EXPECT_EQ(Registrations(), Registration);
	}
	EXPECT_EQ(Source->GetRevision(), SourceRevision + 1);
	EXPECT_EQ(Source->GetPhysicsMeshRequestGeneration(), SourceGeneration);
	EXPECT_EQ(Source->GetResidentComplexGeometry().GetIdentity(), SourceComplex.GetIdentity());
	EXPECT_EQ(Body->GetRevision(), Revision + 1);
	EXPECT_EQ(Body->GetCollisionBuildRevision(), BuildRevision);
	EXPECT_EQ(Body->GetResidentComplexGeometry().GetIdentity(), Complex.GetIdentity());
	EXPECT_EQ(Registrations(), Registration + 1);
	EXPECT_EQ(Component->GetDerivedState(), Render);
	FCollisionGeometryRef ChangedSimple; ASSERT_TRUE(Body->BuildSimpleGeometry(ChangedSimple));
	EXPECT_NE(ChangedSimple.GetIdentity(), Simple.GetIdentity());
	auto* Proxy = DStaticMesh::CreateDebugTriangle(Level);
	ASSERT_TRUE(Mesh->SetComplexCollisionMesh(Proxy)); Source->FinishPhysicsMeshes();
	ASSERT_TRUE(Component->GetCollisionBuildError().empty());
	FCollisionGeometryRef Preserved; ASSERT_TRUE(Body->BuildSimpleGeometry(Preserved));
	EXPECT_EQ(Preserved.GetIdentity(), ChangedSimple.GetIdentity());
	EXPECT_EQ(Body->GetCollisionBuildRevision(), BuildRevision + 1);
	EXPECT_EQ(Body->GetRevision(), Revision + 2);
	EXPECT_EQ(Registrations(), Registration + 2);
	const auto NewComplex = Body->GetResidentComplexGeometry();
	const auto NewSourceGeneration = Source->GetPhysicsMeshRequestGeneration();
	Source->SetCollisionTraceMode(EBodySetupCollisionTraceMode::ComplexAsSimple);
	EXPECT_EQ(Body->GetResidentComplexGeometry().GetIdentity(), NewComplex.GetIdentity());
	EXPECT_EQ(Body->GetCollisionBuildRevision(), BuildRevision + 1);
	EXPECT_EQ(Source->GetPhysicsMeshRequestGeneration(), NewSourceGeneration);
	EXPECT_EQ(Body->GetRevision(), Revision + 3);
	EXPECT_EQ(Registrations(), Registration + 3);
	EXPECT_TRUE(Body->GetSimpleCollision().empty());
	Source->SetCollisionTraceMode(EBodySetupCollisionTraceMode::SimpleAsComplex);
	EXPECT_FALSE(Body->GetResidentComplexGeometry());
	ASSERT_TRUE(Body->BuildSimpleGeometry(Preserved));
	const auto AppliedRevision = Body->GetRevision();
	const auto AppliedRegistration = Registrations();
	auto Params = Component->GetSplineMeshParams(); Params.EndPosition.z = 1;
	Component->SetSplineMeshParams(Params, false);
	ASSERT_TRUE(Source->SetSphere(0.4, {0, 4, 0}));
	EXPECT_EQ(Body->GetRevision(), AppliedRevision);
	Component->UpdateMesh();
	EXPECT_EQ(Body->GetRevision(), AppliedRevision + 1);
	EXPECT_EQ(Registrations(), AppliedRegistration + 1);
	EXPECT_TRUE(Component->GetCollisionBuildError().empty());
	MarkObjectHierarchyAsGarbage(World); CollectGarbage();
}

TEST(FSplineMeshCollisionTests, CombinedRebuildAndFailureEachPublishOneBodyRevision)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	auto* Source = NewObject<DBodySetup>(Mesh, "CombinedTransactionSource"); ASSERT_TRUE(Mesh->SetBodySetup(Source));
	ASSERT_TRUE(Source->SetBox({0.2, 0.2, 0.2}));
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedSimpleAndComplex);
	auto* Body = Component->GetBodySetup(); ASSERT_NE(Body, nullptr);
	const auto Revision = Body->GetRevision(); const auto Build = Body->GetCollisionBuildRevision();
	const auto Request = Body->GetPhysicsMeshRequestGeneration();
	auto Params = Component->GetSplineMeshParams(); Params.EndPosition.z = 0.5;
	Component->SetSplineMeshParams(Params);
	EXPECT_EQ(Body->GetRevision(), Revision + 1);
	EXPECT_EQ(Body->GetCollisionBuildRevision(), Build + 1);
	EXPECT_EQ(Body->GetPhysicsMeshRequestGeneration(), Request + 1);
	FCollisionGeometryRef Simple; ASSERT_TRUE(Body->BuildSimpleGeometry(Simple));
	EXPECT_TRUE(Body->GetResidentComplexGeometry());
	const auto BeforeFailure = Body->GetRevision();
	Params.StartPosition.x = std::numeric_limits<double>::quiet_NaN(); Component->SetSplineMeshParams(Params);
	EXPECT_EQ(Body->GetRevision(), BeforeFailure + 1);
	EXPECT_FALSE(Body->BuildSimpleGeometry(Simple)); EXPECT_FALSE(Body->GetResidentComplexGeometry());
}

TEST(FSplineMeshCollisionTests, PrimitiveAndAggregateInputsDeformIdenticallyWithoutCenterCompensation)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	auto* Source = NewObject<DBodySetup>(Mesh, "UnifiedSplineSimpleSource");
	ASSERT_TRUE(Mesh->SetBodySetup(Source));
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedSimpleCollision);
	for (const auto Type : {EBodySetupShapeType::Box, EBodySetupShapeType::Sphere, EBodySetupShapeType::Capsule})
	{
		if (Type == EBodySetupShapeType::Box) ASSERT_TRUE(Source->SetBox({0.1, 0.2, 0.3}, {0.2, 0.3, 0.1}));
		else if (Type == EBodySetupShapeType::Sphere) ASSERT_TRUE(Source->SetSphere(0.2, {0.2, 0.3, 0.1}));
		else ASSERT_TRUE(Source->SetCapsule(0.2, 0.5, {0.2, 0.3, 0.1}));
		ASSERT_TRUE(Component->GetCollisionBuildError().empty()) << Component->GetCollisionBuildError();
		auto* Body = Component->GetBodySetup(); ASSERT_NE(Body, nullptr);
		const auto Before = Body->GetSimpleCollision(); ASSERT_EQ(Before.size(), 1u);
		FCollisionGeometryRef PrimitiveGeometry; ASSERT_TRUE(Body->BuildSimpleGeometry(PrimitiveGeometry));
		FVector3 BeforeMin, BeforeMax; ASSERT_TRUE(PrimitiveGeometry.GetLocalBounds(BeforeMin, BeforeMax));
		ASSERT_TRUE(Source->SetSimpleCollision(Source->GetSimpleCollision()));
		ASSERT_TRUE(Component->GetCollisionBuildError().empty()) << Component->GetCollisionBuildError();
		const auto& After = Body->GetSimpleCollision(); ASSERT_EQ(After.size(), Before.size());
		EXPECT_EQ(After[0].Type, Before[0].Type);
		EXPECT_EQ(After[0].Dimensions, Before[0].Dimensions);
		EXPECT_EQ(After[0].Transform.Translation, Before[0].Transform.Translation);
		EXPECT_EQ(After[0].Transform.Rotation, Before[0].Transform.Rotation);
		EXPECT_EQ(After[0].Vertices, Before[0].Vertices);
		EXPECT_EQ(After[0].Indices, Before[0].Indices);
		FCollisionGeometryRef AggregateGeometry; ASSERT_TRUE(Body->BuildSimpleGeometry(AggregateGeometry));
		FVector3 AfterMin, AfterMax; ASSERT_TRUE(AggregateGeometry.GetLocalBounds(AfterMin, AfterMax));
		EXPECT_EQ(AfterMin, BeforeMin); EXPECT_EQ(AfterMax, BeforeMax);
		ASSERT_TRUE(Source->SetSimpleCollision({}));
		EXPECT_TRUE(Body->GetSimpleCollision().empty());
		FCollisionGeometryRef Empty; EXPECT_FALSE(Body->BuildSimpleGeometry(Empty));
	}
}

TEST(FSplineMeshCollisionTests, WorldQueriesDefaultToSimpleAndDebugCapturesPublishedRepresentations)
{
	InitializeDObjectSystem();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests()) ASSERT_TRUE(InitializeAssetCompilingManager());
	auto* World = NewObject<DWorld>(nullptr, "PublishedSplineQueries"); ASSERT_TRUE(World->InitializeSubsystems());
	auto* Level = NewObject<DLevel>(World, "PublishedSplineLevel"); ASSERT_TRUE(World->SetCurrentLevel(Level));
	auto* Actor = Level->SpawnActor<AActor>("Owner");
	auto* Component = Cast<DSplineMeshComponent>(Actor->AddInstanceComponent(DSplineMeshComponent::StaticClass(), "Spline"));
	auto* Mesh = DStaticMesh::CreateDebugTriangle(Level); Component->SetStaticMesh(Mesh);
	auto* Source = NewObject<DBodySetup>(Mesh, "SourceBody"); ASSERT_TRUE(Mesh->SetBodySetup(Source));
	ASSERT_TRUE(Source->SetSphere(0.2, {0, 4, 0}));
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh);
	Component->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	const auto Data = Component->GetPhysicsTriMeshData(); ASSERT_TRUE(Data);
	const auto Center = (FVector3(Data->Positions[Data->Indices[0]]) + FVector3(Data->Positions[Data->Indices[1]]) + FVector3(Data->Positions[Data->Indices[2]])) / 3.0;
	const auto Normal = Math::NormalizeOr(Math::Cross(FVector3(Data->Positions[1]) - FVector3(Data->Positions[0]), FVector3(Data->Positions[2]) - FVector3(Data->Positions[0])), FVectorConstants::Up);
	FHitResult Hit; FCollisionQueryParams Complex; Complex.Complexity = EPhysicsQueryComplexity::Complex;
	EXPECT_FALSE(World->LineTraceSingleByChannel(Hit, Center + Normal, Center - Normal, ECollisionChannel::Visibility));
	EXPECT_TRUE(World->LineTraceSingleByChannel(Hit, Center + Normal, Center - Normal, ECollisionChannel::Visibility, Complex));
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedSimpleAndComplex);
	World->SetCollisionDebugDrawEnabled(true);
	auto Snapshot = World->CaptureCollisionDebugSnapshot(); ASSERT_EQ(Snapshot.Bodies.size(), 2u);
	EXPECT_EQ(Snapshot.Bodies[0].Handle, Snapshot.Bodies[1].Handle);
	EXPECT_EQ(Snapshot.Bodies[0].Representation, EPhysicsQueryComplexity::Simple);
	EXPECT_EQ(Snapshot.Bodies[1].Representation, EPhysicsQueryComplexity::Complex);
	EXPECT_NE(Snapshot.Bodies[0].ResourceIdentity, Snapshot.Bodies[1].ResourceIdentity);
	Source->SetCollisionTraceMode(EBodySetupCollisionTraceMode::ComplexAsSimple);
	EXPECT_TRUE(World->LineTraceSingleByChannel(Hit, Center + Normal, Center - Normal, ECollisionChannel::Visibility));
	Snapshot = World->CaptureCollisionDebugSnapshot(); ASSERT_EQ(Snapshot.Bodies.size(), 1u);
	EXPECT_EQ(Snapshot.Bodies[0].Representation, EPhysicsQueryComplexity::Complex);
	EXPECT_EQ(Snapshot.Bodies[0].Component, Component);
	MarkObjectHierarchyAsGarbage(World); CollectGarbage();
}
