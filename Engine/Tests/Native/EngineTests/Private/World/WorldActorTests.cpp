#include "WorldTestSupport.h"

namespace
{
	class FNativeConstructionTestActor final : public Durin::AActor
	{
	public:
		explicit FNativeConstructionTestActor(const Durin::FObjectInitializer& Initializer)
			: AActor(Initializer)
		{
			Root = CreateDefaultComponent<Durin::DSceneComponent>("Root");
			SetRootComponent(Root);
		}

		std::vector<std::pair<Durin::FActorGeneratedComponentKey, Durin::DClass*>> Desired;
		std::vector<Durin::DActorComponent*> Acquired;
		std::vector<bool> OwnedDuringConstruction;
		bool bAcquireFirstTwice = false;
		bool bRequestAgain = false;
		uint32 ConstructionCalls = 0;

	protected:
		auto OnNativeConstruct(Durin::FActorConstructionContext& Context,
			std::string& OutError) -> bool override
		{
			++ConstructionCalls;
			Acquired.clear();
			OwnedDuringConstruction.clear();
			for (const auto& [Key, Class] : Desired)
			{
				Durin::DActorComponent* Component = Context.AcquireGeneratedComponent(Key, Class, "Generated");
				Acquired.push_back(Component);
				OwnedDuringConstruction.push_back(OwnsComponent(Component));
			}
			if (bAcquireFirstTwice && !Desired.empty())
				Context.AcquireGeneratedComponent(Desired.front().first, Desired.front().second, "Duplicate");
			if (bRequestAgain)
			{
				bRequestAgain = false;
				RequestNativeReconstruction();
			}
			OutError.clear();
			return !Context.HasFailed();
		}

	private:
		Durin::DSceneComponent* Root = nullptr;
	};

	auto MakeNativeConstructionTestClass() -> std::unique_ptr<Durin::DClass>
	{
		auto Constructor = [](const Durin::FObjectInitializer& Initializer) {
			new (Initializer.GetObj()) FNativeConstructionTestActor(Initializer);
		};
		auto Class = std::make_unique<Durin::DClass>(Durin::EC_StaticConstructor,
			"NativeConstructionTestActor", static_cast<uint32>(sizeof(FNativeConstructionTestActor)),
			static_cast<uint32>(alignof(FNativeConstructionTestActor)), Durin::EObjectFlags::Transient,
			Durin::EClassFlags::None, Durin::EClassCastFlags::DClass, Constructor);
		Class->SetSuperStructBase(Durin::AActor::StaticClass());
		return Class;
	}

	auto MakeGeneratedKey(uint32 Value) -> Durin::FActorGeneratedComponentKey
	{
		return {"TestGenerated", Durin::FGuid(0x44555249, 0x4E47454E, 0, Value)};
	}
}

TEST(FWorldTests, ReflectedClassNamesSeparateIdentityDisplayAndObjectDefaults)
{
	InitializeDObjectSystem();
	Durin::DClass* StaticMeshClass = Durin::AStaticMeshActor::StaticClass();
	EXPECT_EQ(StaticMeshClass->GetQualifiedName().ToString(), "Durin::AStaticMeshActor");
	EXPECT_EQ(StaticMeshClass->GetShortName(), "AStaticMeshActor");
	EXPECT_EQ(StaticMeshClass->GetDisplayName(), "Static Mesh Actor");
	EXPECT_EQ(StaticMeshClass->GetDefaultObjectName(), "StaticMeshActor");

	Durin::DClass* SkeletalMeshClass = Durin::ASkeletalMeshActor::StaticClass();
	EXPECT_EQ(SkeletalMeshClass->GetQualifiedName().ToString(), "Durin::ASkeletalMeshActor");
	EXPECT_EQ(SkeletalMeshClass->GetShortName(), "ASkeletalMeshActor");
	EXPECT_EQ(SkeletalMeshClass->GetDisplayName(), "Skeletal Mesh Actor");
	EXPECT_EQ(SkeletalMeshClass->GetDefaultObjectName(), "SkeletalMeshActor");

	Durin::DClass* CameraClass = Durin::ACameraActor::StaticClass();
	EXPECT_EQ(CameraClass->GetDisplayName(), "Camera Actor");
	EXPECT_EQ(CameraClass->GetDefaultObjectName(), "CameraActor");

	Durin::DClass* ComponentClass = Durin::DSceneComponent::StaticClass();
	EXPECT_EQ(ComponentClass->GetDisplayName(), "Scene Component");
	EXPECT_EQ(ComponentClass->GetDefaultObjectName(), "SceneComponent");
}

