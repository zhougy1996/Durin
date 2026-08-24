#include "SkyBoxTestSupport.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "Editor/Transaction.h"
#include "Math/Operations.h"
#include "SkyBoxLevelAuthoring.h"

TEST(FSkyBoxEditorWorkflowTests, ImportsCreatesAssignsSavesReloadsAndReportsConflicts)
{
	InitializeSkyBoxAssetMount();
	Durin::InitRenderingThread();
	FSkyBoxTestEngine Engine;
	Durin::FScene* Scene = Engine.CreateTestScene();
	Durin::GEngine = &Engine;

	Durin::AssetForge::Builtins::FTextureCubeImportValidation Validation = Durin::AssetForge::Builtins::ValidateTextureCubeFaces(
		GetSkyBoxConventionFaces());
	ASSERT_TRUE(Validation) << Validation.Message;
	EXPECT_EQ(Validation.Dimension, 128u);
	EXPECT_EQ(Validation.MipCount, 8u);

	Durin::AssetForge::Builtins::FTextureCubeImportResult CubeResult = Durin::AssetForge::Builtins::ImportTextureCubeFaces(
		GetSkyBoxConventionFaces(), "/SkyBoxAssetTests/EditorWorkflowCube");
	ASSERT_TRUE(CubeResult) << CubeResult.Message;
	Durin::FAssetPath CubePath;
	Durin::FAssetPath LevelPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/SkyBoxAssetTests/EditorWorkflowCube", CubePath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/SkyBoxAssetTests/EditorWorkflowLevel", LevelPath));

	Durin::DLevel* Level = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(LevelPath, Level));
	Durin::Editor::FTransactionManager Transactions;
	const Durin::Editor::Level::FSkyBoxPlacementResult Placement =
		Durin::Editor::Level::FSkyBoxLevelAuthoringService::PlaceTextureCube(
			*Level, CubeResult.Asset, "Sky", &Transactions);
	ASSERT_TRUE(Placement) << Placement.Message;
	EXPECT_TRUE(Placement.bChanged);
	auto* Actor = Durin::Cast<Durin::ASkyBoxActor>(Placement.Actor);
	ASSERT_NE(Actor, nullptr);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Level->FindActorByName("Sky"), nullptr);
	ASSERT_TRUE(Transactions.Redo());
	Actor = Durin::Cast<Durin::ASkyBoxActor>(Level->FindActorByName("Sky"));
	ASSERT_NE(Actor, nullptr);
	EXPECT_EQ(Durin::ASkyBoxActor::StaticClass()->GetDisplayName(), "Sky Box Actor");
	Durin::DSkyBoxComponent* Component = Actor->GetSkyBoxComponent();
	ASSERT_NE(Component, nullptr);
	EXPECT_EQ(Component->GetTextureCube(), CubeResult.Asset);
	Component->SetTint({0.25f, 0.5f, 0.75f, 1.0f});
	Component->SetIntensity(2.5f);
	Durin::FTransform Transform = Actor->GetActorTransform();
	Transform.Rotation = Durin::Math::MakeQuaternionFromAxisAngleDegrees(35.0, Durin::FVectorConstants::Up);
	ASSERT_TRUE(Actor->SetActorTransform(Transform));
	const Durin::FGuid SavedSceneId = Component->GetSkyBoxSceneId();
	ASSERT_TRUE(Durin::Asset::SavePackage(Level->GetPackage()));
	Transactions.Clear();
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
	World->SetRenderScene(Scene);
	ASSERT_TRUE(World->SetCurrentLevel(LoadedLevel));
	FSkyBoxObservation Observation = ObserveSkyBoxes(*Scene);
	ASSERT_TRUE(Observation.bHasActive);
	EXPECT_EQ(Observation.Count, 1u);
	EXPECT_EQ(Observation.Active.SceneId, SavedSceneId);
	EXPECT_EQ(
		Observation.Active.TextureReference,
		LoadedComponent->GetTextureCube()->GetTextureReferenceRHI());

	auto* IgnoredActor = LoadedLevel->SpawnActor<Durin::ASkyBoxActor>("IgnoredSky");
	ASSERT_NE(IgnoredActor, nullptr);
	Durin::Editor::Level::FSkyBoxConflictModel ConflictModel(LoadedLevel);
	ASSERT_TRUE(ConflictModel.HasConflict());
	const Durin::Editor::Level::FSkyBoxPlacementResult ConflictPlacement =
		Durin::Editor::Level::FSkyBoxLevelAuthoringService::PlaceTextureCube(
			*LoadedLevel, LoadedComponent->GetTextureCube(), "RejectedSky", nullptr);
	EXPECT_FALSE(ConflictPlacement);
	EXPECT_EQ(LoadedLevel->FindActorByName("RejectedSky"), nullptr);
	ASSERT_EQ(ConflictModel.GetEntries().size(), 2u);
	ASSERT_NE(ConflictModel.GetActive(), nullptr);
	const auto ExpectedActive = std::min(
		std::tuple(LoadedComponent->GetSkyBoxSceneId(), LoadedComponent->GetObjectPath(), LoadedComponent->GetSkyBoxInstanceId()),
		std::tuple(IgnoredActor->GetSkyBoxComponent()->GetSkyBoxSceneId(),
			IgnoredActor->GetSkyBoxComponent()->GetObjectPath(),
			IgnoredActor->GetSkyBoxComponent()->GetSkyBoxInstanceId()));
	EXPECT_EQ(ConflictModel.GetActive()->Component->GetSkyBoxSceneId(), std::get<0>(ExpectedActive));
	IgnoredActor->SetHidden(true);
	EXPECT_FALSE(Durin::Editor::Level::FSkyBoxConflictModel(LoadedLevel).HasConflict());

	ASSERT_TRUE(World->SetCurrentLevel(nullptr));
	World->SetRenderScene(nullptr);
	Durin::FlushRenderingCommands();
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
	Engine.ResetTestScene();
	Durin::GEngine = nullptr;
	ASSERT_TRUE(Durin::Asset::UnloadPackage(
		LevelPath,
		Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(CubePath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(LevelPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(CubePath));
	Durin::ShutdownRenderingThread();
}

TEST(FSkyBoxEditorWorkflowTests, ImportsPanoramaAssignsSkyAndPersistsSettingsAcrossLevelReload)
{
	InitializeSkyBoxAssetMount();
	Durin::InitRenderingThread();
	FSkyBoxTestEngine Engine;
	Durin::FScene* Scene = Engine.CreateTestScene();
	Durin::GEngine = &Engine;

	const std::filesystem::path Panorama = std::filesystem::path(DURIN_TEST_DATA_DIR) /
		"EquirectangularPanorama" / "AnalyticalHDR.hdr";
	const Durin::AssetForge::Builtins::FTextureCubePanoramaImportSettings Settings{
		.FaceDimension = 2,
		.ExposureEV = 1.0f};
	const Durin::AssetForge::Builtins::FTextureCubeImportValidation Validation =
		Durin::AssetForge::Builtins::ValidateTextureCubePanorama(Panorama.generic_string(), Settings);
	ASSERT_TRUE(Validation) << Validation.Message;
	EXPECT_TRUE(Validation.bHDR);
	EXPECT_EQ(Validation.SourceWidth, 8u);
	EXPECT_EQ(Validation.SourceHeight, 4u);
	EXPECT_EQ(Validation.Dimension, 2u);

	Durin::AssetForge::Builtins::FTextureCubeImportResult CubeResult = Durin::AssetForge::Builtins::ImportTextureCubePanorama(
		Panorama.generic_string(), "/SkyBoxAssetTests/PanoramaWorkflowCube", Settings);
	ASSERT_TRUE(CubeResult) << CubeResult.Message;
	Durin::FAssetPath CubePath;
	Durin::FAssetPath LevelPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/SkyBoxAssetTests/PanoramaWorkflowCube", CubePath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/SkyBoxAssetTests/PanoramaWorkflowLevel", LevelPath));

	Durin::DLevel* Level = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(LevelPath, Level));
	auto* Actor = Level->SpawnActor<Durin::ASkyBoxActor>("PanoramaSky");
	ASSERT_NE(Actor, nullptr);
	Durin::Editor::FTransactionManager Transactions;
	const Durin::Editor::Level::FSkyBoxPlacementResult Placement =
		Durin::Editor::Level::FSkyBoxLevelAuthoringService::PlaceTextureCube(
			*Level, CubeResult.Asset, "UnusedName", &Transactions);
	ASSERT_TRUE(Placement) << Placement.Message;
	EXPECT_EQ(Placement.Actor, Actor);
	EXPECT_TRUE(Placement.bChanged);
	EXPECT_EQ(Actor->GetSkyBoxComponent()->GetTextureCube(), CubeResult.Asset);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Actor->GetSkyBoxComponent()->GetTextureCube(), nullptr);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Actor->GetSkyBoxComponent()->GetTextureCube(), CubeResult.Asset);
	ASSERT_TRUE(Durin::Asset::SavePackage(Level->GetPackage()));
	Transactions.Clear();
	ASSERT_TRUE(Durin::Asset::UnloadPackage(LevelPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(CubePath));

	Durin::DLevel* LoadedLevel = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(LevelPath, LoadedLevel));
	auto* LoadedActor = Durin::Cast<Durin::ASkyBoxActor>(
		LoadedLevel->FindActorByName("PanoramaSky"));
	ASSERT_NE(LoadedActor, nullptr);
	Durin::DTextureCube* LoadedCube = LoadedActor->GetSkyBoxComponent()->GetTextureCube();
	ASSERT_NE(LoadedCube, nullptr);
	EXPECT_EQ(LoadedCube->GetSourceLayout(), Durin::ETextureCubeSourceLayout::EquirectangularPanorama);
	EXPECT_EQ(LoadedCube->GetOriginalSourceWidth(), 8u);
	EXPECT_EQ(LoadedCube->GetOriginalSourceHeight(), 4u);
	EXPECT_EQ(LoadedCube->GetPanoramaFaceDimension(), 2u);
	EXPECT_FLOAT_EQ(LoadedCube->GetPanoramaExposureEV(), 1.0f);
	EXPECT_EQ(LoadedCube->GetBuiltPixelFormat(), Durin::EPixelFormat::BC1_UNORM_SRGB);

	auto* World = Durin::NewObject<Durin::DWorld>(nullptr, "PanoramaWorkflowWorld");
	World->SetRenderScene(Scene);
	ASSERT_TRUE(World->SetCurrentLevel(LoadedLevel));
	const FSkyBoxObservation Observation = ObserveSkyBoxes(*Scene);
	ASSERT_TRUE(Observation.bHasActive);
	EXPECT_EQ(
		Observation.Active.TextureReference,
		LoadedCube->GetTextureReferenceRHI());

	ASSERT_TRUE(World->SetCurrentLevel(nullptr));
	World->SetRenderScene(nullptr);
	Durin::FlushRenderingCommands();
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
	Engine.ResetTestScene();
	Durin::GEngine = nullptr;
	ASSERT_TRUE(Durin::Asset::UnloadPackage(LevelPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(CubePath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(LevelPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(CubePath));
	Durin::ShutdownRenderingThread();
}
