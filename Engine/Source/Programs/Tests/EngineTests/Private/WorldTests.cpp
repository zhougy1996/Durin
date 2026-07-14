#include "Actors/CameraActor.h"
#include "Actors/DirectionalLightActor.h"
#include "AssetSystem.h"
#include "Actors/StaticMeshActor.h"
#include "Components/ActorComponent.h"
#include "Components/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Engine.h"
#include "Engine/GameEngine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineTestSupport.h"
#include "Misc/Paths.h"
#include "Threading/RunnableThread.h"

#include <gtest/gtest.h>

namespace
{
	auto CreateEmptyWorld(Durin::DObject* Outer = nullptr) -> Durin::DWorld*
	{
		InitializeDObjectSystem();
		return Durin::NewObject<Durin::DWorld>(Outer, "TestWorld");
	}

	auto CreateWorld(Durin::DObject* Outer = nullptr) -> Durin::DWorld*
	{
		Durin::DWorld* World = CreateEmptyWorld(Outer);
		EXPECT_TRUE(World->SetCurrentLevel(Durin::NewObject<Durin::DLevel>(World, "TestLevel")));
		return World;
	}

	auto ExpectVectorNear(const Durin::FVector3& Actual, const Durin::FVector3& Expected, double Tolerance = 1.e-8) -> void
	{
		EXPECT_NEAR(Actual.x, Expected.x, Tolerance);
		EXPECT_NEAR(Actual.y, Expected.y, Tolerance);
		EXPECT_NEAR(Actual.z, Expected.z, Tolerance);
	}
} // namespace

TEST(FWorldTests, StartsWithoutALevelAndActorOperationsAreSafe)
{
	Durin::DWorld* World = CreateEmptyWorld();
	EXPECT_EQ(World->GetCurrentLevel(), nullptr);
	EXPECT_EQ(World->SpawnActor<Durin::ACameraActor>("Camera"), nullptr);
	EXPECT_TRUE(World->GetActors().empty());
	EXPECT_FALSE(World->ContainsActor(nullptr));
	EXPECT_EQ(World->FindActorByName("Camera"), nullptr);
	EXPECT_FALSE(World->DestroyActor(nullptr));
	World->DestroyAllActors();
	Durin::DestroyObject(World);
}

TEST(FWorldTests, SpawnsEnumeratesAndFindsActors)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ACameraActor* Camera = World->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::AStaticMeshActor* Mesh = World->SpawnActor<Durin::AStaticMeshActor>("Mesh");

	ASSERT_NE(Camera, nullptr);
	ASSERT_NE(Mesh, nullptr);
	ASSERT_EQ(Camera->GetClass(), Durin::ACameraActor::StaticClass());
	ASSERT_EQ(Mesh->GetClass(), Durin::AStaticMeshActor::StaticClass());
	ASSERT_FALSE(Camera->GetClass()->GetFName().IsNone());
	ASSERT_FALSE(Mesh->GetClass()->GetFName().IsNone());
	EXPECT_EQ(Camera->GetClass()->GetName(), "Durin::ACameraActor");
	EXPECT_EQ(Mesh->GetClass()->GetName(), "Durin::AStaticMeshActor");
	EXPECT_EQ(World->GetActors().size(), 2u);
	EXPECT_TRUE(World->ContainsActor(Camera));
	EXPECT_EQ(World->FindActorByName("Camera"), Camera);
	EXPECT_EQ(Camera->GetOuter(), World->GetCurrentLevel());
	EXPECT_EQ(Mesh->GetOuter(), World->GetCurrentLevel());
	EXPECT_EQ(Camera->GetCameraComponent()->GetOuter(), Camera);

	Durin::DestroyObject(World);
}

TEST(FWorldTests, ReflectedClassNamesSeparateIdentityDisplayAndObjectDefaults)
{
	InitializeDObjectSystem();
	Durin::DClass* StaticMeshClass = Durin::AStaticMeshActor::StaticClass();
	EXPECT_EQ(StaticMeshClass->GetQualifiedName().ToString(), "Durin::AStaticMeshActor");
	EXPECT_EQ(StaticMeshClass->GetShortName(), "AStaticMeshActor");
	EXPECT_EQ(StaticMeshClass->GetDisplayName(), "Static Mesh Actor");
	EXPECT_EQ(StaticMeshClass->GetDefaultObjectName(), "StaticMeshActor");

	Durin::DClass* CameraClass = Durin::ACameraActor::StaticClass();
	EXPECT_EQ(CameraClass->GetDisplayName(), "Camera Actor");
	EXPECT_EQ(CameraClass->GetDefaultObjectName(), "CameraActor");

	Durin::DClass* ComponentClass = Durin::DSceneComponent::StaticClass();
	EXPECT_EQ(ComponentClass->GetDisplayName(), "Scene Component");
	EXPECT_EQ(ComponentClass->GetDefaultObjectName(), "SceneComponent");
}