TEST(FNativeConstructionTests, ReconcilesStableKeysAtomicallyAndRoutesLiveLifecycle)
{
	Durin::DWorld* World = CreateWorld();
	auto TestClass = MakeNativeConstructionTestClass();
	auto* Actor = static_cast<FNativeConstructionTestActor*>(
		World->SpawnActor(TestClass.get(), "Constructed"));
	ASSERT_NE(Actor, nullptr);
	ASSERT_EQ(Actor->GetAuthoredComponents().size(), 1u);
	EXPECT_EQ(Actor->GetAuthoredComponents().front()->GetCreationMethod(),
		Durin::EComponentCreationMethod::Native);

	const auto FirstKey = MakeGeneratedKey(1);
	const auto SecondKey = MakeGeneratedKey(2);
	Actor->Desired = {{FirstKey, Durin::DSceneComponent::StaticClass()},
		{SecondKey, Durin::DSceneComponent::StaticClass()}};
	ASSERT_TRUE(Actor->RequestNativeReconstruction()) << Actor->GetNativeConstructionError();
	ASSERT_EQ(Actor->Acquired.size(), 2u);
	Durin::DActorComponent* First = Actor->Acquired[0];
	Durin::DActorComponent* Second = Actor->Acquired[1];
	ASSERT_NE(First, nullptr);
	ASSERT_NE(Second, nullptr);
	EXPECT_EQ(Actor->OwnedDuringConstruction, (std::vector<bool>{false, false}));
	EXPECT_EQ(First->GetCreationMethod(), Durin::EComponentCreationMethod::Generated);
	EXPECT_TRUE(First->HasAnyObjectFlags(Durin::EObjectFlags::Transient));
	EXPECT_EQ(First->GetOwner(), Actor);
	EXPECT_TRUE(First->IsRegistered());
	EXPECT_EQ(static_cast<Durin::DSceneComponent*>(First)->GetAttachParent(), Actor->GetRootComponent());
	EXPECT_EQ(Actor->GetAuthoredComponents().size(), 1u);
	EXPECT_EQ(Actor->GetComponents().size(), 3u);
	EXPECT_EQ(Actor->GetComponents()[0].Get(), Actor->GetRootComponent());
	EXPECT_EQ(Actor->GetComponents()[1].Get(), First);
	EXPECT_EQ(Actor->GetComponents()[2].Get(), Second);
	EXPECT_FALSE(Actor->RenameComponent(First, "AuthoredName"));

	ASSERT_TRUE(Actor->RequestNativeReconstruction());
	EXPECT_EQ(Actor->Acquired[0], First);
	EXPECT_EQ(Actor->Acquired[1], Second);
	EXPECT_EQ(Actor->OwnedDuringConstruction, (std::vector<bool>{true, true}));
	std::unordered_map<Durin::DObject*, Durin::DObject*> Duplicates;
	std::string DuplicateError;
	auto* Duplicate = static_cast<FNativeConstructionTestActor*>(Durin::DuplicateObjectGraph(
		Actor, World->GetCurrentLevel(), "DuplicateConstructed", &DuplicateError, &Duplicates));
	ASSERT_NE(Duplicate, nullptr) << DuplicateError;
	EXPECT_FALSE(Duplicates.contains(First));
	EXPECT_FALSE(Duplicates.contains(Second));
	EXPECT_EQ(Duplicate->GetAuthoredComponents().size(), 1u);
	EXPECT_EQ(Duplicate->GetComponents().size(), 1u);
	Durin::MarkObjectHierarchyAsGarbage(Duplicate);

	Actor->Desired.erase(Actor->Desired.begin() + 1);
	ASSERT_TRUE(Actor->RequestNativeReconstruction());
	EXPECT_EQ(Actor->Acquired.front(), First);
	EXPECT_TRUE(Second->IsPendingKill());
	EXPECT_EQ(Actor->GetComponents().size(), 2u);

	Actor->Desired.front().second = Durin::DPhysicsComponent::StaticClass();
	EXPECT_FALSE(Actor->RequestNativeReconstruction());
	EXPECT_EQ(Actor->GetComponents().size(), 2u);
	EXPECT_TRUE(Actor->OwnsComponent(First));
	EXPECT_FALSE(First->IsPendingKill());
	Actor->Desired.front().second = Durin::DSceneComponent::StaticClass();

	Actor->Desired.push_back({MakeGeneratedKey(3), Durin::DSceneComponent::StaticClass()});
	Actor->bAcquireFirstTwice = true;
	EXPECT_FALSE(Actor->RequestNativeReconstruction());
	Actor->bAcquireFirstTwice = false;
	EXPECT_EQ(Actor->GetComponents().size(), 2u);
	EXPECT_TRUE(Actor->OwnsComponent(First));

	Actor->Desired.resize(1);
	const uint32 CallsBeforeCoalescing = Actor->ConstructionCalls;
	Actor->bRequestAgain = true;
	ASSERT_TRUE(Actor->RequestNativeReconstruction());
	EXPECT_EQ(Actor->ConstructionCalls, CallsBeforeCoalescing + 2);
	EXPECT_EQ(Actor->Acquired.front(), First);

	ASSERT_TRUE(World->BeginPlay({}));
	EXPECT_TRUE(First->HasBegunPlay());
	World->EndPlay();
	EXPECT_FALSE(First->HasBegunPlay());

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FNativeConstructionTests, InstanceCreationMethodRemainsPersistentAuthoredState)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ACameraActor* Actor = World->SpawnActor<Durin::ACameraActor>();
	Durin::DActorComponent* Instance = Actor->AddInstanceComponent(
		Durin::DSceneComponent::StaticClass(), "Instance");
	ASSERT_NE(Instance, nullptr);
	EXPECT_EQ(Instance->GetCreationMethod(), Durin::EComponentCreationMethod::Instance);
	EXPECT_EQ(Actor->GetAuthoredComponents().size(), 2u);
	EXPECT_EQ(Actor->GetComponents().size(), 2u);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FNativeConstructionTests, RepeatedDerivedReconciliationDoesNotDirtyTheLevelPackage)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "NativeConstructionLevels";
	static std::unordered_set<std::filesystem::path> InitializedRoots;
	if (InitializedRoots.insert(Root).second)
	{
		Durin::Testing::RemoveTestWorkDirectory(Root);
		Durin::PathUtilities::RegisterMountPointForTests(
			"/NativeConstructionTests/", Root.generic_string() + "/");
	}
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/NativeConstructionTests/Dirty", Path));
	Durin::DLevel* Level = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Level));
	auto TestClass = MakeNativeConstructionTestClass();
	auto* Actor = static_cast<FNativeConstructionTestActor*>(Level->SpawnActor(
		TestClass.get(), "Constructed"));
	ASSERT_NE(Actor, nullptr);
	Actor->Desired = {{MakeGeneratedKey(1), Durin::DSceneComponent::StaticClass()}};
	Level->GetPackage()->ClearDirty();
	const uint64 Revision = Level->GetPackage()->GetEditRevision();
	ASSERT_TRUE(Actor->RequestNativeReconstruction());
	ASSERT_TRUE(Actor->RequestNativeReconstruction());
	EXPECT_FALSE(Level->GetPackage()->IsDirty());
	EXPECT_EQ(Level->GetPackage()->GetEditRevision(), Revision);
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Level->GetPackage(), Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FWorldTests, SkeletalMeshActorOwnsDefaultRootComponent)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ASkeletalMeshActor* Actor = World->SpawnActor<Durin::ASkeletalMeshActor>();
	ASSERT_NE(Actor, nullptr);
	ASSERT_NE(Actor->GetSkeletalMeshComponent(), nullptr);
	EXPECT_EQ(Actor->GetRootComponent(), Actor->GetSkeletalMeshComponent());
	EXPECT_EQ(Actor->GetSkeletalMeshComponent()->GetOuter(), Actor);
	EXPECT_EQ(Actor->GetSkeletalMeshComponent()->GetSkeletalMesh(), nullptr);
	EXPECT_EQ(Actor->GetSkeletalMeshComponent()->GetAnimationClip(), nullptr);
	EXPECT_FALSE(Actor->GetSkeletalMeshComponent()->IsPlaying());
	EXPECT_EQ(Actor->GetName(), "SkeletalMeshActor");
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
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

	EXPECT_EQ(Camera->FindComponentByExactClass<Durin::DCameraComponent>(), CameraComponent);
	EXPECT_EQ(Camera->FindComponentByExactClass<Durin::DSceneComponent>(), nullptr);
	EXPECT_EQ(Camera->FindComponentByClass<Durin::DSceneComponent>(), CameraComponent);

	const std::vector<Durin::DSceneComponent*> SceneComponents = Camera->FindComponentsByClass<Durin::DSceneComponent>();
	ASSERT_EQ(SceneComponents.size(), 1);
	EXPECT_EQ(SceneComponents.front(), CameraComponent);
	std::vector<Durin::DSceneComponent*> Output = {nullptr};
	const Durin::AActor* ConstCamera = Camera;
	ConstCamera->GetComponents(Output);
	ASSERT_EQ(Output.size(), 1u);
	EXPECT_EQ(Output.front(), CameraComponent);
	EXPECT_EQ(ConstCamera->FindComponentByClass<Durin::DSceneComponent>(), CameraComponent);
	const size_t ComponentCount = Camera->GetComponents().size();
	Camera->AddOwnedComponent(CameraComponent);
	EXPECT_EQ(Camera->GetComponents().size(), ComponentCount);
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
	EXPECT_NE(std::ranges::find(ActorClasses, Durin::ASkeletalMeshActor::StaticClass()), ActorClasses.end());
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
		Durin::PathUtilities::RegisterMountPointForTests("/LevelTests/", Root.generic_string() + "/");
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
	EXPECT_EQ(LoadedParent->GetComponents().size(), 1u);
	EXPECT_EQ(LoadedChild->GetComponents().size(), 2u);
	ASSERT_EQ(LoadedChild->GetInstanceComponents().size(), 1u);
	auto* LoadedExtraComponent = dynamic_cast<Durin::DSceneComponent*>(LoadedChild->GetInstanceComponents().front().Get());
	ASSERT_NE(LoadedExtraComponent, nullptr);
	EXPECT_EQ(LoadedExtraComponent->GetCreationMethod(), Durin::EComponentCreationMethod::Instance);
	EXPECT_EQ(LoadedChild->GetRootComponent()->GetCreationMethod(),
		Durin::EComponentCreationMethod::Native);
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

	ASSERT_EQ(Camera->GetComponents().size(), 1u);
	EXPECT_EQ(Camera->GetComponents().front().Get(), CameraComponent);
	EXPECT_EQ(Camera->FindComponentByClass<Durin::DCameraComponent>(), CameraComponent);
	EXPECT_EQ(Camera->GetRootComponent(), CameraComponent);
	EXPECT_EQ(CameraComponent->GetOwner(), Camera);
	EXPECT_EQ(CameraComponent->GetOuter(), Camera);

	ASSERT_EQ(Mesh->GetComponents().size(), 1u);
	EXPECT_EQ(Mesh->GetComponents().front().Get(), MeshComponent);
	EXPECT_EQ(Mesh->FindComponentByClass<Durin::DStaticMeshComponent>(), MeshComponent);
	EXPECT_EQ(Mesh->GetRootComponent(), MeshComponent);
	EXPECT_EQ(MeshComponent->GetOwner(), Mesh);
	EXPECT_EQ(MeshComponent->GetOuter(), Mesh);

	ASSERT_EQ(Light->GetComponents().size(), 1u);
	EXPECT_EQ(Light->GetComponents().front().Get(), LightComponent);
	EXPECT_EQ(Light->FindComponentByClass<Durin::DDirectionalLightComponent>(), LightComponent);
	EXPECT_EQ(Light->GetRootComponent(), LightComponent);
	EXPECT_EQ(LightComponent->GetOwner(), Light);

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

