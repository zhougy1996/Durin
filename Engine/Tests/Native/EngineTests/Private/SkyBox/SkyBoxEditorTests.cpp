#include "NativeAssetTestSupport.h"
#include "SkyBoxTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "Texture/TextureCubeFactoryTestSupport.h"
#include "Editor/Transaction.h"
#include "Math/Operations.h"
#include "SkyBoxPlacement.h"

TEST(FSkyBoxEditorWorkflowTests, ImportsCreatesAssignsAndPersistsAcrossReload)
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

	Durin::Testing::TFactoryImportResult<Durin::DTextureCube> CubeResult = Durin::AssetForge::Builtins::ImportTextureCubeFacesForTest(
		GetSkyBoxConventionFaces(), "/SkyBoxAssetTests/EditorWorkflowCube");
	ASSERT_TRUE(CubeResult) << CubeResult.Message;
	Durin::FPackagePath CubePath;
	Durin::FPackagePath LevelPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkyBoxAssetTests/EditorWorkflowCube", CubePath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkyBoxAssetTests/EditorWorkflowLevel", LevelPath));

	Durin::DLevel* Level = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(LevelPath, Level));
	Durin::Tests::FTestTransactorOwner Transactions;
	const Durin::Editor::Level::FSkyBoxPlacementResult Placement =
		Durin::Editor::Level::FSkyBoxPlacement::PlaceTextureCube(
			*Level, CubeResult.Asset, "Sky", Transactions.Get());
	ASSERT_TRUE(Placement) << Placement.Message;
	EXPECT_TRUE(Placement.bChanged);
	auto* Actor = Durin::Cast<Durin::ASkyBoxActor>(Placement.Actor);
	ASSERT_NE(Actor, nullptr);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Level->FindActorByName("Sky"), nullptr);
	ASSERT_TRUE(Transactions->Redo());
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
	ASSERT_TRUE(Durin::SavePackage(Level->GetPackage()));
	Transactions->Reset();
	ASSERT_TRUE(Durin::UnloadPackage(LevelPath));
	ASSERT_TRUE(Durin::UnloadPackage(CubePath));

	Durin::DLevel* LoadedLevel = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(LevelPath), LoadedLevel));
	auto* LoadedActor = Durin::Cast<Durin::ASkyBoxActor>(LoadedLevel->FindActorByName("Sky"));
	ASSERT_NE(LoadedActor, nullptr);
	Durin::DSkyBoxComponent* LoadedComponent = LoadedActor->GetSkyBoxComponent();
	ASSERT_NE(LoadedComponent, nullptr);
	ASSERT_NE(LoadedComponent->GetTextureCube(), nullptr);
	EXPECT_EQ(LoadedComponent->GetTint(), (Durin::FLinearColor(0.25f, 0.5f, 0.75f, 1.0f)));
	EXPECT_FLOAT_EQ(LoadedComponent->GetIntensity(), 2.5f);
	EXPECT_EQ(LoadedComponent->GetWorldRotation(), Transform.Rotation);

	auto* World = Durin::NewObject<Durin::DWorld>(nullptr, "SkyBoxEditorWorkflowWorld");
	EXPECT_TRUE(World->InitializeSubsystems());
	World->SetRenderScene(Scene);
	ASSERT_TRUE(World->SetCurrentLevel(LoadedLevel));
	FSkyBoxObservation Observation = ObserveSkyBoxes(*Scene);
	ASSERT_TRUE(Observation.bHasSkyBox);
	EXPECT_EQ(Observation.Count, 1u);
	EXPECT_EQ(
		Observation.Data.TextureReference,
		LoadedComponent->GetTextureCube()->GetTextureReferenceRHI());

	const Durin::Editor::Level::FSkyBoxPlacementResult ExistingPlacement =
		Durin::Editor::Level::FSkyBoxPlacement::PlaceTextureCube(
			*LoadedLevel, LoadedComponent->GetTextureCube(), "RejectedSky", nullptr);
	EXPECT_TRUE(ExistingPlacement);
	EXPECT_EQ(ExistingPlacement.Actor, LoadedActor);
	EXPECT_FALSE(ExistingPlacement.bChanged);
	EXPECT_EQ(LoadedLevel->FindActorByName("RejectedSky"), nullptr);

	ASSERT_TRUE(World->SetCurrentLevel(nullptr));
	World->SetRenderScene(nullptr);
	Durin::FlushRenderingCommands();
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
	Engine.ResetTestScene();
	Durin::GEngine = nullptr;
	ASSERT_TRUE(Durin::UnloadPackage(
		LevelPath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Durin::UnloadPackage(CubePath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(LevelPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(CubePath));
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

	Durin::Testing::TFactoryImportResult<Durin::DTextureCube> CubeResult = Durin::AssetForge::Builtins::ImportTextureCubePanoramaForTest(
		Panorama.generic_string(), "/SkyBoxAssetTests/PanoramaWorkflowCube", Settings);
	ASSERT_TRUE(CubeResult) << CubeResult.Message;
	Durin::FPackagePath CubePath;
	Durin::FPackagePath LevelPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkyBoxAssetTests/PanoramaWorkflowCube", CubePath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkyBoxAssetTests/PanoramaWorkflowLevel", LevelPath));

	Durin::DLevel* Level = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(LevelPath, Level));
	auto* Actor = Level->SpawnActor<Durin::ASkyBoxActor>("PanoramaSky");
	ASSERT_NE(Actor, nullptr);
	Durin::Tests::FTestTransactorOwner Transactions;
	const Durin::Editor::Level::FSkyBoxPlacementResult Placement =
		Durin::Editor::Level::FSkyBoxPlacement::PlaceTextureCube(
			*Level, CubeResult.Asset, "UnusedName", Transactions.Get());
	ASSERT_TRUE(Placement) << Placement.Message;
	EXPECT_EQ(Placement.Actor, Actor);
	EXPECT_TRUE(Placement.bChanged);
	EXPECT_EQ(Actor->GetSkyBoxComponent()->GetTextureCube(), CubeResult.Asset);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Actor->GetSkyBoxComponent()->GetTextureCube(), nullptr);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Actor->GetSkyBoxComponent()->GetTextureCube(), CubeResult.Asset);
	ASSERT_TRUE(Durin::SavePackage(Level->GetPackage()));
	Transactions->Reset();
	ASSERT_TRUE(Durin::UnloadPackage(LevelPath));
	ASSERT_TRUE(Durin::UnloadPackage(CubePath));

	Durin::DLevel* LoadedLevel = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(LevelPath), LoadedLevel));
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
	EXPECT_TRUE(World->InitializeSubsystems());
	World->SetRenderScene(Scene);
	ASSERT_TRUE(World->SetCurrentLevel(LoadedLevel));
	const FSkyBoxObservation Observation = ObserveSkyBoxes(*Scene);
	ASSERT_TRUE(Observation.bHasSkyBox);
	EXPECT_EQ(
		Observation.Data.TextureReference,
		LoadedCube->GetTextureReferenceRHI());

	ASSERT_TRUE(World->SetCurrentLevel(nullptr));
	World->SetRenderScene(nullptr);
	Durin::FlushRenderingCommands();
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
	Engine.ResetTestScene();
	Durin::GEngine = nullptr;
	ASSERT_TRUE(Durin::UnloadPackage(LevelPath));
	ASSERT_TRUE(Durin::UnloadPackage(CubePath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(LevelPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(CubePath));
	Durin::ShutdownRenderingThread();
}