TEST(FWorldTests, UsesReflectedDefaultNamesWhenNamesAreOmitted)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ACameraActor* First = World->SpawnActor<Durin::ACameraActor>();
	Durin::ACameraActor* Second = World->SpawnActor<Durin::ACameraActor>();
	ASSERT_NE(First, nullptr);
	ASSERT_NE(Second, nullptr);
	EXPECT_EQ(First->GetName(), "CameraActor");
	EXPECT_EQ(Second->GetName(), "CameraActor_2");

	Durin::DActorComponent* Component = First->AddInstanceComponent(Durin::DSceneComponent::StaticClass());
	ASSERT_NE(Component, nullptr);
	EXPECT_EQ(Component->GetName(), "SceneComponent");
	Durin::DestroyObject(World);
}

TEST(FWorldTests, MakesDuplicateActorNamesUnique)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ACameraActor* First = World->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::ACameraActor* Second = World->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::ACameraActor* Third = World->SpawnActor<Durin::ACameraActor>("Camera");

	EXPECT_EQ(First->GetName(), "Camera");
	EXPECT_EQ(Second->GetName(), "Camera_2");
	EXPECT_EQ(Third->GetName(), "Camera_3");

	Durin::DestroyObject(World);
}

TEST(FWorldTests, RenamesActorsWithUniqueNames)
{
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* First = World->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::AActor* Second = World->SpawnActor<Durin::ACameraActor>("Other");
	ASSERT_NE(First, nullptr);
	ASSERT_NE(Second, nullptr);
	EXPECT_TRUE(World->GetCurrentLevel()->RenameActor(Second, "Camera"));
	EXPECT_EQ(Second->GetName(), "Camera_2");
	EXPECT_FALSE(World->GetCurrentLevel()->RenameActor(Second, Durin::FName()));
	EXPECT_EQ(Second->GetName(), "Camera_2");
	Durin::DestroyObject(World);
}

TEST(FWorldTests, SpawnsActorsAndComponentsFromRuntimeClasses)
{
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* First = World->SpawnActor(Durin::ACameraActor::StaticClass(), "RuntimeCamera");
	Durin::AActor* Second = World->SpawnActor(Durin::ACameraActor::StaticClass(), "RuntimeCamera");
	ASSERT_NE(First, nullptr);
	ASSERT_NE(Second, nullptr);
	EXPECT_TRUE(First->IsA<Durin::ACameraActor>());
	EXPECT_EQ(Second->GetName(), "RuntimeCamera_2");
	EXPECT_EQ(World->SpawnActor(Durin::DSceneComponent::StaticClass(), "InvalidActor"), nullptr);

	Durin::DActorComponent* Instance = First->AddInstanceComponent(Durin::DSceneComponent::StaticClass(), "ExtraScene");
	ASSERT_NE(Instance, nullptr);
	EXPECT_TRUE(First->IsInstanceComponent(Instance));
	EXPECT_EQ(Instance->GetOwner(), First);
	EXPECT_EQ(Instance->GetOuter(), First);
	EXPECT_TRUE(Instance->IsRegistered());
	auto* SceneInstance = static_cast<Durin::DSceneComponent*>(Instance);
	EXPECT_EQ(SceneInstance->GetAttachParent(), First->GetRootComponent());
	EXPECT_EQ(First->AddInstanceComponent(Durin::ACameraActor::StaticClass(), "InvalidComponent"), nullptr);
	EXPECT_FALSE(First->DestroyInstanceComponent(static_cast<Durin::ACameraActor*>(First)->GetCameraComponent()));

	Durin::TObjectPtr<Durin::DActorComponent> InstancePtr = Instance;
	EXPECT_TRUE(First->DestroyInstanceComponent(Instance));
	EXPECT_EQ(InstancePtr.Get(), Instance);
	EXPECT_FALSE(InstancePtr.IsValid());
	EXPECT_FALSE(Durin::IsValid(Instance));
	EXPECT_TRUE(Instance->IsPendingKill());
	EXPECT_FALSE(Instance->IsRegistered());
	EXPECT_FALSE(First->IsInstanceComponent(Instance));
	Durin::DestroyObject(World);
	EXPECT_EQ(InstancePtr.Get(), nullptr);
}

