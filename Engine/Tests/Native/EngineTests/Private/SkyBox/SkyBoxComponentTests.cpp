#include "Actors/ProceduralSkyActor.h"
#include "Components/ProceduralSkyComponent.h"
#include "NativeAssetTestSupport.h"
#include "SkyBoxTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "Texture/TextureCubeFactoryTestSupport.h"
#include "Math/Operations.h"
#include "Actors/SkyLightActor.h"
#include "Components/SkyLightComponent.h"

TEST(FSkyLightTests, SerializesSourceIdentityAndRejectsNonfiniteIntensity)
{
	InitializeDObjectSystem();
	auto* Actor = Durin::NewObject<Durin::ASkyLightActor>(nullptr, "AuthoredSkyLight");
	auto* Component = Actor->GetSkyLightComponent();
	ASSERT_NE(Component, nullptr);
	Component->SetIntensity(100.0f);
	EXPECT_EQ(Component->GetIntensity(), 16.0f);
	Component->SetIntensity(std::numeric_limits<float>::quiet_NaN());
	EXPECT_EQ(Component->GetIntensity(), 16.0f);
	Component->SetPriority(-2000);
	EXPECT_EQ(Component->GetPriority(), -1000);
	Component->SetSource(Durin::ESkyLightSourceMode::CapturedSky, nullptr);
	const auto Id = Component->GetPersistentId();
	EXPECT_TRUE(Id.IsValid());
	Durin::FByteBuffer Bytes;
	ASSERT_TRUE(Durin::SaveObjectGraphToMemory(Actor, Bytes));
	auto* Loaded = Durin::Cast<Durin::ASkyLightActor>(Durin::LoadObjectGraphFromMemory(Bytes));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->GetSkyLightComponent()->GetPersistentId(), Id);
	EXPECT_EQ(Loaded->GetSkyLightComponent()->GetSourceMode(), Durin::ESkyLightSourceMode::CapturedSky);
	EXPECT_EQ(Loaded->GetSkyLightComponent()->GetIntensity(), 16.0f);
	Durin::MarkObjectHierarchyAsGarbage(Actor);
	Durin::MarkObjectHierarchyAsGarbage(Loaded);
	Durin::CollectGarbage();
}

TEST(FSkyLightTests, SelectsDeterministicallyAndRetiresExactSnapshots)
{
	InitializeDObjectSystem();
	Durin::InitRenderingThread();
	FSkyBoxTestEngine Engine;
	auto* Scene = Engine.CreateTestScene();
	Durin::GEngine = &Engine;
	auto* World = Durin::NewObject<Durin::DWorld>(&Engine, "SkyLightWorld");
	ASSERT_TRUE(World->InitializeSubsystems());
	ASSERT_TRUE(World->SetCurrentLevel(Durin::NewObject<Durin::DLevel>(World, "SkyLightLevel")));
	Engine.SetWorld(World);
	auto* First = World->SpawnActor<Durin::ASkyLightActor>("First");
	auto* Second = World->SpawnActor<Durin::ASkyLightActor>("Second");
	auto* A = First->GetSkyLightComponent();
	auto* B = Second->GetSkyLightComponent();
	A->SetSource(Durin::ESkyLightSourceMode::CapturedSky, nullptr);
	B->SetSource(Durin::ESkyLightSourceMode::CapturedSky, nullptr);
	auto Observe = [&] {
		std::shared_ptr<const Durin::FSkyLightSceneProxy> Result;
		Durin::EnqueueRenderCommand<FObserveSkyBoxCommand>([&](Durin::FRHICommandListImmediate&) {
			Result = Scene->GetSkyLight_RenderThread();
		});
		Durin::FlushRenderingCommands();
		return Result;
	};
	EXPECT_FALSE(Observe());
	auto* Provider = World->SpawnActor<Durin::AProceduralSkyActor>("Provider");
	EXPECT_NE(Provider, nullptr);
	auto Selected = Observe();
	ASSERT_TRUE(Selected);
	EXPECT_EQ(Selected->PersistentId, std::min(A->GetPersistentId(), B->GetPersistentId()));
	B->SetPriority(2);
	Selected = Observe();
	ASSERT_TRUE(Selected);
	EXPECT_EQ(Selected->PersistentId, B->GetPersistentId());
	const auto Frozen = Selected;
	B->Recapture();
	Selected = Observe();
	ASSERT_TRUE(Selected);
	EXPECT_GT(Selected->RequestSerial, Frozen->RequestSerial);
	EXPECT_EQ(Selected->SourceEpoch, Frozen->SourceEpoch);
	Second->SetHidden(true);
	Selected = Observe();
	ASSERT_TRUE(Selected);
	EXPECT_EQ(Selected->PersistentId, A->GetPersistentId());
	A->SetEnabled(false);
	EXPECT_FALSE(Observe());
	EXPECT_TRUE(Frozen->bEligible);
	ASSERT_TRUE(World->SetCurrentLevel(nullptr));
	EXPECT_FALSE(Observe());
	Engine.SetWorld(nullptr);
	Engine.ResetTestScene();
	Durin::FlushRenderingCommands();
	Durin::GEngine = nullptr;
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
	Durin::ShutdownRenderingThread();
}

