#include "Actors/CameraActor.h"
#include "Actors/StaticMeshActor.h"
#include "Collision/CollisionGeometry.h"
#include "Components/ShapeComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Math/Operations.h"
#include "Modules/ModuleManager.h"
#include "NativeDObjectTestSupport.h"
#include "Physics/BodySetup.h"
#include "Physics/PhysicsScene.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "Threading/Task.h"

#include <gtest/gtest.h>
#include <iostream>
#include <random>

namespace
{
#if defined(_WIN32)
	inline constexpr uint64 ExpectedPrimitiveRetainedBytes =
		DURIN_BUILD_DEBUG ? 208u : 200u;
	inline constexpr uint64 ExpectedCompoundRetainedBytes =
		DURIN_BUILD_DEBUG ? 7'264u : 7'256u;
#else
	inline constexpr uint64 ExpectedPrimitiveRetainedBytes = 200u;
	inline constexpr uint64 ExpectedCompoundRetainedBytes = 7'256u;
#endif
	auto MakeBoxBody(
		const Durin::FVector3& Center,
		const Durin::FVector3& HalfExtent = Durin::FVector3(0.5),
		const Durin::FQuat& Rotation = Durin::FQuatConstants::Identity) -> Durin::FPhysicsBodyDesc
	{
		Durin::FPhysicsBodyDesc Desc;
		Desc.Geometry = Durin::FCollisionGeometryRef::MakePrimitive(
			Durin::FCollisionShape::MakeBox(HalfExtent));
		Desc.Transform = Durin::FTransform();
		Desc.Transform.Translation = Center;
		Desc.Transform.Rotation = Rotation;
		return Desc;
	}

	auto CreatePhysicsWorld() -> Durin::DWorld*
	{
		Durin::Testing::InitializeDObjectSystemForTests();
		auto* World = Durin::NewObject<Durin::DWorld>(nullptr, "PhysicsWorld");
		EXPECT_TRUE(World->SetCurrentLevel(Durin::NewObject<Durin::DLevel>(World, "PhysicsLevel")));
		return World;
	}

	auto AddWorldBox(
		Durin::DWorld& World,
		Durin::FVector3 Center,
		Durin::FVector3 HalfExtent) -> Durin::DBoxComponent*
	{
		auto* Actor = World.SpawnActor<Durin::ACameraActor>("BoxOwner");
		auto* Box = Durin::Cast<Durin::DBoxComponent>(
			Actor->AddInstanceComponent(Durin::DBoxComponent::StaticClass(), "Collision"));
		EXPECT_NE(Box, nullptr);
		EXPECT_TRUE(Box->SetBoxHalfExtent(HalfExtent));
		EXPECT_TRUE(Box->SetCollisionProfileName(Durin::CollisionProfile::WorldStatic));
		Box->SetWorldLocation(Center);
		return Box;
	}
}

TEST(FPrimitiveComponentCollisionEditingTests, DirectFilterEditsLeaveThePreviousProfile)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	auto* Actor = Durin::NewObject<Durin::ACameraActor>(nullptr, "CollisionEditingActor");
	auto* Component = Durin::Cast<Durin::DBoxComponent>(
		Actor->AddInstanceComponent(Durin::DBoxComponent::StaticClass(), "Collision"));
	ASSERT_NE(Component, nullptr);
	Durin::FProperty* BodyProperty = Component->GetClass()->FindPropertyByName("BodyInstance");
	ASSERT_NE(BodyProperty, nullptr);
	auto* BodyStructProperty = static_cast<Durin::FStructProperty*>(BodyProperty);
	Durin::FProperty* EnabledProperty = BodyStructProperty->GetStruct()->FindPropertyByName(
		Durin::FName("CollisionEnabled"));
	ASSERT_NE(EnabledProperty, nullptr);
	auto* Body = BodyProperty->ContainerPtrToValuePtr<Durin::FBodyInstance>(Component);
	ASSERT_NE(Body, nullptr);
	ASSERT_EQ(Body->ProfileName, Durin::CollisionProfile::NoCollision);

	Body->CollisionEnabled = Durin::ECollisionEnabled::QueryOnly;
	const std::array Path{
		Durin::FPropertyPathSegment{BodyProperty},
		Durin::FPropertyPathSegment{EnabledProperty}
	};
	Component->PostEditChangeProperty({
		BodyProperty, EnabledProperty, Path, Durin::EPropertyChangePhase::Committed,
		Durin::EPropertyChangeKind::ValueSet, Durin::EPropertyChangeOrigin::Edit
	});

	EXPECT_EQ(Component->GetCollisionEnabled(), Durin::ECollisionEnabled::QueryOnly);
	EXPECT_TRUE(Component->GetCollisionProfileName().IsNone());
	Durin::MarkObjectHierarchyAsGarbage(Actor);
	Durin::CollectGarbage();
}

TEST(FPhysicsPublicContractTests, FreezesCompleteNamesAndReflectionIdentities)
{
	static_assert(std::same_as<decltype(std::declval<Durin::DWorld&>().GetPhysicsScene()), Durin::FPhysicsScene&>);
	static_assert(requires(Durin::DWorld& World, Durin::FHitResult& Hit) {
		World.LineTraceSingleByChannel(Hit, Durin::FVector3(0.0), Durin::FVector3(1.0), Durin::ECollisionChannel::Visibility);
	});
	Durin::Testing::InitializeDObjectSystemForTests();
	EXPECT_EQ(Durin::DBodySetup::StaticClass()->GetQualifiedName().ToString(), "Durin::DBodySetup");
	EXPECT_EQ(Durin::DShapeComponent::StaticClass()->GetQualifiedName().ToString(), "Durin::DShapeComponent");
	EXPECT_EQ(Durin::DBoxComponent::StaticClass()->GetQualifiedName().ToString(), "Durin::DBoxComponent");
	EXPECT_EQ(Durin::DCapsuleComponent::StaticClass()->GetQualifiedName().ToString(), "Durin::DCapsuleComponent");
	EXPECT_NE(Durin::DPrimitiveComponent::StaticClass()->FindPropertyByName("BodyInstance"), nullptr);
	EXPECT_NE(Durin::DCapsuleComponent::StaticClass()->FindPropertyByName("CapsuleRadius"), nullptr);
	EXPECT_NE(Durin::DCapsuleComponent::StaticClass()->FindPropertyByName("CapsuleHalfHeight"), nullptr);
	Durin::FProperty* Dimensions = Durin::DBodySetup::StaticClass()->FindPropertyByName("Dimensions");
	ASSERT_NE(Dimensions, nullptr);
	EXPECT_EQ(Dimensions->GetTypedMetadata().Category, "Shape");
	EXPECT_EQ(Dimensions->GetTypedMetadata().Units, Durin::EPropertyUnit::Meters);
}