TEST(FWorldTests, RuntimeClassConstructionRejectsInvalidClassMetadata)
{
	InitializeDObjectSystem();
	auto ConstructActor = [](const Durin::FObjectInitializer& Initializer) { new (Initializer.GetObj()) Durin::AActor(Initializer); };
	Durin::DClass AbstractActor(
		Durin::EC_StaticConstructor, "AbstractActor", sizeof(Durin::AActor), alignof(Durin::AActor), Durin::EObjectFlags::Transient,
		Durin::EClassFlags::Abstract, Durin::EClassCastFlags::DClass, ConstructActor
	);
	AbstractActor.SetSuperStructBase(Durin::AActor::StaticClass());
	EXPECT_FALSE(Durin::CanConstructObjectOfClass(&AbstractActor, Durin::AActor::StaticClass()));

	Durin::DClass MissingConstructor(
		Durin::EC_StaticConstructor, "MissingConstructor", sizeof(Durin::AActor), alignof(Durin::AActor), Durin::EObjectFlags::Transient,
		Durin::EClassFlags::None, Durin::EClassCastFlags::DClass, nullptr
	);
	MissingConstructor.SetSuperStructBase(Durin::AActor::StaticClass());
	EXPECT_FALSE(Durin::CanConstructObjectOfClass(&MissingConstructor, Durin::AActor::StaticClass()));
	EXPECT_FALSE(Durin::CanConstructObjectOfClass(Durin::DSceneComponent::StaticClass(), Durin::AActor::StaticClass()));

	const std::vector<Durin::DClass*> ActorClasses = Durin::GetDerivedClasses(Durin::AActor::StaticClass(), true);
	EXPECT_NE(std::ranges::find(ActorClasses, Durin::ACameraActor::StaticClass()), ActorClasses.end());
	EXPECT_NE(std::ranges::find(ActorClasses, Durin::AStaticMeshActor::StaticClass()), ActorClasses.end());
	EXPECT_TRUE(std::ranges::is_sorted(ActorClasses, [](const Durin::DClass* Left, const Durin::DClass* Right) {
		return Left->GetQualifiedName().ToString() < Right->GetQualifiedName().ToString();
	}));
}

TEST(FWorldTests, MaintainsPrimaryCameraWhenRuntimeActorsChange)
{
	Durin::DWorld* World = CreateWorld();
	auto* First = static_cast<Durin::ACameraActor*>(World->SpawnActor(Durin::ACameraActor::StaticClass(), "First"));
	auto* Second = static_cast<Durin::ACameraActor*>(World->SpawnActor(Durin::ACameraActor::StaticClass(), "Second"));
	ASSERT_NE(First, nullptr);
	ASSERT_NE(Second, nullptr);
	EXPECT_EQ(World->GetCurrentLevel()->GetPrimaryCameraActor(), First);
	EXPECT_TRUE(World->DestroyActor(First));
	EXPECT_EQ(World->GetCurrentLevel()->GetPrimaryCameraActor(), Second);
	EXPECT_TRUE(World->DestroyActor(Second));
	EXPECT_EQ(World->GetCurrentLevel()->GetPrimaryCameraActor(), nullptr);
	Durin::DestroyObject(World);
}

