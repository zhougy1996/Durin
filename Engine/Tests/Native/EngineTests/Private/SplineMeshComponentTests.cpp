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

	auto CreateAuthoredDebugTriangle(DObject* Outer, std::string_view Name)
		-> DStaticMesh*
	{
		auto* Mesh = NewObject<DStaticMesh>(Outer, FName(Name));
		FStaticMeshDecodedGeometry Imported;
		Imported.MaterialSlots.push_back({
			.Name = "Default", .SourceMaterialIndex = 0, .SourceName = "Default"});
		FStaticMeshImportedMesh& Section = Imported.Meshes.emplace_back();
		Section.Name = "Triangle";
		Section.Positions = {
			FVector3f(-0.5f, -0.5f, 0.0f),
			FVector3f(0.5f, -0.5f, 0.0f),
			FVector3f(0.0f, 0.5f, 0.0f)};
		Section.Indices = {0, 1, 2};
		Section.SourceMaterialIndex = 0;
		std::string Error;
		if (!BuildStaticMeshSynchronously(
			*Mesh, std::move(Imported), Error))
		{
			ADD_FAILURE() << Error;
			return nullptr;
		}
		return Mesh;
	}

	auto MakeComponentWithMesh() -> std::pair<DSplineMeshComponent*, DStaticMesh*>
	{
		auto* Component = NewObject<DSplineMeshComponent>(nullptr, "SplineMeshComponent");
		auto* Mesh = DStaticMesh::CreateDebugTriangle();
		EXPECT_NE(Component, nullptr);
		EXPECT_NE(Mesh, nullptr);
		Component->SetStaticMesh(Mesh);
		return {Component, Mesh};
	}
}

TEST(FSplineMeshComponentTests, DefaultObjectPublishesDiagnosticStateAndReflectsAuthoredFields)
{
	auto* Component = NewObject<DSplineMeshComponent>(nullptr, "DefaultSplineMeshComponent");
	ASSERT_NE(Component, nullptr);
	const auto State = Component->GetDerivedState();
	ASSERT_NE(State, nullptr);
	EXPECT_EQ(State->Status, ESplineMeshDerivedStateStatus::NoStaticMesh);
	EXPECT_EQ(Component->GetDeformationRevision(), 0u);
	EXPECT_NE(Component->GetClass()->FindPropertyByName(FName("StaticMesh")), nullptr);
	EXPECT_NE(Component->GetClass()->FindPropertyByName(FName("SplineMeshParams")), nullptr);
	EXPECT_NE(Component->GetClass()->FindPropertyByName(FName("OverrideMaterials")), nullptr);
}

TEST(FSplineMeshComponentTests, PlanarStaticMeshPublishesZeroThicknessLocalBounds)
{
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
	FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
	FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/Engine/Models/SplineBox", Path));
	DStaticMesh* Mesh = nullptr;
	const FAssetResult LoadResult = LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Mesh);
	ASSERT_TRUE(LoadResult) << LoadResult.Message;
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

TEST(FSplineMeshComponentTests, PublishesNormalizedExactLOD0AndConservativeBounds)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	const auto Initial = Component->GetDerivedState();
	ASSERT_TRUE(Initial && Initial->IsValid()) << (Initial ? Initial->Diagnostic : "missing state");
	EXPECT_EQ(Initial->DeformedLOD0Positions.size(), 3u);
	EXPECT_EQ(Initial->LOD0Indices, (std::vector<uint32>{0, 1, 2}));
	EXPECT_NE(Initial->EditorAcceleration, nullptr);
	EXPECT_NE(Initial->CollisionInputIdentity, 0u);
	EXPECT_EQ(Initial->SourceRenderResourceRevision, Mesh->GetRenderResourceStatus().Revision);
	EXPECT_NEAR(Initial->Params.SourceForwardMin, -0.65, 1.e-6);
	EXPECT_NEAR(Initial->Params.SourceForwardMax, 0.65, 1.e-6);
	for (const FVector3f& Position : Initial->DeformedLOD0Positions)
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
	std::string Error;
	ASSERT_TRUE(Component->SetSplineMeshParams(Params, &Error)) << Error;
	const auto Curved = Component->GetDerivedState();
	ASSERT_TRUE(Curved && Curved->IsValid());
	EXPECT_GT(Curved->DeformationRevision, Initial->DeformationRevision);
	EXPECT_NE(Curved->CollisionInputIdentity, Initial->CollisionInputIdentity);
	EXPECT_EQ(Initial->Params.EndPosition, FVector3(5.0, 0.0, 0.0));
}