TEST(FPhysicsGeometryTests, RaycastsRotatedPositiveScaleBoxes)
{
	Durin::FPhysicsScene Scene;
	Durin::FPhysicsBodyDesc Body = MakeBoxBody(
		{0.0, 0.0, 0.0}, {1.0, 0.5, 0.5},
		Durin::Math::MakeQuaternionFromAxisAngleDegrees(45.0, Durin::FVectorConstants::Up));
	Body.Transform.Scale3D = {2.0, 1.0, 1.0};
	ASSERT_TRUE(Scene.AddBody(Body).IsValid());
	Durin::FPhysicsQueryHit Hit;
	ASSERT_TRUE(Scene.LineTraceSingle({-4.0, 0.0, 0.0}, {4.0, 0.0, 0.0}, {}, Hit));
	EXPECT_GT(Hit.Time, 0.0);
	EXPECT_LT(Hit.Time, 0.5);
	EXPECT_NEAR(Durin::Math::Length(Hit.ImpactNormal), 1.0, 1.e-8);
}

TEST(FPhysicsGeometryTests, SweepsCapsulesAndReportsInitialPenetration)
{
	Durin::FPhysicsScene Scene;
	ASSERT_TRUE(Scene.AddBody(MakeBoxBody({0.0, 0.0, 0.0}, {0.5, 3.0, 3.0})).IsValid());
	Durin::FTransform CapsuleTransform;
	CapsuleTransform.Translation = {-3.0, 0.0, 0.0};
	const Durin::FCollisionShape Capsule = Durin::FCollisionShape::MakeCapsule(0.4, 1.0);
	Durin::FPhysicsQueryHit Hit;
	ASSERT_TRUE(Scene.SweepSingle(Capsule, CapsuleTransform, {6.0, 0.0, 0.0}, {}, Hit));
	EXPECT_NEAR(Hit.Time, 0.35, 2.e-3);
	EXPECT_LT(Hit.ImpactNormal.x, -0.99);
	CapsuleTransform.Translation = {0.0, 0.0, 0.0};
	ASSERT_TRUE(Scene.SweepSingle(Capsule, CapsuleTransform, {1.0, 0.0, 0.0}, {}, Hit));
	EXPECT_TRUE(Hit.bStartPenetrating);
	EXPECT_DOUBLE_EQ(Hit.Time, 0.0);
	EXPECT_GE(Hit.PenetrationDepth, 0.0);
}