TEST(FLevelAssetTests, SavesLoadsTransformsAttachmentsCameraAndDefaultComponents)
{
	InitializeDObjectSystem();
	static const bool bMountInitialized = [] {
		const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "Levels";
		std::filesystem::remove_all(Root);
		Durin::PathUtilities::RegisterMountPoint("/LevelTests/", Root.generic_string() + "/");
		return true;
	}();
	(void)bMountInitialized;

	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/LevelTests/TransformRoundTrip", Path));
	Durin::DLevel* Level = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Level));
	Durin::ACameraActor* ParentActor = Level->SpawnActor<Durin::ACameraActor>("ParentCamera");
	Durin::ACameraActor* ChildActor = Level->SpawnActor<Durin::ACameraActor>("ChildCamera");
	Durin::DActorComponent* ExtraComponent = ChildActor->AddInstanceComponent(Durin::DSceneComponent::StaticClass(), "ExtraScene");
	ASSERT_NE(ExtraComponent, nullptr);
	ASSERT_TRUE(Level->SetPrimaryCameraActor(ParentActor));

	Durin::FTransform ParentTransform;
	ParentTransform.Translation = {10.0, 20.0, 30.0};
	ParentTransform.Scale3D = {2.0, 2.0, 2.0};
	ParentActor->SetActorTransform(ParentTransform);
	ASSERT_TRUE(ChildActor->AttachToActor(ParentActor, Durin::EAttachmentTransformRule::KeepRelative));
	Durin::FTransform ChildRelative;
	ChildRelative.Translation = {1.0, 2.0, 3.0};
	ChildActor->GetRootComponent()->SetRelativeTransform(ChildRelative);
	ParentActor->GetCameraComponent()->SetFieldOfViewDegrees(75.0f);
	ParentActor->GetCameraComponent()->SetNearClip(0.25f);
	ParentActor->GetCameraComponent()->SetFarClip(2500.0f);

	ASSERT_TRUE(Durin::Asset::SavePackage(Level->GetPackage()));
	ASSERT_FALSE(Level->GetPackage()->IsDirty());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));

	Durin::DLevel* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Path, Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->GetActors().size(), 2u);
	auto* LoadedParent = dynamic_cast<Durin::ACameraActor*>(Loaded->FindActorByName("ParentCamera"));
	auto* LoadedChild = dynamic_cast<Durin::ACameraActor*>(Loaded->FindActorByName("ChildCamera"));
	ASSERT_NE(LoadedParent, nullptr);
	ASSERT_NE(LoadedChild, nullptr);
	EXPECT_EQ(Loaded->GetPrimaryCameraActor(), LoadedParent);
	EXPECT_EQ(LoadedParent->GetOwnedComponents().size(), 1u);
	EXPECT_EQ(LoadedChild->GetOwnedComponents().size(), 2u);
	ASSERT_EQ(LoadedChild->GetInstanceComponents().size(), 1u);
	auto* LoadedExtraComponent = dynamic_cast<Durin::DSceneComponent*>(LoadedChild->GetInstanceComponents().front().Get());
	ASSERT_NE(LoadedExtraComponent, nullptr);
	EXPECT_EQ(LoadedExtraComponent->GetAttachParent(), LoadedChild->GetRootComponent());
	ExpectVectorNear(LoadedParent->GetActorTransform().Translation, ParentTransform.Translation);
	ExpectVectorNear(LoadedChild->GetRootComponent()->GetRelativeTransform().Translation, ChildRelative.Translation);
	EXPECT_EQ(LoadedChild->GetAttachParentActor(), LoadedParent);
	EXPECT_NEAR(LoadedParent->GetCameraComponent()->GetFieldOfViewDegrees(), 75.0f, 1.e-6f);
	EXPECT_NEAR(LoadedParent->GetCameraComponent()->GetNearClip(), 0.25f, 1.e-6f);
	EXPECT_NEAR(LoadedParent->GetCameraComponent()->GetFarClip(), 2500.0f, 1.e-6f);

	Durin::DWorld* World = CreateWorld();
	ASSERT_TRUE(World->SetCurrentLevel(Loaded));
	EXPECT_EQ(World->GetActors().size(), 2u);
	EXPECT_EQ(Loaded->GetWorld(), World);
	Durin::DestroyObject(World);
	EXPECT_EQ(Loaded->GetWorld(), nullptr);
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
}