TEST(FSkyBoxTests, SceneAcceptsOneSkyBoxAndAppliesFifoReplacement)
{
	InitializeDObjectSystem();
	Durin::InitRenderingThread();
	Durin::FRendererModule SceneFactory;
	Durin::FScenePtr SceneOwner = SceneFactory.CreateScene();
	auto& Scene = static_cast<Durin::FScene&>(*SceneOwner);
	Durin::FSkyBoxSceneData Sky;
	Sky.Intensity = 2.0f;
	auto* SkyToken = PublishSkyBox(Scene, Sky);
	ASSERT_NE(SkyToken, nullptr);
	EXPECT_EQ(PublishSkyBox(Scene, Sky), nullptr);
	EXPECT_EQ(ObserveSkyBoxes(Scene).Count, 1u);

	EXPECT_TRUE(Durin::FSceneInterfaceTestAccess::TryRemoveSkyBoxProxy(Scene, SkyToken));
	Sky.Intensity = 4.0f;
	auto* ReplacementToken = PublishSkyBox(
		Scene, Sky);
	ASSERT_NE(ReplacementToken, nullptr);
	// The removed proxy may already be destroyed and its address reused.
	// Replacement is established by the single live proxy and new data below.

	FSkyBoxObservation Observation = ObserveSkyBoxes(Scene);
	ASSERT_TRUE(Observation.bHasSkyBox);
	EXPECT_EQ(Observation.Count, 1u);
	EXPECT_EQ(Observation.Data.Intensity, 4.0f);
	Durin::FSceneInterfaceTestAccess::TryRemoveSkyBoxProxy(Scene, ReplacementToken);

	Durin::FSceneInterfaceTestAccess::ReleaseScene(SceneOwner);
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
}