TEST(FSplineMeshComponentTests, InvalidProposalPreservesAuthoredAndPublishedState)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	const FSplineMeshParams BeforeParams = Component->GetSplineMeshParams();
	const auto BeforeState = Component->GetDerivedState();
	const uint64 BeforeRevision = Component->GetDeformationRevision();
	FSplineMeshParams Invalid = BeforeParams;
	Invalid.StartPosition.x = std::numeric_limits<double>::quiet_NaN();
	std::string Error;
	EXPECT_FALSE(Component->SetSplineMeshParams(Invalid, &Error));
	EXPECT_FALSE(Error.empty());
	EXPECT_EQ(Component->GetSplineMeshParams(), BeforeParams);
	EXPECT_EQ(Component->GetDerivedState(), BeforeState);
	EXPECT_EQ(Component->GetDeformationRevision(), BeforeRevision);
}

TEST(FSplineMeshComponentTests, ReflectedInvalidDraftIsRejectedBeforeMutation)
{
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
	std::string Error;
	EXPECT_FALSE(Component->PreEditChangeProperty(Proposal, Error));
	EXPECT_FALSE(Error.empty());
	EXPECT_TRUE(Math::IsFinite(Component->GetSplineMeshParams().EndPosition));
	EXPECT_TRUE(std::isfinite(Component->GetSplineMeshParams().EndRollRadians));
}

TEST(FSplineMeshComponentTests, DuplicateRebuildsIndependentEquivalentSnapshot)
{
	auto [Source, Mesh] = MakeComponentWithMesh();
	FSplineMeshParams Params = Source->GetSplineMeshParams();
	Params.EndPosition = {75.0, 25.0, 10.0};
	ASSERT_TRUE(Source->SetSplineMeshParams(Params));
	auto* Duplicate = Cast<DSplineMeshComponent>(
		DuplicateObject(Source, nullptr, "SplineMeshDuplicate"));
	ASSERT_NE(Duplicate, nullptr);
	EXPECT_EQ(Duplicate->GetStaticMesh(), Mesh);
	EXPECT_EQ(Duplicate->GetSplineMeshParams(), Source->GetSplineMeshParams());
	const auto SourceState = Source->GetDerivedState();
	const auto DuplicateState = Duplicate->GetDerivedState();
	ASSERT_TRUE(SourceState && DuplicateState && DuplicateState->IsValid());
	EXPECT_NE(SourceState, DuplicateState);
	EXPECT_EQ(DuplicateState->Params, SourceState->Params);
	EXPECT_EQ(DuplicateState->DeformedLOD0Positions, SourceState->DeformedLOD0Positions);
	EXPECT_EQ(DuplicateState->LOD0Indices, SourceState->LOD0Indices);
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
}

TEST(FSplineMeshCollisionTests, UsesExactDerivedTriangleMeshAndRevisionsEveryInputMutation)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh);
	const auto FirstState = Component->GetDerivedState();
	ASSERT_TRUE(FirstState && FirstState->IsValid());
	ASSERT_TRUE(FirstState->CollisionGeometry.IsValid());
	EXPECT_EQ(FirstState->CollisionGeometry.GetKind(), ECollisionGeometryKind::TriangleMesh);
	EXPECT_EQ(FirstState->CollisionGeometry.GetTriangleCount(), 1u);
	FCollisionGeometryRef Geometry;
	FTransform Transform;
	ASSERT_TRUE(Component->BuildCollisionGeometry(Geometry, Transform));
	EXPECT_EQ(Geometry.GetIdentity(), FirstState->CollisionGeometry.GetIdentity());

	const FVector3 A(FirstState->DeformedLOD0Positions[0]);
	const FVector3 B(FirstState->DeformedLOD0Positions[1]);
	const FVector3 C(FirstState->DeformedLOD0Positions[2]);
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
	ASSERT_TRUE(Component->SetSplineMeshParams(Params));
	const auto CurvedState = Component->GetDerivedState();
	ASSERT_TRUE(CurvedState && CurvedState->CollisionGeometry.IsValid());
	EXPECT_NE(CurvedState->CollisionInputIdentity, FirstState->CollisionInputIdentity);
	EXPECT_NE(CurvedState->CollisionGeometry.GetIdentity(), Geometry.GetIdentity());

	Params.StartScale = {-1.0, 1.0};
	Params.EndScale = {-1.0, 1.0};
	ASSERT_TRUE(Component->SetSplineMeshParams(Params));
	EXPECT_TRUE(Component->BuildCollisionGeometry(Geometry, Transform));
	Params.StartScale = {0.0, 0.0};
	Params.EndScale = {0.0, 0.0};
	Params.EndPosition = Params.StartPosition;
	Params.StartTangent = FVector3(0.0);
	Params.EndTangent = FVector3(0.0);
	ASSERT_TRUE(Component->SetSplineMeshParams(Params));
	EXPECT_TRUE(Component->GetDerivedState()->IsValid());
	EXPECT_FALSE(Component->BuildCollisionGeometry(Geometry, Transform));
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::Disabled);
	EXPECT_FALSE(Component->BuildCollisionGeometry(Geometry, Transform));
}