TEST(FWorldTests, BuiltInActorsOwnTheirDefaultComponents)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ACameraActor* Camera = World->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::AStaticMeshActor* Mesh = World->SpawnActor<Durin::AStaticMeshActor>("Mesh");
	Durin::ADirectionalLightActor* Light = World->SpawnActor<Durin::ADirectionalLightActor>("Light");
	Durin::DCameraComponent* CameraComponent = Camera->GetCameraComponent();
	Durin::DStaticMeshComponent* MeshComponent = Mesh->GetStaticMeshComponent();
	Durin::DDirectionalLightComponent* LightComponent = Light->GetLightComponent();

	ASSERT_EQ(Camera->GetOwnedComponents().size(), 1u);
	EXPECT_EQ(Camera->GetOwnedComponents().front().Get(), CameraComponent);
	EXPECT_EQ(Camera->FindComponentByClass<Durin::DCameraComponent>(), CameraComponent);
	EXPECT_EQ(Camera->GetRootComponent(), CameraComponent);
	EXPECT_EQ(CameraComponent->GetOwner(), Camera);
	EXPECT_EQ(CameraComponent->GetOuter(), Camera);

	ASSERT_EQ(Mesh->GetOwnedComponents().size(), 1u);
	EXPECT_EQ(Mesh->GetOwnedComponents().front().Get(), MeshComponent);
	EXPECT_EQ(Mesh->FindComponentByClass<Durin::DStaticMeshComponent>(), MeshComponent);
	EXPECT_EQ(Mesh->GetRootComponent(), MeshComponent);
	EXPECT_EQ(MeshComponent->GetOwner(), Mesh);
	EXPECT_EQ(MeshComponent->GetOuter(), Mesh);

	ASSERT_EQ(Light->GetOwnedComponents().size(), 1u);
	EXPECT_EQ(Light->GetOwnedComponents().front().Get(), LightComponent);
	EXPECT_EQ(Light->FindComponentByClass<Durin::DDirectionalLightComponent>(), LightComponent);
	EXPECT_EQ(Light->GetRootComponent(), LightComponent);
	EXPECT_EQ(LightComponent->GetOwner(), Light);

	Durin::DestroyObject(World);
}

TEST(FSceneComponentTests, SupportsAttachmentTransformRulesAndPropagation)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ACameraActor* ParentActor = World->SpawnActor<Durin::ACameraActor>("Parent");
	Durin::ACameraActor* ChildActor = World->SpawnActor<Durin::ACameraActor>("Child");
	Durin::ACameraActor* GrandchildActor = World->SpawnActor<Durin::ACameraActor>("Grandchild");
	Durin::DSceneComponent* Parent = ParentActor->GetRootComponent();
	Durin::DSceneComponent* Child = ChildActor->GetRootComponent();
	Durin::DSceneComponent* Grandchild = GrandchildActor->GetRootComponent();

	Parent->SetWorldLocation(Durin::FVector3(10.0, 0.0, 0.0));
	Child->SetWorldLocation(Durin::FVector3(15.0, 0.0, 0.0));
	ASSERT_TRUE(Child->AttachToComponent(Parent));
	ExpectVectorNear(Child->GetWorldLocation(), Durin::FVector3(15.0, 0.0, 0.0));
	ExpectVectorNear(Child->GetRelativeLocation(), Durin::FVector3(5.0, 0.0, 0.0));

	Grandchild->SetRelativeLocation(Durin::FVector3(0.0, 2.0, 0.0));
	ASSERT_TRUE(Grandchild->AttachToComponent(Child, Durin::EAttachmentTransformRule::KeepRelative));
	Parent->SetWorldLocation(Durin::FVector3(20.0, 0.0, 0.0));
	ExpectVectorNear(Child->GetWorldLocation(), Durin::FVector3(25.0, 0.0, 0.0));
	ExpectVectorNear(Grandchild->GetWorldLocation(), Durin::FVector3(25.0, 2.0, 0.0));

	ASSERT_TRUE(Grandchild->DetachFromComponent(Durin::EDetachmentTransformRule::KeepRelative));
	ExpectVectorNear(Grandchild->GetWorldLocation(), Durin::FVector3(0.0, 2.0, 0.0));
	ASSERT_TRUE(Grandchild->AttachToComponent(Parent, Durin::EAttachmentTransformRule::SnapToTarget));
	ExpectVectorNear(Grandchild->GetRelativeLocation(), Durin::FVector3(0.0));
	ExpectVectorNear(Grandchild->GetWorldLocation(), Parent->GetWorldLocation());

	ASSERT_TRUE(Child->DetachFromComponent());
	ExpectVectorNear(Child->GetWorldLocation(), Durin::FVector3(25.0, 0.0, 0.0));
	Durin::DestroyObject(World);
}