TEST(FSkyBoxTests, ActorDefaultsSerializeAndRetainCubeReference)
{
	InitializeDObjectSystem();
	auto* Actor = Durin::NewObject<Durin::ASkyBoxActor>(nullptr, "SkyBoxActor");
	Durin::DSkyBoxComponent* Component = Actor->GetSkyBoxComponent();
	auto* Cube = Durin::NewObject<Durin::DTextureCube>(nullptr, "ReferencedCube");
	ASSERT_NE(Component, nullptr);

	Component->SetTextureCube(Cube);
	Component->SetTint(Durin::FLinearColor(0.25f, 0.5f, 0.75f, 1.0f));
	Component->SetIntensity(-2.0f);
	EXPECT_EQ(Component->GetIntensity(), 0.0f);

	Durin::AddToRoot(Actor);
	const Durin::FObjectHandle CubeHandle = Durin::MakeObjectHandle(Cube);
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::ResolveObjectHandle(CubeHandle), Cube);
	Component->SetTextureCube(nullptr);

	Durin::FByteBuffer Bytes;
	ASSERT_TRUE(Durin::SaveObjectGraphToMemory(Actor, Bytes));
	auto* LoadedActor = Durin::Cast<Durin::ASkyBoxActor>(Durin::LoadObjectGraphFromMemory(Bytes));
	ASSERT_NE(LoadedActor, nullptr);
	Durin::DSkyBoxComponent* LoadedComponent = LoadedActor->GetSkyBoxComponent();
	ASSERT_NE(LoadedComponent, nullptr);
	EXPECT_EQ(LoadedComponent->GetTextureCube(), nullptr);
	EXPECT_EQ(LoadedComponent->GetTint(), (Durin::FLinearColor(0.25f, 0.5f, 0.75f, 1.0f)));
	EXPECT_EQ(LoadedComponent->GetIntensity(), 0.0f);

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
	EXPECT_TRUE(World->InitializeSubsystems());
	ASSERT_TRUE(World->SetCurrentLevel(
		Durin::NewObject<Durin::DLevel>(World, "LiveSkyBoxLevel")));
	Engine.SetWorld(World);
	auto* Actor = World->SpawnActor<Durin::ASkyBoxActor>("LiveSkyBoxActor");
	Durin::DSkyBoxComponent* Component = Actor->GetSkyBoxComponent();
	auto* Cube = Durin::NewObject<Durin::DTextureCube>(nullptr, "LiveSkyBoxCube");

	Component->RegisterComponent();
	FSkyBoxObservation Observation = ObserveSkyBoxes(*Scene);
	ASSERT_TRUE(Observation.bHasSkyBox);
	EXPECT_EQ(Observation.Count, 1u);

	const Durin::FQuat Rotation = Durin::Math::MakeQuaternionFromAxisAngleDegrees(
		45.0, Durin::FVectorConstants::Up);
	Component->SetTextureCube(Cube);
	Component->SetTint(Durin::FLinearColor(0.2f, 0.4f, 0.6f, 1.0f));
	Component->SetIntensity(3.0f);
	Component->SetWorldRotation(Rotation);
	Observation = ObserveSkyBoxes(*Scene);
	ASSERT_TRUE(Observation.bHasSkyBox);
	EXPECT_EQ(
		Observation.Data.TextureReference, Cube->GetTextureReferenceRHI());
	EXPECT_EQ(Observation.Data.Rotation, Rotation);
	EXPECT_EQ(Observation.Data.Tint, Durin::FVector3f(0.2f, 0.4f, 0.6f));
	EXPECT_EQ(Observation.Data.Intensity, 3.0f);

	Actor->SetHidden(true);
	EXPECT_FALSE(ObserveSkyBoxes(*Scene).bHasSkyBox);
	Actor->SetHidden(false);
	EXPECT_TRUE(ObserveSkyBoxes(*Scene).bHasSkyBox);
	Component->UnregisterComponent();
	EXPECT_FALSE(ObserveSkyBoxes(*Scene).bHasSkyBox);

	Engine.SetWorld(nullptr);
	Engine.ResetTestScene();
	Durin::FlushRenderingCommands();
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
	Durin::FRendererModule SceneFactory;
	auto AuxiliaryScene = SceneFactory.CreateScene();
	Durin::GEngine = &Engine;

	auto* World = Durin::NewObject<Durin::DWorld>(&Engine, "AuxiliarySkyBoxWorld");
	EXPECT_TRUE(World->InitializeSubsystems());
	World->SetRenderScene(AuxiliaryScene.get());
	ASSERT_TRUE(World->SetCurrentLevel(
		Durin::NewObject<Durin::DLevel>(World, "AuxiliarySkyBoxLevel")));
	ASSERT_NE(World->SpawnActor<Durin::ASkyBoxActor>("AuxiliarySkyBox"), nullptr);
	EXPECT_FALSE(ObserveSkyBoxes(*MainScene).bHasSkyBox);
	EXPECT_TRUE(ObserveSkyBoxes(*AuxiliaryScene).bHasSkyBox);

	Durin::GEngine = nullptr;
	ASSERT_TRUE(World->SetCurrentLevel(nullptr));
	EXPECT_FALSE(ObserveSkyBoxes(*AuxiliaryScene).bHasSkyBox);
	World->SetRenderScene(nullptr);

	Durin::FSceneInterfaceTestAccess::ReleaseScene(AuxiliaryScene);
	Engine.ResetTestScene();
	Durin::FlushRenderingCommands();
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
	Durin::ShutdownRenderingThread();
}

TEST(FSkyBoxTests, PackageTracksAndReloadsCubeAssetDependency)
{
	InitializeSkyBoxAssetMount();
	Durin::Testing::TFactoryImportResult<Durin::DTextureCube> CubeResult = Durin::AssetForge::Builtins::ImportTextureCubeFacesForTest(
		GetSkyBoxConventionFaces(), "/SkyBoxAssetTests/Cube");
	ASSERT_TRUE(CubeResult) << CubeResult.Message;

	Durin::FPackagePath CubePath;
	Durin::FPackagePath ActorPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkyBoxAssetTests/Cube", CubePath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkyBoxAssetTests/Actor", ActorPath));
	Durin::ASkyBoxActor* Actor = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(ActorPath, Actor));
	Actor->GetSkyBoxComponent()->SetTextureCube(CubeResult.Asset);
	ASSERT_TRUE(Durin::SavePackage(Actor->GetPackage()));

	const Durin::FAssetCatalogEntry ActorData = Durin::FindAssetExact(ActorPath);
	ASSERT_NE(ActorData, nullptr);
	EXPECT_NE(std::ranges::find(ActorData->Dependencies, CubePath), ActorData->Dependencies.end());

	ASSERT_TRUE(Durin::UnloadPackage(ActorPath));
	ASSERT_TRUE(Durin::UnloadPackage(CubePath));
	Durin::ASkyBoxActor* LoadedActor = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(ActorPath), LoadedActor));
	ASSERT_NE(LoadedActor, nullptr);
	ASSERT_NE(LoadedActor->GetSkyBoxComponent()->GetTextureCube(), nullptr);
	EXPECT_EQ(LoadedActor->GetSkyBoxComponent()->GetTextureCube()->GetName(), "Cube");

	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(ActorPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(CubePath));
}
