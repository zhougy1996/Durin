#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "WorldTestSupport.h"

#include "Rendering/LightSceneProxy.h"
#include "DObject/Package.h"
#include "Math/Operations.h"

TEST(FDirectionalLightTests, SceneDataRemainsDarkUntilPopulatedByAComponent)
{
	Durin::FDirectionalLightSceneData SceneData;
	EXPECT_FLOAT_EQ(SceneData.Intensity, 0.0f);
	EXPECT_FLOAT_EQ(SceneData.AmbientIntensity, 0.0f);
	EXPECT_TRUE(SceneData.bCastShadows);

	Durin::DWorld* World = CreateWorld();
	Durin::ADirectionalLightActor* Light = World->SpawnActor<Durin::ADirectionalLightActor>("DirectionalLight");
	ASSERT_NE(Light, nullptr);
	SceneData = Light->GetLightComponent()->GetSceneData();
	EXPECT_FLOAT_EQ(SceneData.Intensity, 1.0f);
	EXPECT_FLOAT_EQ(SceneData.AmbientIntensity, 0.08f);
	EXPECT_TRUE(SceneData.bCastShadows);
	EXPECT_EQ(SceneData.Color, Durin::FVector3f(1.0f));

	Durin::FProperty* ColorProperty = Durin::DDirectionalLightComponent::StaticClass()->FindPropertyByName("Color");
	ASSERT_NE(ColorProperty, nullptr);
	EXPECT_EQ(ColorProperty->GetMetaData(Durin::FName("HideAlpha")), "true");
	auto* Color = ColorProperty->ContainerPtrToValuePtr<Durin::FLinearColor>(Light->GetLightComponent());
	*Color = Durin::FLinearColor(-0.25f, 0.25f, 1.25f, 0.1f);
	SceneData = Light->GetLightComponent()->GetSceneData();
	EXPECT_EQ(SceneData.Color, Durin::FVector3f(0.0f, 0.25f, 1.0f));
	Light->GetLightComponent()->SetCastShadows(false);
	EXPECT_FALSE(Light->GetLightComponent()->GetSceneData().bCastShadows);
	EXPECT_NE(
		Durin::DDirectionalLightComponent::StaticClass()->FindPropertyByName(
			"bCastShadows"),
		nullptr);

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FDirectionalLightTests, LinearColorRoundTripsThroughLevelAssets)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "DirectionalLightLevels";
	static std::unordered_set<std::filesystem::path> InitializedRoots;
	if (InitializedRoots.insert(Root).second)
	{
		Durin::Testing::RemoveTestWorkDirectory(Root);
		Durin::Testing::RegisterMountPointForTests("/DirectionalLightTests/", Root.generic_string() + "/");
	}

	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/DirectionalLightTests/ColorRoundTrip", Path));
	Durin::DLevel* Level = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Level));
	Durin::ADirectionalLightActor* Light = Level->SpawnActor<Durin::ADirectionalLightActor>("ColoredLight");
	ASSERT_NE(Light, nullptr);
	Durin::FProperty* ColorProperty = Durin::DDirectionalLightComponent::StaticClass()->FindPropertyByName("Color");
	ASSERT_NE(ColorProperty, nullptr);
	*ColorProperty->ContainerPtrToValuePtr<Durin::FLinearColor>(Light->GetLightComponent()) = Durin::FLinearColor(0.1f, 0.35f, 0.8f, 1.0f);
	Light->GetLightComponent()->SetCastShadows(false);

	ASSERT_TRUE(Durin::SavePackage(Level->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(Path));
	Durin::DLevel* Loaded = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Loaded));
	ASSERT_NE(Loaded, nullptr);
	auto* LoadedLight = dynamic_cast<Durin::ADirectionalLightActor*>(Loaded->FindActorByName("ColoredLight"));
	ASSERT_NE(LoadedLight, nullptr);
	const Durin::FDirectionalLightSceneData SceneData = LoadedLight->GetLightComponent()->GetSceneData();
	EXPECT_NEAR(SceneData.Color.r, 0.1f, 1.e-6f);
	EXPECT_NEAR(SceneData.Color.g, 0.35f, 1.e-6f);
	EXPECT_NEAR(SceneData.Color.b, 0.8f, 1.e-6f);
	EXPECT_FALSE(SceneData.bCastShadows);
	EXPECT_TRUE(Durin::UnloadPackage(Path));
}