#if DURIN_WITH_EDITOR
TEST(FWorldTests, EditorPickingMutationPublishesNestedTransientWeakReflection)
{
	auto* Mutation = Durin::FEditorPickingPrimitiveMutation::StaticStruct();
	auto* Batch = Durin::FEditorPickingPrimitiveMutationBatch::StaticStruct();
	ASSERT_NE(Mutation, nullptr);
	ASSERT_NE(Batch, nullptr);
	for (const char* Name : {"Actor", "Component"})
	{
		auto* Property = Mutation->FindPropertyByName(Name, false);
		ASSERT_NE(Property, nullptr);
		EXPECT_EQ(Property->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::WeakObject);
		EXPECT_TRUE(Property->HasAnyPropertyFlags(Durin::EPropertyFlags::Transient));
	}
	auto* Mutations = static_cast<Durin::FArrayProperty*>(
		Batch->FindPropertyByName("Mutations", false));
	ASSERT_NE(Mutations, nullptr);
	EXPECT_TRUE(Mutations->HasAnyPropertyFlags(Durin::EPropertyFlags::Transient));
	auto* Inner = static_cast<Durin::FStructProperty*>(Mutations->GetInner());
	ASSERT_NE(Inner, nullptr);
	EXPECT_EQ(Inner->GetStruct(), Mutation);
}
#endif