TEST(FSplineMeshCollisionTests, RegisteredMutationReplacesBodiesWithoutStaleHandles)
{
	InitializeDObjectSystem();
	if (!Durin::FAssetCompilingManager::Get().IsAcceptingRequests())
		ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
	auto* World = NewObject<DWorld>(nullptr, "SplineMeshCollisionWorld");
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
	ASSERT_TRUE(Component->SetSplineMeshParams(Params));
	ASSERT_EQ(PhysicsScene.GetBodyCount(), 1u);
	EXPECT_FALSE(PhysicsScene.ContainsBody(FirstHandle));
	EXPECT_TRUE(Component->GetPhysicsActorHandle().IsValid());
	EXPECT_NE(Component->GetPhysicsActorHandle(), FirstHandle);
	EXPECT_NE(Component->GetPublishedBodySetupRevision(), FirstRevision);

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
	ASSERT_TRUE(Component->SetSplineMeshParams(Params));
	ASSERT_TRUE(SavePackage(Level->GetPackage()));
	ASSERT_TRUE(UnloadPackage(Path));

	DObject* LoadedObject = nullptr;
	ASSERT_TRUE(LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), LoadedObject));
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
	EXPECT_EQ(State->DeformedLOD0Positions.size(), 3u);
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
	EXPECT_NE(std::ranges::find(Segments, First), Segments.end());
	EXPECT_NE(std::ranges::find(Segments, Second), Segments.end());
	EXPECT_EQ(First->GetSplineMeshParams().EndPosition, B.Position);

	FSplinePoint Inserted({50.0, 10.0, 0.0});
	ASSERT_TRUE(Actor->GetSplineComponent()->InsertSplinePoint(1, Inserted));
	Segments = Actor->FindComponentsByClass<DSplineMeshComponent>();
	ASSERT_EQ(Segments.size(), 3u);
	EXPECT_NE(std::ranges::find(Segments, First), Segments.end());
	EXPECT_NE(std::ranges::find(Segments, Second), Segments.end());
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
		FCollisionGeometryRef Geometry;
		FTransform Transform;
		EXPECT_TRUE(Segment->BuildCollisionGeometry(Geometry, Transform));
	}
	auto* ReplacementMesh = CreateAuthoredDebugTriangle(Level, "ReplacementStaticMesh");
	ASSERT_NE(ReplacementMesh, nullptr);
	Actor->SetPathMesh(ReplacementMesh);
	MarkAsGarbage(InitialMesh);
	CollectGarbage();
	Segments = Actor->FindComponentsByClass<DSplineMeshComponent>();
	EXPECT_NE(std::ranges::find(Segments, First), Segments.end());
	EXPECT_NE(std::ranges::find(Segments, Second), Segments.end());
	for (DActorComponent* Owned : Segments)
		EXPECT_EQ(Cast<DSplineMeshComponent>(Owned)->GetStaticMesh(), ReplacementMesh);
	FTransform ActorTransform;
	ActorTransform.Translation = {25.0, 50.0, 10.0};
	ASSERT_TRUE(Actor->SetActorTransform(ActorTransform));
	EXPECT_EQ(Actor->FindComponentsByClass<DSplineMeshComponent>(), Segments);

	Level->GetPackage()->ClearDirty();
	ASSERT_TRUE(Actor->RequestNativeReconstruction());
	EXPECT_FALSE(Level->GetPackage()->IsDirty());
	EXPECT_EQ(Actor->FindComponentsByClass<DSplineMeshComponent>().size(), 2u);
	std::unordered_map<DObject*, DObject*> Duplicates;
	auto* Duplicate = Cast<ASplineMeshActor>(DuplicateObject(
		Actor, Level, "SplineMeshActorDuplicate", &Duplicates));
	ASSERT_NE(Duplicate, nullptr);
	EXPECT_EQ(Duplicate->GetSplineComponent()->GetSplinePoints(),
		Actor->GetSplineComponent()->GetSplinePoints());
	const auto DuplicateSegments = Duplicate->FindComponentsByClass<DSplineMeshComponent>();
	ASSERT_EQ(DuplicateSegments.size(), 2u);
	for (DActorComponent* Segment : DuplicateSegments)
		EXPECT_EQ(Segment->GetCreationMethod(), EComponentCreationMethod::Generated);
	Level->GetPackage()->MarkDirty();
	const FAssetResult SaveResult = SavePackage(Level->GetPackage());
	EXPECT_TRUE(SaveResult) << SaveResult.Message;
	EXPECT_TRUE(UnloadPackage(Path));
	DObject* LoadedObject = nullptr;
	ASSERT_TRUE(LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), LoadedObject));
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
		EXPECT_EQ(IdentityByStartGuid.at(Point->Id), Segment);
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
