#include "WorldTestSupport.h"

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
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
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

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
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
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

#if DURIN_WITH_EDITOR

TEST(FWorldTests, TracksEditorActorHierarchyRevision)
{
	Durin::DWorld* World = CreateWorld();
	Durin::DLevel* Level = World->GetCurrentLevel();
	const auto InitialRevision = Level->GetEditorActorHierarchyRevision();

	Durin::AActor* Parent = World->SpawnActor<Durin::ACameraActor>("Parent");
	ASSERT_NE(Parent, nullptr);
	const auto AfterParentSpawn = Level->GetEditorActorHierarchyRevision();
	EXPECT_GT(AfterParentSpawn, InitialRevision);

	Durin::AActor* Child = World->SpawnActor<Durin::ACameraActor>("Child");
	ASSERT_NE(Child, nullptr);
	const auto AfterChildSpawn = Level->GetEditorActorHierarchyRevision();
	EXPECT_GT(AfterChildSpawn, AfterParentSpawn);

	Child->SetHidden(true);
	EXPECT_EQ(Level->GetEditorActorHierarchyRevision(), AfterChildSpawn);
	Durin::FTransform Transform;
	Transform.Translation = {1.0, 2.0, 3.0};
	EXPECT_TRUE(Child->SetActorTransform(Transform));
	EXPECT_EQ(Level->GetEditorActorHierarchyRevision(), AfterChildSpawn);

	ASSERT_TRUE(Level->RenameActor(Child, "RenamedChild"));
	const auto AfterRename = Level->GetEditorActorHierarchyRevision();
	EXPECT_GT(AfterRename, AfterChildSpawn);
	EXPECT_FALSE(Level->RenameActor(Child, Durin::FName()));
	EXPECT_EQ(Level->GetEditorActorHierarchyRevision(), AfterRename);

	ASSERT_TRUE(Child->GetRootComponent()->AttachToComponent(Parent->GetRootComponent()));
	const auto AfterAttach = Level->GetEditorActorHierarchyRevision();
	EXPECT_GT(AfterAttach, AfterRename);
	EXPECT_FALSE(Parent->AttachToActor(Child));
	EXPECT_EQ(Level->GetEditorActorHierarchyRevision(), AfterAttach);

	ASSERT_TRUE(Child->GetRootComponent()->DetachFromComponent());
	const auto AfterDetach = Level->GetEditorActorHierarchyRevision();
	EXPECT_GT(AfterDetach, AfterAttach);

	ASSERT_TRUE(World->DestroyActor(Child));
	EXPECT_GT(Level->GetEditorActorHierarchyRevision(), AfterDetach);

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}
#endif

TEST(FWorldTests, TracksActorVisibility)
{
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* Actor = World->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Actor, nullptr);
	EXPECT_FALSE(Actor->IsHidden());
	Actor->SetHidden(true);
	EXPECT_TRUE(Actor->IsHidden());
	Actor->SetHidden(false);
	EXPECT_FALSE(Actor->IsHidden());
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldTests, RenamesComponentsWithUniqueNames)
{
	Durin::DWorld* World = CreateWorld();
	Durin::AActor* Actor = World->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::DActorComponent* First = Actor->AddInstanceComponent(Durin::DSceneComponent::StaticClass(), "Scene");
	Durin::DActorComponent* Second = Actor->AddInstanceComponent(Durin::DSceneComponent::StaticClass(), "Other");
	ASSERT_NE(First, nullptr);
	ASSERT_NE(Second, nullptr);
	EXPECT_TRUE(Actor->RenameComponent(Second, "Scene"));
	EXPECT_EQ(Second->GetName(), "Scene_2");
	EXPECT_FALSE(Actor->RenameComponent(Second, Durin::FName()));
	EXPECT_EQ(Second->GetName(), "Scene_2");
	EXPECT_FALSE(Actor->RenameComponent(nullptr, "Invalid"));
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldTests, DistinguishesExactAndPolymorphicComponentQueries)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ACameraActor* Camera = World->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Camera, nullptr);
	Durin::DCameraComponent* CameraComponent = Camera->GetCameraComponent();
	ASSERT_NE(CameraComponent, nullptr);

	EXPECT_EQ(Camera->FindComponentByStaticClass<Durin::DCameraComponent>(), CameraComponent);
	EXPECT_EQ(Camera->FindComponentByStaticClass<Durin::DSceneComponent>(), nullptr);
	EXPECT_EQ(Camera->FindComponentByClass<Durin::DSceneComponent>(), CameraComponent);

	const std::vector<Durin::DActorComponent*> SceneComponents = Camera->FindComponentsByClass<Durin::DSceneComponent>();
	ASSERT_EQ(SceneComponents.size(), 1);
	EXPECT_EQ(SceneComponents.front(), CameraComponent);
	EXPECT_EQ(CameraComponent->GetOwner<Durin::AActor>(), Camera);
	EXPECT_EQ(CameraComponent->GetOwner<Durin::ACameraActor>(), Camera);

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
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
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
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
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FLevelAssetTests, SavesLoadsTransformsAttachmentsCameraAndDefaultComponents)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "Levels";
	static std::unordered_set<std::filesystem::path> InitializedRoots;
	if (InitializedRoots.insert(Root).second)
	{
		Durin::Testing::RemoveTestWorkDirectory(Root);
		Durin::PathUtilities::RegisterMountPoint("/LevelTests/", Root.generic_string() + "/");
	}

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
	ParentActor->GetCameraComponent()->SetAspectRatio(Durin::ECameraAspectRatioMode::Custom, 2.39f);

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
	EXPECT_EQ(LoadedParent->GetCameraComponent()->GetAspectRatioMode(), Durin::ECameraAspectRatioMode::Custom);
	EXPECT_NEAR(LoadedParent->GetCameraComponent()->GetCustomAspectRatio(), 2.39f, 1.e-6f);

	Durin::DWorld* World = CreateWorld();
	ASSERT_TRUE(World->SetCurrentLevel(Loaded));
	EXPECT_EQ(World->GetActors().size(), 2u);
	EXPECT_EQ(Loaded->GetWorld(), World);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
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

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}