TEST(FSceneComponentTests, ConvertsWorldAndRelativeTransformsAcrossHierarchy)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ACameraActor* ParentActor = World->SpawnActor<Durin::ACameraActor>("Parent");
	Durin::ACameraActor* ChildActor = World->SpawnActor<Durin::ACameraActor>("Child");
	Durin::DSceneComponent* Parent = ParentActor->GetRootComponent();
	Durin::DSceneComponent* Child = ChildActor->GetRootComponent();

	Durin::FTransform ParentTransform;
	ParentTransform.Translation = Durin::FVector3(3.0, 4.0, 5.0);
	ParentTransform.Rotation = glm::angleAxis(glm::radians(90.0), Durin::FVector3(0.0, 0.0, 1.0));
	ParentTransform.Scale3D = Durin::FVector3(2.0);
	Parent->SetWorldTransform(ParentTransform);
	ASSERT_TRUE(Child->AttachToComponent(Parent, Durin::EAttachmentTransformRule::KeepRelative));

	Durin::FTransform DesiredWorld;
	DesiredWorld.Translation = Durin::FVector3(7.0, 8.0, 9.0);
	DesiredWorld.Rotation = glm::angleAxis(glm::radians(45.0), Durin::FVector3(1.0, 0.0, 0.0));
	DesiredWorld.Scale3D = Durin::FVector3(4.0);
	Child->SetWorldTransform(DesiredWorld);

	const Durin::FTransform Reconstructed = Durin::FTransform::Combine(Parent->GetWorldTransform(), Child->GetRelativeTransform());
	ExpectVectorNear(Reconstructed.Translation, DesiredWorld.Translation);
	ExpectVectorNear(Reconstructed.Scale3D, DesiredWorld.Scale3D);
	EXPECT_NEAR(std::abs(glm::dot(Reconstructed.Rotation, DesiredWorld.Rotation)), 1.0, 1.e-8);
	Durin::DestroyObject(World);
}

TEST(FSceneComponentTests, SupportsInstanceComponentTreesWithinOneActor)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ACameraActor* Actor = World->SpawnActor<Durin::ACameraActor>("Actor");
	ASSERT_NE(Actor, nullptr);
	Durin::DSceneComponent* Root = Actor->GetRootComponent();
	ASSERT_NE(Root, nullptr);
	Root->SetWorldLocation(Durin::FVector3(10.0, 0.0, 0.0));

	auto* Parent = Durin::Cast<Durin::DSceneComponent>(Actor->AddInstanceComponent(Durin::DSceneComponent::StaticClass(), "Parent"));
	auto* Child = Durin::Cast<Durin::DSceneComponent>(Actor->AddInstanceComponent(Durin::DSceneComponent::StaticClass(), "Child"));
	auto* NewParent = Durin::Cast<Durin::DSceneComponent>(Actor->AddInstanceComponent(Durin::DSceneComponent::StaticClass(), "NewParent"));
	ASSERT_NE(Parent, nullptr);
	ASSERT_NE(Child, nullptr);
	ASSERT_NE(NewParent, nullptr);

	ASSERT_TRUE(Parent->AttachToComponent(Root, Durin::EAttachmentTransformRule::SnapToTarget));
	ASSERT_TRUE(Child->AttachToComponent(Parent, Durin::EAttachmentTransformRule::SnapToTarget));
	ExpectVectorNear(Child->GetRelativeLocation(), Durin::FVector3(0.0));
	ExpectVectorNear(Child->GetRelativeScale3D(), Durin::FVector3(1.0));
	EXPECT_NEAR(std::abs(glm::dot(Child->GetRelativeRotation(), Durin::FQuat(1.0, 0.0, 0.0, 0.0))), 1.0, 1.e-8);
	EXPECT_EQ(Child->GetAttachParent(), Parent);
	ASSERT_EQ(Parent->GetAttachChildren().size(), 1u);
	EXPECT_EQ(Parent->GetAttachChildren().front().Get(), Child);

	EXPECT_FALSE(Parent->AttachToComponent(Child));
	EXPECT_FALSE(Root->AttachToComponent(Parent));
	EXPECT_FALSE(Root->AttachToComponent(Root));

	Child->SetRelativeLocation(Durin::FVector3(2.0, 3.0, 4.0));
	const Durin::FTransform PreviousWorld = Child->GetWorldTransform();
	NewParent->SetRelativeLocation(Durin::FVector3(5.0, 0.0, 0.0));
	ASSERT_TRUE(Child->AttachToComponent(NewParent, Durin::EAttachmentTransformRule::KeepWorld));
	ExpectVectorNear(Child->GetWorldLocation(), PreviousWorld.Translation);
	EXPECT_NEAR(std::abs(glm::dot(Child->GetWorldRotation(), PreviousWorld.Rotation)), 1.0, 1.e-8);
	ExpectVectorNear(Child->GetWorldScale3D(), PreviousWorld.Scale3D);

	ASSERT_TRUE(Child->AttachToComponent(Parent, Durin::EAttachmentTransformRule::KeepWorld));
	Durin::TObjectPtr<Durin::DSceneComponent> ParentHandle = Parent;
	const Durin::FTransform BeforeParentRemoval = Child->GetWorldTransform();
	ASSERT_TRUE(Actor->DestroyInstanceComponent(Parent));
	EXPECT_EQ(ParentHandle.Get(), Parent);
	EXPECT_FALSE(ParentHandle.IsValid());
	EXPECT_FALSE(Actor->IsInstanceComponent(Parent));
	EXPECT_TRUE(std::ranges::none_of(Actor->GetOwnedComponents(), [Parent](const Durin::TObjectPtr<Durin::DActorComponent>& Entry) { return Entry.Get() == Parent; }));
	EXPECT_EQ(Child->GetAttachParent(), nullptr);
	ExpectVectorNear(Child->GetWorldLocation(), BeforeParentRemoval.Translation);
	EXPECT_TRUE(std::ranges::none_of(Root->GetAttachChildren(), [Parent](const Durin::TObjectPtr<Durin::DSceneComponent>& Entry) { return Entry.Get() == Parent; }));
	EXPECT_TRUE(Actor->IsInstanceComponent(Child));

	Durin::DestroyObject(World);
}

