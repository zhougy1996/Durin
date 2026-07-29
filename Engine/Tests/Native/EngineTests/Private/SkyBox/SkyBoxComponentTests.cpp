#include "SkyBoxTestSupport.h"

TEST(FSkyBoxTests, SceneSelectsSmallestStableIdAndRejectsStaleUpdates)
{
	InitializeDObjectSystem();
	Durin::InitRenderingThread();
	Durin::FScene Scene;
	const Durin::FGuid SmallerId(1, 0, 0, 0);
	const Durin::FGuid LargerId(2, 0, 0, 0);

	Durin::FSkyBoxSceneData Larger;
	Larger.SceneId = LargerId;
	Larger.InstanceId = 2;
	Larger.SelectionKey = "Larger";
	Larger.Intensity = 2.0f;
	Larger.Revision = 3;
	Scene.AddOrReplaceSkyBox(Larger);
	Scene.RemoveSkyBox(Larger.InstanceId, 2);

	Durin::FSkyBoxSceneData Smaller;
	Smaller.SceneId = SmallerId;
	Smaller.InstanceId = 1;
	Smaller.SelectionKey = "Smaller";
	Smaller.Intensity = 4.0f;
	Smaller.Revision = 1;
	Scene.AddOrReplaceSkyBox(Smaller);

	FSkyBoxObservation Observation = ObserveSkyBoxes(Scene);
	ASSERT_TRUE(Observation.bHasActive);
	EXPECT_EQ(Observation.Count, 2u);
	EXPECT_EQ(Observation.Active.SceneId, SmallerId);
	EXPECT_EQ(Observation.Active.Intensity, 4.0f);

	Durin::FSkyBoxSceneData DuplicateGuid = Smaller;
	DuplicateGuid.InstanceId = 3;
	DuplicateGuid.SelectionKey = "A";
	DuplicateGuid.Intensity = 6.0f;
	Scene.AddOrReplaceSkyBox(DuplicateGuid);
	Observation = ObserveSkyBoxes(Scene);
	ASSERT_TRUE(Observation.bHasActive);
	EXPECT_EQ(Observation.Count, 3u);
	EXPECT_EQ(Observation.Active.InstanceId, DuplicateGuid.InstanceId);
	EXPECT_EQ(Observation.Active.Intensity, 6.0f);
	Scene.RemoveSkyBox(DuplicateGuid.InstanceId, 2);

	Scene.RemoveSkyBox(Smaller.InstanceId, 2);
	Observation = ObserveSkyBoxes(Scene);
	ASSERT_TRUE(Observation.bHasActive);
	EXPECT_EQ(Observation.Count, 1u);
	EXPECT_EQ(Observation.Active.SceneId, LargerId);

	Scene.Release();
	Observation = ObserveSkyBoxes(Scene);
	EXPECT_FALSE(Observation.bHasActive);
	EXPECT_EQ(Observation.Count, 0u);
	Durin::ShutdownRenderingThread();
}