TEST(FPhysicsSceneTests, AppliesTwoSidedFiltersIgnoresAndStableTieBreaking)
{
	Durin::FPhysicsScene Scene;
	Durin::FPhysicsBodyDesc First = MakeBoxBody({0.0, 0.0, 0.0});
	Durin::FPhysicsBodyDesc Second = First;
	const Durin::FPhysicsActorHandle FirstHandle = Scene.AddBody(First);
	const Durin::FPhysicsActorHandle SecondHandle = Scene.AddBody(Second);
	ASSERT_TRUE(FirstHandle.IsValid());
	ASSERT_TRUE(SecondHandle.IsValid());
	Durin::FPhysicsQueryHit Hit;
	ASSERT_TRUE(Scene.LineTraceSingle({-2.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, {}, Hit));
	EXPECT_EQ(Hit.ActorHandle, FirstHandle);
	Durin::FPhysicsQueryFilter Ignored;
	Ignored.IgnoredActors.push_back(FirstHandle);
	ASSERT_TRUE(Scene.LineTraceSingle({-2.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, Ignored, Hit));
	EXPECT_EQ(Hit.ActorHandle, SecondHandle);
	Second.Filter.Responses[0] = Durin::EPhysicsQueryResponse::Ignore;
	ASSERT_TRUE(Scene.UpdateBody(SecondHandle, Second));
	EXPECT_FALSE(Scene.LineTraceSingle({-2.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, Ignored, Hit));
	EXPECT_TRUE(Scene.RemoveBody(SecondHandle));
	EXPECT_FALSE(Scene.RemoveBody(SecondHandle));
}

TEST(FPhysicsSceneTests, RejectsOffThreadAndNonFiniteInputWithoutMutation)
{
	Durin::FPhysicsScene Scene;
	const Durin::FPhysicsActorHandle Handle = Scene.AddBody(MakeBoxBody({0.0, 0.0, 0.0}));
	ASSERT_TRUE(Handle.IsValid());
	bool bRemoved = true;
	bool bQueried = true;
	std::thread Worker([&] {
		Durin::FPhysicsQueryHit Hit;
		bRemoved = Scene.RemoveBody(Handle);
		bQueried = Scene.LineTraceSingle({-1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {}, Hit);
	});
	Worker.join();
	EXPECT_FALSE(bRemoved);
	EXPECT_FALSE(bQueried);
	EXPECT_TRUE(Scene.ContainsBody(Handle));
	Durin::FPhysicsQueryHit Hit;
	EXPECT_FALSE(Scene.LineTraceSingle(
		{std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}, {1.0, 0.0, 0.0}, {}, Hit));
	EXPECT_TRUE(Scene.ContainsBody(Handle));
}

TEST(FPhysicsWorldTests, SynchronizesComponentLifecycleTransformAndWorldIsolation)
{
	Durin::DWorld* World = CreatePhysicsWorld();
	Durin::DWorld* OtherWorld = CreatePhysicsWorld();
	Durin::DBoxComponent* Box = AddWorldBox(*World, {0.0, 0.0, 0.0}, {1.0, 1.0, 1.0});
	ASSERT_EQ(World->GetPhysicsScene().GetBodyCount(), 1u);
	EXPECT_EQ(OtherWorld->GetPhysicsScene().GetBodyCount(), 0u);
	Durin::FHitResult Hit;
	ASSERT_TRUE(World->LineTraceSingleByChannel(
		Hit, {-3.0, 0.0, 0.0}, {3.0, 0.0, 0.0}, Durin::ECollisionChannel::Visibility));
	EXPECT_EQ(Hit.Component, Box);
	Box->SetWorldLocation({10.0, 0.0, 0.0});
	EXPECT_FALSE(World->LineTraceSingleByChannel(
		Hit, {-3.0, 0.0, 0.0}, {3.0, 0.0, 0.0}, Durin::ECollisionChannel::Visibility));
	Box->UnregisterComponent();
	EXPECT_EQ(World->GetPhysicsScene().GetBodyCount(), 0u);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::MarkObjectHierarchyAsGarbage(OtherWorld);
	Durin::CollectGarbage();
}

TEST(FPhysicsWorldTests, QueriesRemainAvailableWhenSimulationIsDisabledOrPaused)
{
	Durin::DWorld* World = CreatePhysicsWorld();
	AddWorldBox(*World, {0.0, 0.0, -0.5}, {5.0, 5.0, 0.5});
	World->SetPhysicsSimulationEnabled(false);
	World->SetPaused(true);
	Durin::FHitResult Hit;
	EXPECT_TRUE(World->LineTraceSingleByChannel(
		Hit, {0.0, 0.0, 2.0}, {0.0, 0.0, -2.0}, Durin::ECollisionChannel::Visibility));
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FPhysicsWorldTests, CollisionDebugSnapshotIsBoundedAndDisabledByDefault)
{
	Durin::DWorld* World = CreatePhysicsWorld();
	Durin::DBoxComponent* Box = AddWorldBox(*World, {0.0, 0.0, 0.0}, {1.0, 1.0, 1.0});
	EXPECT_TRUE(World->CaptureCollisionDebugSnapshot().Bodies.empty());
	World->SetCollisionDebugDrawEnabled(true);
	Durin::FHitResult Hit;
	ASSERT_TRUE(World->LineTraceSingleByChannel(
		Hit, {-2.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, Durin::ECollisionChannel::Visibility));
	const Durin::FCollisionDebugSnapshot Snapshot = World->CaptureCollisionDebugSnapshot();
	ASSERT_EQ(Snapshot.Bodies.size(), 1u);
	EXPECT_EQ(Snapshot.Bodies.front().Component, Box);
	EXPECT_TRUE(Snapshot.LastBlockingHit.has_value());
	std::vector<Durin::FVector3> DebugVertices;
	std::vector<uint32> DebugIndices;
	for (uint32 Index = 0; Index < 300; ++Index)
	{
		const uint32 First = static_cast<uint32>(DebugVertices.size());
		const double X = static_cast<double>(Index) * 2.0;
		const double Z = static_cast<double>(Index & 1u);
		DebugVertices.insert(DebugVertices.end(), {{X, 0.0, Z}, {X + 1.0, 0.0, Z}, {X, 1.0, Z}});
		DebugIndices.insert(DebugIndices.end(), {First, First + 1, First + 2});
	}
	Durin::FPhysicsBodyDesc FeatureBody;
	FeatureBody.Geometry = Durin::FCollisionGeometryRef::BuildTriangleMesh(DebugVertices, DebugIndices);
	FeatureBody.Transform = Durin::FTransform();
	FeatureBody.UserToken = reinterpret_cast<uint64>(Box);
	ASSERT_TRUE(World->GetPhysicsScene().AddBody(FeatureBody).IsValid());
	const Durin::FCollisionDebugSnapshot FeatureSnapshot = World->CaptureCollisionDebugSnapshot();
	const auto Feature = std::ranges::find_if(FeatureSnapshot.Bodies, [](const Durin::FCollisionDebugBody& Body) {
		return Body.GeometryKind == Durin::ECollisionGeometryKind::TriangleMesh;
	});
	ASSERT_NE(Feature, FeatureSnapshot.Bodies.end());
	EXPECT_FALSE(Feature->bHasPrimitiveShape);
	EXPECT_EQ(Feature->TotalTriangles, 300u);
	EXPECT_EQ(Feature->TriangleSample.size(), 256u);
	Durin::FPhysicsBodyDesc DebugBody = MakeBoxBody({10.0, 0.0, 0.0});
	DebugBody.UserToken = static_cast<decltype(DebugBody.UserToken)>(reinterpret_cast<std::uintptr_t>(Box));
	for (size_t Index = 0; Index < 4096; ++Index)
	{
		DebugBody.Transform.Translation.x = 10.0 + static_cast<double>(Index);
		ASSERT_TRUE(World->GetPhysicsScene().AddBody(DebugBody).IsValid());
	}
	EXPECT_EQ(World->CaptureCollisionDebugSnapshot().Bodies.size(), 4096u);
	World->SetCollisionDebugDrawEnabled(false);
	EXPECT_TRUE(World->CaptureCollisionDebugSnapshot().Bodies.empty());
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FPhysicsWorldTests, StaticMeshCollisionPolicyRepublishesSharedSceneGeometry)
{
	Durin::FModuleManager::Get().LoadModule("StaticMeshBuild");
	Durin::DWorld* FirstWorld = CreatePhysicsWorld();
	Durin::DWorld* SecondWorld = CreatePhysicsWorld();
	std::string Error;
	Durin::DStaticMesh* Mesh = Durin::NewObject<Durin::DStaticMesh>(FirstWorld, "SceneCollisionMesh");
	Durin::FStaticMeshDecodedGeometry Imported;
	Imported.MaterialSlots.push_back({"Default", 0, "Default"});
	Durin::FStaticMeshImportedMesh& ImportedMesh = Imported.Meshes.emplace_back();
	ImportedMesh.Name = "Tetrahedron";
	ImportedMesh.Positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	ImportedMesh.Indices = {0, 2, 1, 0, 1, 3, 1, 2, 3, 2, 0, 3};
	ImportedMesh.SourceMaterialIndex = 0;
	ASSERT_TRUE(Durin::BuildStaticMeshSynchronously(
		*Mesh, std::move(Imported), Error)) << Error;
	ASSERT_TRUE(Mesh->SetCollisionSourceMode(
		Durin::EBodySetupCollisionSourceMode::TriangleMeshFromLOD0, Error)) << Error;
	auto AddMesh = [&](Durin::DWorld& World, std::string_view Name) {
		auto* Actor = World.SpawnActor<Durin::AStaticMeshActor>(Durin::FName(Name));
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		return Actor->GetStaticMeshComponent();
	};
	Durin::DStaticMeshComponent* First = AddMesh(*FirstWorld, "FirstCollisionOwner");
	Durin::DStaticMeshComponent* Second = AddMesh(*SecondWorld, "SecondCollisionOwner");
	ASSERT_TRUE(First->GetPhysicsActorHandle().IsValid());
	ASSERT_TRUE(Second->GetPhysicsActorHandle().IsValid());
	EXPECT_EQ(First->GetCollisionProfileName(), Durin::CollisionProfile::WorldStatic);
	EXPECT_EQ(First->GetPublishedBodySetupRevision(), Mesh->GetBodySetup()->GetRevision());
	const auto FirstBodies = FirstWorld->GetPhysicsScene().CaptureBodies();
	const auto SecondBodies = SecondWorld->GetPhysicsScene().CaptureBodies();
	ASSERT_EQ(FirstBodies.size(), 1u);
	ASSERT_EQ(SecondBodies.size(), 1u);
	EXPECT_EQ(FirstBodies.front().Desc.Geometry.GetIdentity(), SecondBodies.front().Desc.Geometry.GetIdentity());
	EXPECT_EQ(FirstWorld->GetPhysicsScene().CaptureQueryDiagnostics().Mutations.UniqueGeometryResources, 1u);
	EXPECT_EQ(SecondWorld->GetPhysicsScene().CaptureQueryDiagnostics().Mutations.UniqueGeometryResources, 1u);
	const std::optional<Durin::FBox> Bounds = Mesh->GetLOD0LocalBounds();
	ASSERT_TRUE(Bounds.has_value());
	const Durin::FVector3 Center = Bounds->GetCenter();
	Durin::FHitResult Hit;
	ASSERT_TRUE(FirstWorld->LineTraceSingleByChannel(
		Hit, {Bounds->Min.x - 1.0, Center.y, Center.z},
		{Bounds->Max.x + 1.0, Center.y, Center.z}, Durin::ECollisionChannel::Visibility));
	EXPECT_EQ(Hit.Component, First);

	ASSERT_TRUE(Mesh->SetCollisionQueryPolicy(
		Durin::EBodySetupCollisionQueryPolicy::SimpleOnly, Error)) << Error;
	EXPECT_FALSE(First->GetPhysicsActorHandle().IsValid());
	EXPECT_FALSE(Second->GetPhysicsActorHandle().IsValid());
	EXPECT_EQ(First->GetCollisionProfileName(), Durin::CollisionProfile::WorldStatic);
	ASSERT_TRUE(Mesh->SetCollisionQueryPolicy(
		Durin::EBodySetupCollisionQueryPolicy::ComplexOnly, Error)) << Error;
	EXPECT_TRUE(First->GetPhysicsActorHandle().IsValid());
	EXPECT_TRUE(Second->GetPhysicsActorHandle().IsValid());
	EXPECT_EQ(First->GetPublishedBodySetupRevision(), Mesh->GetBodySetup()->GetRevision());

	First->SetStaticMesh(nullptr);
	EXPECT_FALSE(First->GetPhysicsActorHandle().IsValid());
	EXPECT_TRUE(Second->GetPhysicsActorHandle().IsValid());
	Durin::MarkObjectHierarchyAsGarbage(FirstWorld);
	Durin::MarkObjectHierarchyAsGarbage(SecondWorld);
	Durin::CollectGarbage();
}

TEST(FPhysicsBodySetupTests, SharesAssetGeometryWhileInstancesKeepDistinctHandles)
{
	Durin::DWorld* World = CreatePhysicsWorld();
	auto* Mesh = Durin::NewObject<Durin::DStaticMesh>(World, "SharedCollisionMesh");
	auto* Setup = Durin::NewObject<Durin::DBodySetup>(Mesh, "BodySetup");
	ASSERT_TRUE(Setup->SetBox({0.5, 0.5, 0.5}));
	ASSERT_TRUE(Mesh->SetBodySetup(Setup));
	auto* First = World->SpawnActor<Durin::AStaticMeshActor>("First");
	auto* Second = World->SpawnActor<Durin::AStaticMeshActor>("Second");
	First->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	Second->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	Second->GetStaticMeshComponent()->SetWorldLocation({2.0, 0.0, 0.0});
	EXPECT_EQ(First->GetStaticMeshComponent()->GetPhysicsBodyMotionType(),
		Durin::EPhysicsBodyMotionType::Static);
	EXPECT_EQ(Second->GetStaticMeshComponent()->GetPhysicsBodyMotionType(),
		Durin::EPhysicsBodyMotionType::Static);
	EXPECT_EQ(First->GetStaticMeshComponent()->GetBodySetup(), Setup);
	EXPECT_EQ(Second->GetStaticMeshComponent()->GetBodySetup(), Setup);
	EXPECT_NE(First->GetStaticMeshComponent()->GetPhysicsActorHandle(),
		Second->GetStaticMeshComponent()->GetPhysicsActorHandle());
	EXPECT_EQ(World->GetPhysicsScene().GetBodyCount(), 2u);
	const std::vector<Durin::FPhysicsBodySnapshot> SharedBodies = World->GetPhysicsScene().CaptureBodies();
	ASSERT_EQ(SharedBodies.size(), 2u);
	EXPECT_EQ(SharedBodies[0].Desc.Geometry.GetIdentity(), SharedBodies[1].Desc.Geometry.GetIdentity());
	auto* RenderOnlyOwner = World->SpawnActor<Durin::ACameraActor>("RenderOnlyOwner");
	auto* RenderOnly = Durin::Cast<Durin::DStaticMeshComponent>(
		RenderOnlyOwner->AddInstanceComponent(Durin::DStaticMeshComponent::StaticClass(), "RenderOnly"));
	ASSERT_NE(RenderOnly, nullptr);
	RenderOnly->SetStaticMesh(Mesh);
	EXPECT_EQ(RenderOnly->GetCollisionEnabled(), Durin::ECollisionEnabled::NoCollision);
	EXPECT_FALSE(RenderOnly->GetPhysicsActorHandle().IsValid());
	EXPECT_EQ(World->GetPhysicsScene().GetBodyCount(), 2u);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FPhysicsCollisionGeometryResourceTests, ValidatesIdentityBoundsChildrenAndRetainedBytes)
{
	static_assert(sizeof(Durin::FCollisionGeometryRef) == 16);
	static_assert(sizeof(Durin::FCollisionGeometryChild) == 112);
	EXPECT_FALSE(Durin::FCollisionGeometryRef::MakePrimitive({}).IsValid());
	EXPECT_FALSE(Durin::FCollisionGeometryRef::MakeCompound({}).IsValid());
	std::vector<Durin::FCollisionGeometryChild> Children(65);
	for (Durin::FCollisionGeometryChild& Child : Children)
		Child.Shape = Durin::FCollisionShape::MakeSphere(0.5);
	EXPECT_FALSE(Durin::FCollisionGeometryRef::MakeCompound(Children).IsValid());

	Children.resize(64);
	for (uint32 Index = 0; Index < Children.size(); ++Index)
	{
		Children[Index].LocalTransform.Translation = {static_cast<double>(Index), 0.0, 0.0};
	}
	const Durin::FCollisionGeometryRef Compound = Durin::FCollisionGeometryRef::MakeCompound(Children);
	ASSERT_TRUE(Compound.IsValid());
	EXPECT_NE(Compound.GetIdentity(), 0u);
	EXPECT_EQ(Compound.GetChildCount(), 64u);
	EXPECT_EQ(Compound.GetRetainedBytes(), ExpectedCompoundRetainedBytes);
	Durin::FVector3 Min;
	Durin::FVector3 Max;
	ASSERT_TRUE(Compound.GetLocalBounds(Min, Max));
	EXPECT_DOUBLE_EQ(Min.x, -0.5);
	EXPECT_DOUBLE_EQ(Max.x, 63.5);
	EXPECT_DOUBLE_EQ(Compound.GetChild(17)->LocalTransform.Translation.x, 17.0);

	Children[4].LocalTransform.Scale3D.x = 0.0;
	EXPECT_FALSE(Durin::FCollisionGeometryRef::MakeCompound(Children).IsValid());
}

TEST(FPhysicsBodySetupTests, CachesOneGeometryIdentityPerAuthoredRevision)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	auto* Setup = Durin::NewObject<Durin::DBodySetup>(nullptr, "CachedGeometrySetup");
	ASSERT_TRUE(Setup->SetCapsule(0.5, 1.5, {1.0, 2.0, 3.0}));
	Durin::FCollisionGeometryRef First;
	Durin::FCollisionGeometryRef Second;
	Durin::FTransform FirstTransform;
	Durin::FTransform SecondTransform;
	ASSERT_TRUE(Setup->BuildGeometry(First, FirstTransform));
	ASSERT_TRUE(Setup->BuildGeometry(Second, SecondTransform));
	EXPECT_EQ(First.GetIdentity(), Second.GetIdentity());
	EXPECT_EQ(FirstTransform.Translation, Durin::FVector3(1.0, 2.0, 3.0));
	ASSERT_TRUE(Setup->SetSphere(2.0));
	ASSERT_TRUE(Setup->BuildGeometry(Second, SecondTransform));
	EXPECT_NE(First.GetIdentity(), Second.GetIdentity());
	Durin::MarkObjectHierarchyAsGarbage(Setup);
	Durin::CollectGarbage();
}

TEST(FPhysicsNarrowphaseMatrixTests, ReachesEveryPrimitiveRaySweepAndOverlapCell)
{
	const std::array<Durin::FCollisionShape, 3> Shapes{
		Durin::FCollisionShape::MakeBox({0.75, 0.75, 0.75}),
		Durin::FCollisionShape::MakeSphere(0.75),
		Durin::FCollisionShape::MakeCapsule(0.5, 1.0)};
	for (const Durin::FCollisionShape& TargetShape : Shapes)
	{
		const Durin::FCollisionGeometryRef Target =
			Durin::FCollisionGeometryRef::MakePrimitive(TargetShape);
		Durin::FPhysicsQueryHit Hit;
		EXPECT_EQ(Durin::CollisionGeometry::Raycast(
			{-5.0, 0.0, 0.0}, {5.0, 0.0, 0.0}, Target, Durin::FTransform(),
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Hit),
			Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
		for (const Durin::FCollisionShape& QueryShape : Shapes)
		{
			Durin::FTransform OverlapTransform;
			EXPECT_EQ(Durin::CollisionGeometry::Overlap(
				QueryShape, OverlapTransform, Target, Durin::FTransform(),
				Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Hit),
				Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
			Durin::FTransform SweepTransform;
			SweepTransform.Translation = {-5.0, 0.0, 0.0};
			EXPECT_EQ(Durin::CollisionGeometry::Sweep(
				QueryShape, SweepTransform, {10.0, 0.0, 0.0}, Target, Durin::FTransform(),
				Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Hit),
				Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
			EXPECT_GE(Hit.Time, 0.0);
			EXPECT_LE(Hit.Time, 1.0);
			EXPECT_NEAR(Durin::Math::Length(Hit.ImpactNormal), 1.0, 1.0e-8);
		}
	}
}

TEST(FPhysicsNarrowphaseMatrixTests, CompoundUsesStableChildOrderAndOneBodyResult)
{
	std::array<Durin::FCollisionGeometryChild, 2> Children{};
	Children[0].Shape = Durin::FCollisionShape::MakeSphere(0.5);
	Children[0].LocalTransform.Translation = {2.0, 0.0, 0.0};
	Children[1].Shape = Durin::FCollisionShape::MakeBox({0.5, 0.5, 0.5});
	Children[1].LocalTransform.Translation = {-2.0, 0.0, 0.0};
	Durin::FPhysicsBodyDesc Body;
	Body.Geometry = Durin::FCollisionGeometryRef::MakeCompound(Children);
	Durin::FPhysicsScene Scene;
	ASSERT_TRUE(Scene.AddBody(Body).IsValid());
	Durin::FPhysicsQueryHit Hit;
	ASSERT_TRUE(Scene.LineTraceSingle({-5.0, 0.0, 0.0}, {5.0, 0.0, 0.0}, {}, Hit));
	EXPECT_NEAR(Hit.Time, 0.25, 1.0e-12);
	std::vector<Durin::FPhysicsQueryHit> Hits;
	ASSERT_TRUE(Scene.OverlapMulti(
		Durin::FCollisionShape::MakeBox({3.0, 1.0, 1.0}), Durin::FTransform(), {}, Hits));
	EXPECT_EQ(Hits.size(), 1u);
}

namespace
{
	auto MakeStage1CubeHull() -> Durin::FCollisionGeometryRef
	{
		const std::array<Durin::FVector3, 8> Vertices{
			Durin::FVector3{-1.0, -1.0, -1.0}, Durin::FVector3{1.0, -1.0, -1.0},
			Durin::FVector3{-1.0, 1.0, -1.0}, Durin::FVector3{1.0, 1.0, -1.0},
			Durin::FVector3{-1.0, -1.0, 1.0}, Durin::FVector3{1.0, -1.0, 1.0},
			Durin::FVector3{-1.0, 1.0, 1.0}, Durin::FVector3{1.0, 1.0, 1.0}};
		const std::array<uint32, 36> Indices{
			0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5,
			0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6,
			0, 1, 5, 0, 5, 4, 2, 6, 7, 2, 7, 3};
		return Durin::FCollisionGeometryRef::MakeConvexHull(Vertices, Indices);
	}

	auto MakeStage1TrianglePlane() -> Durin::FCollisionGeometryRef
	{
		const std::array<Durin::FVector3, 4> Vertices{
			Durin::FVector3{0.0, -3.0, -3.0}, Durin::FVector3{0.0, 3.0, -3.0},
			Durin::FVector3{0.0, 3.0, 3.0}, Durin::FVector3{0.0, -3.0, 3.0}};
		const std::array<uint32, 6> Indices{0, 1, 2, 0, 2, 3};
		const std::array<uint32, 2> Ordinals{17, 42};
		return Durin::FCollisionGeometryRef::MakeTriangleMesh(Vertices, Indices, Ordinals);
	}
}

TEST(FPhysicsCollisionGeometryStage1Tests, ValidatesImmutableFeatureResourcesAndStableAccess)
{
	static_assert(sizeof(Durin::FCollisionGeometryRef) == 16);
	static_assert(sizeof(Durin::FCollisionGeometryTriangle) == 16);
	const Durin::FCollisionGeometryRef Hull = MakeStage1CubeHull();
	ASSERT_TRUE(Hull.IsValid());
	EXPECT_EQ(Hull.GetKind(), Durin::ECollisionGeometryKind::ConvexHull);
	EXPECT_EQ(Hull.GetChildCount(), 0u);
	EXPECT_EQ(Hull.GetVertexCount(), 8u);
	EXPECT_EQ(Hull.GetTriangleCount(), 12u);
	ASSERT_NE(Hull.GetTriangle(0), nullptr);
	EXPECT_EQ(Hull.GetTriangle(0)->SourceOrdinal, 0u);
	EXPECT_EQ(*Hull.GetVertex(7), Durin::FVector3(1.0));
	EXPECT_EQ(Hull.GetVertex(8), nullptr);
	EXPECT_EQ(Hull.GetTriangle(12), nullptr);
	Durin::FVector3 Minimum;
	Durin::FVector3 Maximum;
	ASSERT_TRUE(Hull.GetLocalBounds(Minimum, Maximum));
	EXPECT_EQ(Minimum, Durin::FVector3(-1.0));
	EXPECT_EQ(Maximum, Durin::FVector3(1.0));
	EXPECT_GT(Hull.GetIdentity(), 0u);
	EXPECT_GE(Hull.GetRetainedBytes(), 8u * sizeof(Durin::FVector3)
		+ 12u * sizeof(Durin::FCollisionGeometryTriangle));

	const Durin::FCollisionGeometryRef Mesh = MakeStage1TrianglePlane();
	ASSERT_TRUE(Mesh.IsValid());
	EXPECT_EQ(Mesh.GetKind(), Durin::ECollisionGeometryKind::TriangleMesh);
	ASSERT_NE(Mesh.GetTriangle(1), nullptr);
	EXPECT_EQ(Mesh.GetTriangle(0)->SourceOrdinal, 17u);
	EXPECT_EQ(Mesh.GetTriangle(1)->SourceOrdinal, 42u);
	EXPECT_NE(Hull.GetIdentity(), Mesh.GetIdentity());

	const Durin::FCollisionGeometryRef Primitive = Durin::FCollisionGeometryRef::MakePrimitive(
		Durin::FCollisionShape::MakeBox({0.5, 0.5, 0.5}));
	EXPECT_EQ(Primitive.GetKind(), Durin::ECollisionGeometryKind::Primitive);
	EXPECT_EQ(Primitive.GetRetainedBytes(), ExpectedPrimitiveRetainedBytes);
}

TEST(FPhysicsCollisionGeometryStage1Tests, RejectsMalformedFeatureResourcesTransactionally)
{
	const std::array<Durin::FVector3, 4> Tetra{
		Durin::FVector3{0.0, 0.0, 0.0}, Durin::FVector3{1.0, 0.0, 0.0},
		Durin::FVector3{0.0, 1.0, 0.0}, Durin::FVector3{0.0, 0.0, 1.0}};
	const std::array<uint32, 12> Closed{0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
	EXPECT_TRUE(Durin::FCollisionGeometryRef::MakeConvexHull(Tetra, Closed).IsValid());
	EXPECT_FALSE(Durin::FCollisionGeometryRef::MakeConvexHull(Tetra,
		std::span(Closed).first(9)).IsValid());
	std::array<uint32, 12> Inconsistent = Closed;
	std::swap(Inconsistent[1], Inconsistent[2]);
	EXPECT_FALSE(Durin::FCollisionGeometryRef::MakeConvexHull(Tetra, Inconsistent).IsValid());
	std::array<Durin::FVector3, 257> Oversized{};
	EXPECT_FALSE(Durin::FCollisionGeometryRef::MakeConvexHull(Oversized, Closed).IsValid());
	std::array<Durin::FVector3, 3> Degenerate{
		Durin::FVector3{0.0}, Durin::FVector3{1.0, 0.0, 0.0}, Durin::FVector3{2.0, 0.0, 0.0}};
	const std::array<uint32, 3> Triangle{0, 1, 2};
	EXPECT_FALSE(Durin::FCollisionGeometryRef::MakeTriangleMesh(Degenerate, Triangle).IsValid());
	Degenerate[2] = {0.0, 1.0, 0.0};
	const std::array<uint32, 1> WrongOrdinals{1};
	const std::array<uint32, 2> TooManyOrdinals{1, 2};
	EXPECT_TRUE(Durin::FCollisionGeometryRef::MakeTriangleMesh(
		Degenerate, Triangle, WrongOrdinals).IsValid());
	EXPECT_FALSE(Durin::FCollisionGeometryRef::MakeTriangleMesh(
		Degenerate, Triangle, TooManyOrdinals).IsValid());
	Degenerate[0].x = std::numeric_limits<double>::infinity();
	EXPECT_FALSE(Durin::FCollisionGeometryRef::MakeTriangleMesh(Degenerate, Triangle).IsValid());
}

TEST(FPhysicsCollisionGeometryStage1Tests, ReferenceMatrixCoversHullAndTriangleTargets)
{
	const std::array<Durin::FCollisionGeometryRef, 2> Targets{
		MakeStage1CubeHull(), MakeStage1TrianglePlane()};
	const std::array<Durin::FCollisionShape, 3> Queries{
		Durin::FCollisionShape::MakeBox({0.25, 0.25, 0.25}),
		Durin::FCollisionShape::MakeSphere(0.25),
		Durin::FCollisionShape::MakeCapsule(0.25, 0.5)};
	for (uint32 TargetIndex = 0; TargetIndex < Targets.size(); ++TargetIndex)
	{
		ASSERT_TRUE(Targets[TargetIndex].IsValid());
		for (const Durin::FCollisionShape& Query : Queries)
		{
			Durin::CollisionGeometry::FCollisionGeometryCounters Counters;
			Durin::FPhysicsQueryHit Hit;
			const Durin::FVector3 RayStart = TargetIndex == 0
				? Durin::FVector3{-3.0, 0.0, 0.0} : Durin::FVector3{-2.0, 0.0, 0.0};
			const Durin::FVector3 RayEnd = -RayStart;
			EXPECT_EQ(Durin::CollisionGeometry::Raycast(
				RayStart, RayEnd, Targets[TargetIndex], Durin::FTransform(),
				Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Hit, &Counters),
				Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
			EXPECT_NEAR(Hit.Time, TargetIndex == 0 ? 1.0 / 3.0 : 0.5, 1.0e-8);
			EXPECT_TRUE(Durin::Math::IsFinite(Hit.ImpactNormal));

			Durin::FTransform SweepTransform;
			SweepTransform.Translation = RayStart;
			EXPECT_EQ(Durin::CollisionGeometry::Sweep(
				Query, SweepTransform, RayEnd - RayStart, Targets[TargetIndex], Durin::FTransform(),
				Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Hit, &Counters),
				Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
			EXPECT_GT(Hit.Time, 0.0);
			EXPECT_LT(Hit.Time, 1.0);
			EXPECT_NEAR(Durin::Math::Length(Hit.ImpactNormal), 1.0, 1.0e-8);

			Durin::FTransform OverlapTransform;
			OverlapTransform.Translation = TargetIndex == 0
				? Durin::FVector3{-0.9, 0.0, 0.0} : Durin::FVector3{-0.1, 0.0, 0.0};
			EXPECT_EQ(Durin::CollisionGeometry::Overlap(
				Query, OverlapTransform, Targets[TargetIndex], Durin::FTransform(),
				Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Hit, &Counters),
				Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
			EXPECT_TRUE(Hit.bStartPenetrating);
			EXPECT_GT(Hit.PenetrationDepth, 0.0);
			EXPECT_GT(Counters.FeatureTests, 0u);
			EXPECT_EQ(Counters.Unsupported, 0u);
			EXPECT_EQ(Counters.NonConverged, 0u);
			EXPECT_FALSE(Counters.bOverflowed);
		}
	}
}

TEST(FPhysicsCollisionGeometryStage1Tests, AppliesRandomizedPositiveTransformsWithStableReferenceProductionParity)
{
	const Durin::FCollisionGeometryRef Target = MakeStage1CubeHull();
	std::mt19937_64 Random(0x5341474531ull);
	std::uniform_real_distribution<double> Position(-20.0, 20.0);
	std::uniform_real_distribution<double> Scale(0.25, 3.0);
	std::uniform_real_distribution<double> Angle(-180.0, 180.0);
	for (uint32 Iteration = 0; Iteration < 128; ++Iteration)
	{
		Durin::FTransform Transform;
		Transform.Translation = {Position(Random), Position(Random), Position(Random)};
		Transform.Scale3D = {Scale(Random), Scale(Random), Scale(Random)};
		Transform.Rotation = Durin::Math::MakeQuaternionFromAxisAngleDegrees(
			Angle(Random), Durin::Math::NormalizeOr(
				Durin::FVector3{Position(Random), Position(Random), Position(Random)},
				Durin::FVectorConstants::Up));
		const Durin::FVector3 Axis = Durin::Math::RotateVector(
			Transform.Rotation, Durin::FVectorConstants::Forward);
		const Durin::FVector3 Start = Transform.Translation - Axis * (Transform.Scale3D.x + 2.0);
		const Durin::FVector3 End = Transform.Translation + Axis * (Transform.Scale3D.x + 2.0);
		Durin::FPhysicsQueryHit Reference;
		Durin::FPhysicsQueryHit Production;
		ASSERT_EQ(Durin::CollisionGeometry::Raycast(Start, End, Target, Transform,
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Reference),
			Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
		ASSERT_EQ(Durin::CollisionGeometry::Raycast(Start, End, Target, Transform,
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Production),
			Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
		EXPECT_DOUBLE_EQ(Reference.Time, Production.Time);
		EXPECT_EQ(Reference.ImpactNormal, Production.ImpactNormal);
		EXPECT_TRUE(Durin::Math::IsFinite(Reference.Location));
	}
}

TEST(FPhysicsCollisionGeometryStage2Tests, BuildsDeterministicHullAndMeshTopology)
{
	static_assert(sizeof(Durin::FCollisionGeometryNode) == 32);
	static_assert(sizeof(Durin::FCollisionHullPlane) == 16);
	static_assert(sizeof(Durin::FCollisionHullHalfEdge) == 16);
	static_assert(sizeof(Durin::FCollisionHullFace) == 16);
	std::array<Durin::FVector3, 10> Points{
		Durin::FVector3{-1.0, -1.0, -1.0}, Durin::FVector3{1.0, -1.0, -1.0},
		Durin::FVector3{-1.0, 1.0, -1.0}, Durin::FVector3{1.0, 1.0, -1.0},
		Durin::FVector3{-1.0, -1.0, 1.0}, Durin::FVector3{1.0, -1.0, 1.0},
		Durin::FVector3{-1.0, 1.0, 1.0}, Durin::FVector3{1.0, 1.0, 1.0},
		Durin::FVector3{0.0, 0.0, 0.0}, Durin::FVector3{1.0, 1.0, 1.0}};
	Durin::FCollisionGeometryBuildDiagnostics FirstFacts;
	const Durin::FCollisionGeometryRef First =
		Durin::FCollisionGeometryRef::BuildConvexHull(Points, &FirstFacts);
	ASSERT_TRUE(First.IsValid());
	EXPECT_EQ(FirstFacts.Status, Durin::ECollisionGeometryBuildStatus::Success);
	EXPECT_EQ(FirstFacts.RetainedVertices, 8u);
	EXPECT_EQ(First.GetVertexCount(), 8u);
	EXPECT_EQ(First.GetTriangleCount(), 12u);
	EXPECT_EQ(First.GetHullPlaneCount(), 12u);
	EXPECT_EQ(First.GetHullFaceCount(), 12u);
	EXPECT_EQ(First.GetHullHalfEdgeCount(), 36u);
	for (uint32 EdgeIndex = 0; EdgeIndex < First.GetHullHalfEdgeCount(); ++EdgeIndex)
	{
		const Durin::FCollisionHullHalfEdge* Edge = First.GetHullHalfEdge(EdgeIndex);
		ASSERT_NE(Edge, nullptr);
		ASSERT_LT(Edge->Twin, First.GetHullHalfEdgeCount());
		EXPECT_EQ(First.GetHullHalfEdge(Edge->Twin)->Twin, EdgeIndex);
		EXPECT_LT(Edge->Next, First.GetHullHalfEdgeCount());
		EXPECT_LT(Edge->Face, First.GetHullFaceCount());
	}
	std::reverse(Points.begin(), Points.end());
	Durin::FCollisionGeometryBuildDiagnostics SecondFacts;
	const Durin::FCollisionGeometryRef Second =
		Durin::FCollisionGeometryRef::BuildConvexHull(Points, &SecondFacts);
	ASSERT_TRUE(Second.IsValid());
	ASSERT_EQ(First.GetVertexCount(), Second.GetVertexCount());
	ASSERT_EQ(First.GetTriangleCount(), Second.GetTriangleCount());
	for (uint32 Index = 0; Index < First.GetVertexCount(); ++Index)
		EXPECT_EQ(*First.GetVertex(Index), *Second.GetVertex(Index));
	for (uint32 Index = 0; Index < First.GetTriangleCount(); ++Index)
	{
		ASSERT_NE(First.GetTriangle(Index), nullptr);
		ASSERT_NE(Second.GetTriangle(Index), nullptr);
		EXPECT_EQ(First.GetTriangle(Index)->First, Second.GetTriangle(Index)->First);
		EXPECT_EQ(First.GetTriangle(Index)->Second, Second.GetTriangle(Index)->Second);
		EXPECT_EQ(First.GetTriangle(Index)->Third, Second.GetTriangle(Index)->Third);
	}

	const std::array<Durin::FVector3, 4> MeshVertices{
		Durin::FVector3{0.0, 0.0, 0.0}, Durin::FVector3{1.0, 0.0, 0.0},
		Durin::FVector3{0.0, 1.0, 0.0}, Durin::FVector3{2.0, 0.0, 0.0}};
	const std::array<uint32, 12> DirtyIndices{
		0, 1, 2, 2, 1, 0, 0, 1, 3, 0, 0, 1};
	Durin::FCollisionGeometryBuildDiagnostics MeshFacts;
	const Durin::FCollisionGeometryRef Mesh = Durin::FCollisionGeometryRef::BuildTriangleMesh(
		MeshVertices, DirtyIndices, &MeshFacts);
	ASSERT_TRUE(Mesh.IsValid());
	EXPECT_EQ(MeshFacts.SourceTriangles, 4u);
	EXPECT_EQ(MeshFacts.RetainedTriangles, 1u);
	EXPECT_EQ(MeshFacts.RemovedTriangles, 3u);
	EXPECT_EQ(Mesh.GetTriangle(0)->SourceOrdinal, 0u);
	EXPECT_EQ(Mesh.GetNodeCount(), 1u);
	EXPECT_TRUE(Mesh.GetNode(0)->IsLeaf());
	EXPECT_EQ(Mesh.GetNode(0)->GetLeafCount(), 1u);
	EXPECT_GE(MeshFacts.EstimatedPeakBytes, MeshFacts.RetainedBytes);
	Durin::FCollisionGeometryBuildDiagnostics RepeatFacts;
	const Durin::FCollisionGeometryRef Repeat = Durin::FCollisionGeometryRef::BuildTriangleMesh(
		MeshVertices, DirtyIndices, &RepeatFacts);
	ASSERT_TRUE(Repeat.IsValid());
	ASSERT_EQ(Mesh.GetNodeCount(), Repeat.GetNodeCount());
	ASSERT_EQ(Mesh.GetLeafTriangleCount(), Repeat.GetLeafTriangleCount());
	for (uint32 Index = 0; Index < Mesh.GetNodeCount(); ++Index)
	{
		EXPECT_EQ(Mesh.GetNode(Index)->Minimum, Repeat.GetNode(Index)->Minimum);
		EXPECT_EQ(Mesh.GetNode(Index)->Maximum, Repeat.GetNode(Index)->Maximum);
		EXPECT_EQ(Mesh.GetNode(Index)->First, Repeat.GetNode(Index)->First);
		EXPECT_EQ(Mesh.GetNode(Index)->CountOrSecond, Repeat.GetNode(Index)->CountOrSecond);
	}
	for (uint32 Index = 0; Index < Mesh.GetLeafTriangleCount(); ++Index)
		EXPECT_EQ(Mesh.GetLeafTriangle(Index), Repeat.GetLeafTriangle(Index));
}

TEST(FPhysicsCollisionGeometryStage2Tests, ReportsTransactionalBuilderFailures)
{
	Durin::FCollisionGeometryBuildDiagnostics Facts;
	const std::array<Durin::FVector3, 3> Collinear{
		Durin::FVector3{0.0}, Durin::FVector3{1.0, 0.0, 0.0}, Durin::FVector3{2.0, 0.0, 0.0}};
	EXPECT_FALSE(Durin::FCollisionGeometryRef::BuildConvexHull(Collinear, &Facts).IsValid());
	EXPECT_EQ(Facts.Status, Durin::ECollisionGeometryBuildStatus::InvalidInput);
	std::array<Durin::FVector3, 257> Oversized{};
	EXPECT_FALSE(Durin::FCollisionGeometryRef::BuildConvexHull(Oversized, &Facts).IsValid());
	EXPECT_EQ(Facts.Status, Durin::ECollisionGeometryBuildStatus::LimitExceeded);
	const std::array<uint32, 3> Triangle{0, 1, 2};
	EXPECT_FALSE(Durin::FCollisionGeometryRef::BuildTriangleMesh(Collinear, Triangle, &Facts).IsValid());
	EXPECT_EQ(Facts.Status, Durin::ECollisionGeometryBuildStatus::EmptyAfterCleanup);
	const std::array<uint32, 3> Invalid{0, 1, 99};
	EXPECT_FALSE(Durin::FCollisionGeometryRef::BuildTriangleMesh(Collinear, Invalid, &Facts).IsValid());
	EXPECT_EQ(Facts.Status, Durin::ECollisionGeometryBuildStatus::InvalidInput);
}

TEST(FPhysicsCollisionGeometryStage2Tests, ProductionSweepAndOverlapMatchReferenceWithoutFallback)
{
	const std::array<Durin::FVector3, 4> Vertices{
		Durin::FVector3{0.0, -3.0, -3.0}, Durin::FVector3{0.0, 3.0, -3.0},
		Durin::FVector3{0.0, 3.0, 3.0}, Durin::FVector3{0.0, -3.0, 3.0}};
	const std::array<uint32, 6> Indices{0, 1, 2, 0, 2, 3};
	const Durin::FCollisionGeometryRef Mesh =
		Durin::FCollisionGeometryRef::BuildTriangleMesh(Vertices, Indices);
	ASSERT_TRUE(Mesh.IsValid());
	const std::array<Durin::FCollisionShape, 3> Queries{
		Durin::FCollisionShape::MakeBox({0.25, 0.25, 0.25}),
		Durin::FCollisionShape::MakeSphere(0.25),
		Durin::FCollisionShape::MakeCapsule(0.25, 0.5)};
	for (const Durin::FCollisionShape& Query : Queries)
	{
		Durin::FTransform Start;
		Start.Translation = {-2.0, 0.0, 0.0};
		Durin::FPhysicsQueryHit Reference;
		Durin::FPhysicsQueryHit Production;
		Durin::CollisionGeometry::FCollisionGeometryCounters Work;
		ASSERT_EQ(Durin::CollisionGeometry::Sweep(Query, Start, {4.0, 0.0, 0.0},
			Mesh, Durin::FTransform(), Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference,
			Reference), Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
		ASSERT_EQ(Durin::CollisionGeometry::Sweep(Query, Start, {4.0, 0.0, 0.0},
			Mesh, Durin::FTransform(), Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production,
			Production, &Work), Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
		EXPECT_NEAR(Reference.Time, Production.Time, 1.0e-10);
		EXPECT_EQ(Work.ReferenceFallbacks, 0u);
		EXPECT_GT(Work.AssetNodeTests, 0u);
		Start.Translation = {-0.1, 0.0, 0.0};
		ASSERT_EQ(Durin::CollisionGeometry::Overlap(Query, Start, Mesh, Durin::FTransform(),
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Reference),
			Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
		ASSERT_EQ(Durin::CollisionGeometry::Overlap(Query, Start, Mesh, Durin::FTransform(),
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Production, &Work),
			Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
		EXPECT_NEAR(Reference.PenetrationDepth, Production.PenetrationDepth, 1.0e-10);
	}
}