TEST(FSceneComponentTests, RejectsInvalidAndCrossWorldAttachments)
{
	Durin::DWorld* World = CreateWorld();
	Durin::DWorld* OtherWorld = CreateWorld();
	Durin::ACameraActor* Parent = World->SpawnActor<Durin::ACameraActor>("Parent");
	Durin::ACameraActor* Child = World->SpawnActor<Durin::ACameraActor>("Child");
	Durin::ACameraActor* Other = OtherWorld->SpawnActor<Durin::ACameraActor>("Other");
	Durin::DSceneComponent* ParentRoot = Parent->GetRootComponent();

	Durin::FTransform ActorTransform;
	ActorTransform.Translation = Durin::FVector3(1.0, 2.0, 3.0);
	EXPECT_TRUE(Parent->SetActorTransform(ActorTransform));
	ExpectVectorNear(Parent->GetActorTransform().Translation, ActorTransform.Translation);
	EXPECT_FALSE(Parent->SetRootComponent(Child->GetRootComponent()));
	EXPECT_EQ(Parent->GetRootComponent(), ParentRoot);

	EXPECT_FALSE(Parent->AttachToActor(Parent));
	ASSERT_TRUE(Child->AttachToActor(Parent));
	EXPECT_EQ(Child->GetAttachParentActor(), Parent);
	EXPECT_FALSE(Parent->AttachToActor(Child));
	EXPECT_FALSE(Child->AttachToActor(Other));
	EXPECT_EQ(Child->GetAttachParentActor(), Parent);
	ASSERT_TRUE(Child->DetachFromActor());
	EXPECT_EQ(Child->GetAttachParentActor(), nullptr);

	Durin::DestroyObject(World);
	Durin::DestroyObject(OtherWorld);
}

TEST(FSceneComponentTests, DestructionSafelyRemovesAttachmentLinks)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ACameraActor* Parent = World->SpawnActor<Durin::ACameraActor>("Parent");
	Durin::AStaticMeshActor* Child = World->SpawnActor<Durin::AStaticMeshActor>("Child");
	Durin::DSceneComponent* ParentRoot = Parent->GetRootComponent();
	Durin::DStaticMeshComponent* ChildRoot = Child->GetStaticMeshComponent();
	ChildRoot->SetWorldLocation(Durin::FVector3(9.0, 8.0, 7.0));
	ASSERT_TRUE(Child->AttachToActor(Parent));

	ASSERT_TRUE(World->DestroyActor(Parent));
	EXPECT_EQ(ChildRoot->GetAttachParent(), nullptr);
	ExpectVectorNear(ChildRoot->GetWorldLocation(), Durin::FVector3(9.0, 8.0, 7.0));

	Durin::ACameraActor* NewParent = World->SpawnActor<Durin::ACameraActor>("NewParent");
	ParentRoot = NewParent->GetRootComponent();
	ASSERT_TRUE(Child->AttachToActor(NewParent));
	ASSERT_EQ(ParentRoot->GetAttachChildren().size(), 1u);
	ChildRoot->DestroyComponent();
	EXPECT_TRUE(ParentRoot->GetAttachChildren().empty());
	EXPECT_EQ(Child->GetRootComponent(), nullptr);

	Durin::DestroyObject(World);
}