TEST(FLocalLightTests, PointAndSpotActorsNormalizeAuthoredValues)
{
	Durin::DWorld* World = CreateWorld();
	auto* Point = World->SpawnActor<Durin::APointLightActor>("PointLight");
	auto* Spot = World->SpawnActor<Durin::ASpotLightActor>("SpotLight");
	ASSERT_NE(Point, nullptr);
	ASSERT_NE(Spot, nullptr);
	Point->GetLightComponent()->SetWorldLocation(Durin::FVector3(1.0, 2.0, 3.0));
	Point->GetLightComponent()->SetRange(-5.0f);
	const Durin::FPointLightSceneData PointData =
		Point->GetLightComponent()->GetSceneData();
	EXPECT_EQ(PointData.Position, Durin::FVector3(1.0, 2.0, 3.0));
	EXPECT_FLOAT_EQ(PointData.Range, 1.0f);

	Spot->GetLightComponent()->SetRange(20.0f);
	Spot->GetLightComponent()->SetConeAngles(80.0f, 40.0f);
	const Durin::FSpotLightSceneData SpotData =
		Spot->GetLightComponent()->GetSceneData();
	EXPECT_FLOAT_EQ(SpotData.Range, 20.0f);
	EXPECT_FLOAT_EQ(SpotData.InnerConeAngle, 40.0f);
	EXPECT_FLOAT_EQ(SpotData.OuterConeAngle, 40.0f);
	EXPECT_NEAR(Durin::Math::Length(SpotData.Direction), 1.0, 1.0e-8);
	EXPECT_NE(Durin::DPointLightComponent::StaticClass()->FindPropertyByName("Range"), nullptr);
	EXPECT_NE(Durin::DSpotLightComponent::StaticClass()->FindPropertyByName("OuterConeAngle"), nullptr);

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
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
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
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
	ParentTransform.Rotation = Durin::Math::MakeQuaternionFromAxisAngleDegrees(
		90.0, Durin::FVector3(0.0, 0.0, 1.0));
	ParentTransform.Scale3D = Durin::FVector3(2.0);
	Parent->SetWorldTransform(ParentTransform);
	ASSERT_TRUE(Child->AttachToComponent(Parent, Durin::EAttachmentTransformRule::KeepRelative));

	Durin::FTransform DesiredWorld;
	DesiredWorld.Translation = Durin::FVector3(7.0, 8.0, 9.0);
	DesiredWorld.Rotation = Durin::Math::MakeQuaternionFromAxisAngleDegrees(
		45.0, Durin::FVector3(1.0, 0.0, 0.0));
	DesiredWorld.Scale3D = Durin::FVector3(4.0);
	Child->SetWorldTransform(DesiredWorld);

	const Durin::FTransform Reconstructed = Durin::FTransform::Combine(Parent->GetWorldTransform(), Child->GetRelativeTransform());
	ExpectVectorNear(Reconstructed.Translation, DesiredWorld.Translation);
	ExpectVectorNear(Reconstructed.Scale3D, DesiredWorld.Scale3D);
	EXPECT_TRUE(Durin::Math::AreRotationsEquivalent(Reconstructed.Rotation, DesiredWorld.Rotation, 1.e-8));
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FSceneComponentTests, EqualTransformSettersDoNotDirtyTheOwningPackage)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "SceneComponentLevels";
	static std::unordered_set<std::filesystem::path> InitializedRoots;
	if (InitializedRoots.insert(Root).second)
	{
		Durin::Testing::RemoveTestWorkDirectory(Root);
		Durin::Testing::RegisterMountPointForTests("/SceneComponentTests/", Root.generic_string() + "/");
	}

	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SceneComponentTests/EqualTransformSetters", Path));
	Durin::DLevel* Level = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Level));
	Durin::ACameraActor* Actor = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Actor, nullptr);
	Durin::DSceneComponent* RootComponent = Actor->GetRootComponent();
	ASSERT_NE(RootComponent, nullptr);
	RootComponent->SetRelativeLocation(Durin::FVector3(1.0, 2.0, 3.0));
	Durin::DPackage* Package = Level->GetPackage();
	ASSERT_NE(Package, nullptr);
	const uint64 Revision = Package->GetEditRevision();

	RootComponent->SetRelativeTransform(RootComponent->GetRelativeTransform());
	RootComponent->SetWorldTransform(RootComponent->GetWorldTransform());

	EXPECT_EQ(Package->GetEditRevision(), Revision);
	EXPECT_TRUE(Durin::UnloadPackage(Package, Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
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
	EXPECT_TRUE(Durin::Math::AreRotationsEquivalent(
		Child->GetRelativeRotation(), Durin::FQuatConstants::Identity, 1.e-8));
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
	EXPECT_TRUE(Durin::Math::AreRotationsEquivalent(Child->GetWorldRotation(), PreviousWorld.Rotation, 1.e-8));
	ExpectVectorNear(Child->GetWorldScale3D(), PreviousWorld.Scale3D);

	ASSERT_TRUE(Child->AttachToComponent(Parent, Durin::EAttachmentTransformRule::KeepWorld));
	Durin::TObjectPtr<Durin::DSceneComponent> ParentHandle = Parent;
	const Durin::FTransform BeforeParentRemoval = Child->GetWorldTransform();
	ASSERT_TRUE(Actor->DestroyInstanceComponent(Parent));
	EXPECT_EQ(ParentHandle.Get(), Parent);
	EXPECT_FALSE(ParentHandle.IsValid());
	EXPECT_FALSE(Actor->IsInstanceComponent(Parent));
	EXPECT_TRUE(std::ranges::none_of(Actor->GetComponents(), [Parent](const Durin::TObjectPtr<Durin::DActorComponent>& Entry) { return Entry.Get() == Parent; }));
	EXPECT_EQ(Child->GetAttachParent(), nullptr);
	ExpectVectorNear(Child->GetWorldLocation(), BeforeParentRemoval.Translation);
	EXPECT_TRUE(std::ranges::none_of(Root->GetAttachChildren(), [Parent](const Durin::TObjectPtr<Durin::DSceneComponent>& Entry) { return Entry.Get() == Parent; }));
	EXPECT_TRUE(Actor->IsInstanceComponent(Child));

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
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

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::MarkObjectHierarchyAsGarbage(OtherWorld);
	Durin::CollectGarbage();
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

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
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

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}