TEST(FSkyBoxTests, ActorDefaultsSerializeAndRetainCubeReference)
{
	InitializeDObjectSystem();
	auto* Actor = Durin::NewObject<Durin::ASkyBoxActor>(nullptr, "SkyBoxActor");
	Durin::DSkyBoxComponent* Component = Actor->GetSkyBoxComponent();
	auto* Cube = Durin::NewObject<Durin::DTextureCube>(nullptr, "ReferencedCube");
	ASSERT_NE(Component, nullptr);
	ASSERT_TRUE(Component->GetSkyBoxSceneId().IsValid());
	const Durin::FGuid OriginalSceneId = Component->GetSkyBoxSceneId();

	Component->SetTextureCube(Cube);
	Component->SetTint(Durin::FLinearColor(0.25f, 0.5f, 0.75f, 1.0f));
	Component->SetIntensity(-2.0f);
	EXPECT_EQ(Component->GetIntensity(), 0.0f);

	Durin::AddToRoot(Actor);
	const Durin::FObjectHandle CubeHandle = Durin::MakeObjectHandle(Cube);
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::ResolveObjectHandle(CubeHandle), Cube);

	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::SaveObjectGraphToMemory(Actor, Bytes));
	auto* LoadedActor = Durin::Cast<Durin::ASkyBoxActor>(Durin::LoadObjectGraphFromMemory(Bytes));
	ASSERT_NE(LoadedActor, nullptr);
	Durin::DSkyBoxComponent* LoadedComponent = LoadedActor->GetSkyBoxComponent();
	ASSERT_NE(LoadedComponent, nullptr);
	ASSERT_NE(LoadedComponent->GetTextureCube(), nullptr);
	EXPECT_EQ(LoadedComponent->GetTextureCube()->GetName(), "ReferencedCube");
	EXPECT_EQ(LoadedComponent->GetSkyBoxSceneId(), OriginalSceneId);
	EXPECT_NE(LoadedComponent->GetSkyBoxInstanceId(), Component->GetSkyBoxInstanceId());
	EXPECT_EQ(LoadedComponent->GetTint(), (Durin::FLinearColor(0.25f, 0.5f, 0.75f, 1.0f)));
	EXPECT_EQ(LoadedComponent->GetIntensity(), 0.0f);

	Component->SetTextureCube(nullptr);
	Durin::RemoveFromRoot(Actor);
	Durin::MarkAsGarbage(Actor);
	Durin::MarkAsGarbage(LoadedActor);
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::ResolveObjectHandle(CubeHandle), nullptr);
}

TEST(FSkyBoxTests, ComponentSynchronizesRegistrationVisibilityTransformAndProperties)
{
	InitializeDObjectSystem();
	Durin::InitRenderingThread();
	FSkyBoxTestEngine Engine;
	Durin::FScene* Scene = Engine.CreateTestScene();
	Durin::GEngine = &Engine;
	auto* World = Durin::NewObject<Durin::DWorld>(&Engine, "LiveSkyBoxWorld");
	ASSERT_TRUE(World->SetCurrentLevel(
		Durin::NewObject<Durin::DLevel>(World, "LiveSkyBoxLevel")));
	Engine.SetWorld(World);
	auto* Actor = World->SpawnActor<Durin::ASkyBoxActor>("LiveSkyBoxActor");
	Durin::DSkyBoxComponent* Component = Actor->GetSkyBoxComponent();
	auto* Cube = Durin::NewObject<Durin::DTextureCube>(nullptr, "LiveSkyBoxCube");

	Component->RegisterComponent();
	FSkyBoxObservation Observation = ObserveSkyBoxes(*Scene);
	ASSERT_TRUE(Observation.bHasActive);
	EXPECT_EQ(Observation.Count, 1u);
	EXPECT_EQ(Observation.Active.SceneId, Component->GetSkyBoxSceneId());

	const Durin::FQuat Rotation = glm::angleAxis(glm::radians(45.0), Durin::FVectorConstants::Up);
	Component->SetTextureCube(Cube);
	Component->SetTint(Durin::FLinearColor(0.2f, 0.4f, 0.6f, 1.0f));
	Component->SetIntensity(3.0f);
	Component->SetWorldRotation(Rotation);
	Observation = ObserveSkyBoxes(*Scene);
	ASSERT_TRUE(Observation.bHasActive);
	EXPECT_EQ(
		Observation.Active.TextureReference, Cube->GetTextureReferenceRHI());
	EXPECT_EQ(Observation.Active.Rotation, Rotation);
	EXPECT_EQ(Observation.Active.Tint, Durin::FVector3f(0.2f, 0.4f, 0.6f));
	EXPECT_EQ(Observation.Active.Intensity, 3.0f);
	EXPECT_EQ(Observation.Active.Revision, Component->GetSkyBoxRevision());

	Actor->SetHidden(true);
	EXPECT_FALSE(ObserveSkyBoxes(*Scene).bHasActive);
	Actor->SetHidden(false);
	EXPECT_TRUE(ObserveSkyBoxes(*Scene).bHasActive);
	Component->UnregisterComponent();
	EXPECT_FALSE(ObserveSkyBoxes(*Scene).bHasActive);

	Engine.SetWorld(nullptr);
	Scene->Release();
	Durin::FlushRenderingCommands();
	Engine.ResetTestScene();
	Durin::GEngine = nullptr;
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::MarkAsGarbage(Cube);
	Durin::CollectGarbage();
	Durin::ShutdownRenderingThread();
}

