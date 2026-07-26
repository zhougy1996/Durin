#include "SkyBoxTestSupport.h"

TEST(FSkyBoxEditorWorkflowTests, ImportsCreatesAssignsSavesReloadsAndReportsConflicts)
{
	InitializeSkyBoxAssetMount();
	Durin::InitRenderingThread();
	FSkyBoxTestEngine Engine;
	Durin::FScene* Scene = Engine.CreateTestScene();
	Durin::GEngine = &Engine;

	Durin::FTextureCubeImportValidation Validation = Durin::DTextureCube::ValidateImportSources(
		GetSkyBoxConventionFaces());
	ASSERT_TRUE(Validation) << Validation.Message;
	EXPECT_EQ(Validation.Dimension, 128u);
	EXPECT_EQ(Validation.MipCount, 8u);

	Durin::FTextureCubeImportResult CubeResult = Durin::DTextureCube::ImportAsset(
		GetSkyBoxConventionFaces(), "/SkyBoxAssetTests/EditorWorkflowCube");
	ASSERT_TRUE(CubeResult) << CubeResult.Message;
	Durin::FAssetPath CubePath;
	Durin::FAssetPath LevelPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/SkyBoxAssetTests/EditorWorkflowCube", CubePath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/SkyBoxAssetTests/EditorWorkflowLevel", LevelPath));

	Durin::DLevel* Level = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(LevelPath, Level));
	auto* Actor = Durin::Cast<Durin::ASkyBoxActor>(
		Level->SpawnActor(Durin::ASkyBoxActor::StaticClass(), "Sky"));
	ASSERT_NE(Actor, nullptr);
	EXPECT_EQ(Durin::ASkyBoxActor::StaticClass()->GetDisplayName(), "Sky Box Actor");
	Durin::DSkyBoxComponent* Component = Actor->GetSkyBoxComponent();
	ASSERT_NE(Component, nullptr);
	Component->SetTextureCube(CubeResult.Asset);
	Component->SetTint({0.25f, 0.5f, 0.75f, 1.0f});
	Component->SetIntensity(2.5f);
	Durin::FTransform Transform = Actor->GetActorTransform();
	Transform.Rotation = glm::angleAxis(glm::radians(35.0), Durin::FVectorConstants::Up);
	ASSERT_TRUE(Actor->SetActorTransform(Transform));
	const Durin::FGuid SavedSceneId = Component->GetSkyBoxSceneId();
	ASSERT_TRUE(Durin::Asset::SavePackage(Level->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(LevelPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(CubePath));

	Durin::DLevel* LoadedLevel = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(LevelPath, LoadedLevel));
	auto* LoadedActor = Durin::Cast<Durin::ASkyBoxActor>(LoadedLevel->FindActorByName("Sky"));
	ASSERT_NE(LoadedActor, nullptr);
	Durin::DSkyBoxComponent* LoadedComponent = LoadedActor->GetSkyBoxComponent();
	ASSERT_NE(LoadedComponent, nullptr);
	ASSERT_NE(LoadedComponent->GetTextureCube(), nullptr);
	EXPECT_EQ(LoadedComponent->GetSkyBoxSceneId(), SavedSceneId);
	EXPECT_EQ(LoadedComponent->GetTint(), (Durin::FLinearColor(0.25f, 0.5f, 0.75f, 1.0f)));
	EXPECT_FLOAT_EQ(LoadedComponent->GetIntensity(), 2.5f);
	EXPECT_EQ(LoadedComponent->GetWorldRotation(), Transform.Rotation);

	auto* World = Durin::NewObject<Durin::DWorld>(nullptr, "SkyBoxEditorWorkflowWorld");
	ASSERT_TRUE(World->SetCurrentLevel(LoadedLevel));
	FSkyBoxObservation Observation = ObserveSkyBoxes(*Scene);
	ASSERT_TRUE(Observation.bHasActive);
	EXPECT_EQ(Observation.Count, 1u);
	EXPECT_EQ(Observation.Active.SceneId, SavedSceneId);
	EXPECT_EQ(Observation.Active.TextureResource, LoadedComponent->GetTextureCube()->GetRenderResource());

	auto* IgnoredActor = LoadedLevel->SpawnActor<Durin::ASkyBoxActor>("IgnoredSky");
	ASSERT_NE(IgnoredActor, nullptr);
	Durin::FSkyBoxConflictModel ConflictModel(LoadedLevel);
	ASSERT_TRUE(ConflictModel.HasConflict());
	ASSERT_EQ(ConflictModel.GetEntries().size(), 2u);
	ASSERT_NE(ConflictModel.GetActive(), nullptr);
	const auto ExpectedActive = std::min(
		std::tuple(LoadedComponent->GetSkyBoxSceneId(), LoadedComponent->GetObjectPath(), LoadedComponent->GetSkyBoxInstanceId()),
		std::tuple(IgnoredActor->GetSkyBoxComponent()->GetSkyBoxSceneId(),
			IgnoredActor->GetSkyBoxComponent()->GetObjectPath(),
			IgnoredActor->GetSkyBoxComponent()->GetSkyBoxInstanceId()));
	EXPECT_EQ(ConflictModel.GetActive()->Component->GetSkyBoxSceneId(), std::get<0>(ExpectedActive));
	IgnoredActor->SetHidden(true);
	EXPECT_FALSE(Durin::FSkyBoxConflictModel(LoadedLevel).HasConflict());

	ASSERT_TRUE(World->SetCurrentLevel(nullptr));
	Durin::FlushRenderingCommands();
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
	Engine.ResetTestScene();
	Durin::GEngine = nullptr;
	ASSERT_TRUE(Durin::Asset::UnloadPackage(LevelPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(CubePath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(LevelPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(CubePath));
	Durin::ShutdownRenderingThread();
}