TEST(FWorldTests, DestroyActorRemovesItFromTheWorld)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ACameraActor* Camera = World->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::TObjectPtr<Durin::AActor> Selection = Camera;

	EXPECT_TRUE(World->DestroyActor(Camera));
	EXPECT_TRUE(World->GetActors().empty());
	EXPECT_EQ(World->FindActorByName("Camera"), nullptr);
	EXPECT_FALSE(World->ContainsActor(Selection.Get()));
	Selection = nullptr;
	EXPECT_EQ(Selection.Get(), nullptr);
	EXPECT_FALSE(World->DestroyActor(nullptr));

	Durin::DestroyObject(World);
}

TEST(FWorldTests, DestroyAllActorsInvalidatesObjectPointers)
{
	Durin::DWorld* World = CreateWorld();
	Durin::TObjectPtr<Durin::AActor> Camera = World->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::TObjectPtr<Durin::AActor> Mesh = World->SpawnActor<Durin::AStaticMeshActor>("Mesh");
	Durin::TObjectPtr<Durin::DActorComponent> CameraComponent = Camera->FindComponentByClass<Durin::DCameraComponent>();
	Durin::TObjectPtr<Durin::DActorComponent> MeshComponent = Mesh->FindComponentByClass<Durin::DStaticMeshComponent>();

	World->DestroyAllActors();

	EXPECT_TRUE(World->GetActors().empty());
	EXPECT_EQ(Camera.Get(), nullptr);
	EXPECT_EQ(Mesh.Get(), nullptr);
	EXPECT_EQ(CameraComponent.Get(), nullptr);
	EXPECT_EQ(MeshComponent.Get(), nullptr);

	Durin::DestroyObject(World);
}

TEST(FWorldTests, DestroyingWorldCascadesToActorsAndComponents)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ACameraActor* Camera = World->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::TObjectPtr<Durin::DWorld> WorldPtr = World;
	Durin::TObjectPtr<Durin::AActor> ActorPtr = Camera;
	Durin::TObjectPtr<Durin::DActorComponent> ComponentPtr = Camera->GetCameraComponent();

	Durin::DestroyObject(World);

	EXPECT_EQ(WorldPtr.Get(), nullptr);
	EXPECT_EQ(ActorPtr.Get(), nullptr);
	EXPECT_EQ(ComponentPtr.Get(), nullptr);
}

TEST(FEngineObjectTests, EngineAndWorldHaveReflectedOwnership)
{
	InitializeDObjectSystem();
	Durin::DEngine* Engine = Durin::NewObject<Durin::DEngine>(nullptr, "TestEngine");
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(Engine, "MainWorld");

	EXPECT_EQ(Engine->GetClass(), Durin::DEngine::StaticClass());
	EXPECT_EQ(World->GetClass(), Durin::DWorld::StaticClass());
	EXPECT_EQ(World->GetOuter(), Engine);

	Durin::DestroyObject(Engine);
}

TEST(FEngineObjectTests, RootedEngineSurvivesGarbageCollection)
{
	InitializeDObjectSystem();
	Durin::DEngine* Engine = Durin::NewObject<Durin::DEngine>(nullptr, "RootedEngine");
	Durin::TObjectPtr<Durin::DEngine> EnginePtr = Engine;

	Durin::AddToRoot(Engine);
	Durin::CollectGarbage();
	EXPECT_EQ(EnginePtr.Get(), Engine);

	Durin::RemoveFromRoot(Engine);
	Durin::CollectGarbage();
	EXPECT_EQ(EnginePtr.Get(), nullptr);
}

TEST(FEngineObjectTests, GameEngineHasConcreteRuntimeClass)
{
	InitializeDObjectSystem();
	Durin::DGameEngine* Engine = Durin::NewObject<Durin::DGameEngine>(nullptr, "GameEngine");

	EXPECT_EQ(Engine->GetClass(), Durin::DGameEngine::StaticClass());
	EXPECT_TRUE(Engine->IsA<Durin::DEngine>());

	Durin::DestroyObject(Engine);
}
