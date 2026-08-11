#include "Actors/CameraActor.h"
#include "Actors/StaticMeshActor.h"
#include "Collision/CollisionGeometry.h"
#include "Components/ShapeComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Math/Operations.h"
#include "NativeDObjectTestSupport.h"
#include "Physics/BodySetup.h"
#include "Physics/PhysicsScene.h"
#include "StaticMesh/StaticMesh.h"

#include <gtest/gtest.h>

namespace
{
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
}

TEST(FAetherGeometryTests, RaycastsRotatedPositiveScaleBoxes)
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

TEST(FAetherGeometryTests, SweepsCapsulesAndReportsInitialPenetration)
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

TEST(FAetherSceneTests, AppliesTwoSidedFiltersIgnoresAndStableTieBreaking)
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

TEST(FAetherSceneTests, RejectsOffThreadAndNonFiniteInputWithoutMutation)
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

TEST(FAetherCollisionGeometryResourceTests, ValidatesIdentityBoundsChildrenAndRetainedBytes)
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
	for (Durin::uint32 Index = 0; Index < Children.size(); ++Index)
	{
		Children[Index].LocalTransform.Translation = {static_cast<double>(Index), 0.0, 0.0};
	}
	const Durin::FCollisionGeometryRef Compound = Durin::FCollisionGeometryRef::MakeCompound(Children);
	ASSERT_TRUE(Compound.IsValid());
	EXPECT_NE(Compound.GetIdentity(), 0u);
	EXPECT_EQ(Compound.GetChildCount(), 64u);
	EXPECT_EQ(Compound.GetRetainedBytes(), 7'264u);
	Durin::FVector3 Min;
	Durin::FVector3 Max;
	ASSERT_TRUE(Compound.GetLocalBounds(Min, Max));
	EXPECT_DOUBLE_EQ(Min.x, -0.5);
	EXPECT_DOUBLE_EQ(Max.x, 63.5);
	EXPECT_DOUBLE_EQ(Compound.GetChild(17)->LocalTransform.Translation.x, 17.0);

	Children[4].LocalTransform.Scale3D.x = 0.0;
	EXPECT_FALSE(Durin::FCollisionGeometryRef::MakeCompound(Children).IsValid());
}

TEST(FAetherCollisionGeometryResourceTests, TenThousandBodiesRetainOneSharedPayload)
{
	const Durin::FCollisionGeometryRef Geometry = Durin::FCollisionGeometryRef::MakePrimitive(
		Durin::FCollisionShape::MakeBox({0.5, 0.5, 0.5}));
	EXPECT_EQ(Geometry.GetRetainedBytes(), 208u);
	Durin::FPhysicsScene Scene;
	std::vector<Durin::FPhysicsActorHandle> Handles;
	Handles.reserve(10'000);
	for (Durin::uint32 Index = 0; Index < 10'000; ++Index)
	{
		Durin::FPhysicsBodyDesc Desc;
		Desc.Geometry = Geometry;
		Desc.Transform.Translation = {static_cast<double>(Index) * 2.0, 0.0, 0.0};
		Handles.push_back(Scene.AddBody(Desc));
		ASSERT_TRUE(Handles.back().IsValid());
	}
	Durin::FPhysicsSceneQueryDiagnostics Diagnostics = Scene.CaptureQueryDiagnostics();
	EXPECT_EQ(Diagnostics.Mutations.UniqueGeometryResources, 1u);
	EXPECT_EQ(Diagnostics.Mutations.RetainedGeometryBytes, Geometry.GetRetainedBytes());
	ASSERT_TRUE(Scene.ResetQueryDiagnostics());
	Diagnostics = Scene.CaptureQueryDiagnostics();
	EXPECT_EQ(Diagnostics.Mutations.UniqueGeometryResources, 1u);
	for (const Durin::FPhysicsActorHandle Handle : Handles) ASSERT_TRUE(Scene.RemoveBody(Handle));
	Diagnostics = Scene.CaptureQueryDiagnostics();
	EXPECT_EQ(Diagnostics.Mutations.UniqueGeometryResources, 0u);
	EXPECT_EQ(Diagnostics.Mutations.RetainedGeometryBytes, 0u);
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

TEST(FAetherNarrowphaseMatrixTests, ReachesEveryPrimitiveRaySweepAndOverlapCell)
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

TEST(FAetherNarrowphaseMatrixTests, CompoundUsesStableChildOrderAndOneBodyResult)
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