TEST(FSkyBoxTests, WorldSceneEndpointIsIndependentOfGlobalEngine)
{
	InitializeDObjectSystem();
	Durin::InitRenderingThread();
	FSkyBoxTestEngine Engine;
	Durin::FScene* MainScene = Engine.CreateTestScene();
	auto AuxiliaryScene = std::make_unique<Durin::FScene>();
	Durin::GEngine = &Engine;

	auto* World = Durin::NewObject<Durin::DWorld>(&Engine, "AuxiliarySkyBoxWorld");
	World->SetRenderScene(AuxiliaryScene.get());
	ASSERT_TRUE(World->SetCurrentLevel(
		Durin::NewObject<Durin::DLevel>(World, "AuxiliarySkyBoxLevel")));
	ASSERT_NE(World->SpawnActor<Durin::ASkyBoxActor>("AuxiliarySkyBox"), nullptr);
	EXPECT_FALSE(ObserveSkyBoxes(*MainScene).bHasActive);
	EXPECT_TRUE(ObserveSkyBoxes(*AuxiliaryScene).bHasActive);

	Durin::GEngine = nullptr;
	ASSERT_TRUE(World->SetCurrentLevel(nullptr));
	EXPECT_FALSE(ObserveSkyBoxes(*AuxiliaryScene).bHasActive);
	World->SetRenderScene(nullptr);

	AuxiliaryScene->Release();
	MainScene->Release();
	Durin::FlushRenderingCommands();
	AuxiliaryScene.reset();
	Engine.ResetTestScene();
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
	Durin::ShutdownRenderingThread();
}

TEST(FSkyBoxTests, PackageTracksAndReloadsCubeAssetDependency)
{
	InitializeSkyBoxAssetMount();
	Durin::FTextureCubeImportResult CubeResult = Durin::DTextureCube::ImportAsset(
		GetSkyBoxConventionFaces(), "/SkyBoxAssetTests/Cube");
	ASSERT_TRUE(CubeResult) << CubeResult.Message;

	Durin::FAssetPath CubePath;
	Durin::FAssetPath ActorPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/SkyBoxAssetTests/Cube", CubePath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/SkyBoxAssetTests/Actor", ActorPath));
	Durin::ASkyBoxActor* Actor = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(ActorPath, Actor));
	Actor->GetSkyBoxComponent()->SetTextureCube(CubeResult.Asset);
	ASSERT_TRUE(Durin::Asset::SavePackage(Actor->GetPackage()));

	const Durin::Asset::FAssetData* ActorData = Durin::Asset::GetAssetRegistry().FindAsset(ActorPath);
	ASSERT_NE(ActorData, nullptr);
	EXPECT_NE(std::ranges::find(ActorData->Dependencies, CubePath), ActorData->Dependencies.end());

	ASSERT_TRUE(Durin::Asset::UnloadPackage(ActorPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(CubePath));
	Durin::ASkyBoxActor* LoadedActor = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(ActorPath, LoadedActor));
	ASSERT_NE(LoadedActor, nullptr);
	ASSERT_NE(LoadedActor->GetSkyBoxComponent()->GetTextureCube(), nullptr);
	EXPECT_EQ(LoadedActor->GetSkyBoxComponent()->GetTextureCube()->GetName(), "Cube");

	ASSERT_TRUE(Durin::Asset::DeleteAsset(ActorPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(CubePath));
}
