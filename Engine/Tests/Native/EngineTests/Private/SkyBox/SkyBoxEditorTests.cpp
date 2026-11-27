#include "NativeAssetRuntimeTestSupport.h"
#include "AssetRegistry/Scan.h"
#include "Misc/UnifiedError.h"
#include "NativeAssetTestSupport.h"
#include "NativePackageUnloadTestSupport.h"
#include "SkyBoxTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "Texture/TextureCubeFactoryTestSupport.h"
#include "Transactions/Transaction.h"
#include "Math/Operations.h"
#include "Preview/PreviewMeshPaths.h"
#include "Editor/EditorActorSubsystemTestSupport.h"
#include "DObject/StrongObjectPtr.h"
#include "Texture/TextureCube.h"
#include "Materials/Material.h"

TEST(FSkyBoxEditorWorkflowTests, TextureAssignmentComposesWithOtherComponentFieldsThroughGenericHistory)
{
	using namespace Durin;
	Tests::FActorEditorFixture Editor;
	auto* Transactions = Editor.GetTransactions();
	auto* Level = Editor.GetWorld()->GetCurrentLevel();
	TStrongObjectPtr<DTextureCube> Before(NewObject<DTextureCube>(nullptr, "SkyBefore")), After(NewObject<DTextureCube>(nullptr, "SkyAfter"));
	auto* Actor = Level->SpawnActor<ASkyBoxActor>("Sky"); ASSERT_NE(Actor, nullptr);
	auto* Component = Actor->GetSkyBoxComponent(); Component->SetTextureCube(Before.Get());
	TStrongObjectPtr<DMaterial> Override(NewObject<DMaterial>(nullptr, "SkyOverride"));
	{
		Durin::FScopedTransaction Scope("test", "Texture and intensity", nullptr);
		Component->SetFlags(EObjectFlags::Transactional);
		ASSERT_TRUE(Component->Modify(false));
		Component->SetTextureCube(After.Get());
		Component->SetIntensity(3.0f);
		Component->SetMaterialOverride(Override.Get());
	}
	EXPECT_EQ(Transactions->GetTransaction(0)->GetRecordCount(), 1u);
	EXPECT_FALSE(Transactions->GetTransaction(0)->IsOwnedByModule("LevelEditor"));
	ASSERT_TRUE(Transactions->Undo()); EXPECT_EQ(Component->GetTextureCube(), Before.Get()); EXPECT_FLOAT_EQ(Component->GetIntensity(), 1.0f);
	EXPECT_EQ(Component->GetMaterialOverride(), nullptr);
	ASSERT_TRUE(Transactions->Redo()); EXPECT_EQ(Component->GetMaterialOverride(), Override.Get()); EXPECT_EQ(Component->GetTextureCube(), After.Get()); EXPECT_FLOAT_EQ(Component->GetIntensity(), 3.0f);
}

TEST(FSkyBoxEditorWorkflowTests, RedoRestoresActorWithEmptyTextureAfterAssetRetirement)
{
	using namespace Durin;
	Testing::InitializeAssetSystemForTests();
	Testing::RegisterMountPointForTests("/Engine/", FPaths::EngineContentDir());
	ASSERT_TRUE(RefreshAssetRegistry());
	Tests::FActorEditorFixture Editor;
	auto* Transactions = Editor.GetTransactions();
	auto* Level = Editor.GetWorld()->GetCurrentLevel();
	TStrongObjectPtr<DTextureCube> Cube(NewObject<DTextureCube>(nullptr, "PreparedSkyCube"));
	InitRenderingThread();
	FSkyBoxTestEngine Engine;
	auto* Scene = Engine.CreateTestScene();
	GEngine = &Engine;
	auto* World = Editor.GetWorld();
	World->SetRenderScene(Scene);
	// Keep scene teardown outside the assertions that exercise replay.
	auto CheckReplay = [&] {
		auto* Placement = Editor.GetActorEditor()->SpawnActor(*Level, ASkyBoxActor::StaticClass(), "Sky", {}, {false},
			[&](AActor& Actor) { static_cast<ASkyBoxActor&>(Actor).GetSkyBoxComponent()->SetTextureCube(Cube.Get()); });
		ASSERT_NE(Placement, nullptr);
		ASSERT_EQ(Transactions->GetQueueLength(), 1u);
		EXPECT_FALSE(Transactions->GetTransaction(0)->IsOwnedByModule("DurinEd"));
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(Level->FindActorByName("Sky"), nullptr);
		Cube->MarkAsGarbage();
		ASSERT_TRUE(Transactions->Redo());
		ASSERT_EQ(Level->FindActorByName("Sky"), Placement);
		EXPECT_EQ(Cast<ASkyBoxActor>(Placement)->GetSkyBoxComponent()->GetTextureCube(), nullptr);

	};
	CheckReplay();
	World->SetCurrentLevel(nullptr);
	World->SetRenderScene(nullptr);
	FlushRenderingCommands();
	Engine.ResetTestScene();
	GEngine = nullptr;
	ShutdownRenderingThread();
}

TEST(FSkyBoxEditorWorkflowTests, ImportsCreatesAssignsAndPersistsAcrossReload)
{
	InitializeSkyBoxAssetMount();
	Durin::Testing::RegisterMountPointForTests("/Engine/", Durin::FPaths::EngineContentDir());
	ASSERT_TRUE(Durin::RefreshAssetRegistry());
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
	Durin::Tests::FActorEditorFixture Editor;
	ASSERT_TRUE(Editor.GetWorld()->SetCurrentLevel(Level));
	auto* Transactions = Editor.GetTransactions();
	Durin::FSoftObjectPath SpherePath;
	ASSERT_TRUE(Durin::FSoftObjectPath::TryCreate(Durin::PreviewMeshPaths::SphereAssetPath, SpherePath));
	auto* Sphere = Durin::LoadObject<Durin::DStaticMesh>(SpherePath);
	ASSERT_NE(Sphere, nullptr);
	auto* Actor = Durin::Cast<Durin::ASkyBoxActor>(Editor.GetActorEditor()->SpawnActor(*Level,
		Durin::ASkyBoxActor::StaticClass(), "Sky", {}, {false}, [&](Durin::AActor& Created) {
			auto* Sky = static_cast<Durin::ASkyBoxActor&>(Created).GetSkyBoxComponent();
			Sky->SetStaticMesh(Sphere);
			Sky->SetTextureCube(CubeResult.Asset);
		}));
	ASSERT_NE(Actor, nullptr);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Level->FindActorByName("Sky"), nullptr);
	auto* Conflict = Level->SpawnActor<Durin::ASkyBoxActor>("Sky");
	ASSERT_NE(Conflict, nullptr);
	EXPECT_NE(Conflict->GetFName(), Actor->GetFName());
	EXPECT_EQ(Conflict->GetActorLabel(), "Sky");
	const auto ReservedName = Conflict->GetFName();
	Conflict->Rename("Sky");
	EXPECT_EQ(Conflict->GetFName(), ReservedName);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Level->DestroyActor(Conflict));
	EXPECT_EQ(Level->FindActorByName("Sky"), Actor);
	Actor = Durin::Cast<Durin::ASkyBoxActor>(Level->FindActorByName("Sky"));
	ASSERT_NE(Actor, nullptr);
	EXPECT_EQ(Durin::ASkyBoxActor::StaticClass()->GetDisplayName(), "Sky Box Actor");
	Durin::DSkyBoxComponent* Component = Actor->GetSkyBoxComponent();
	ASSERT_NE(Component, nullptr);
	EXPECT_EQ(Component->GetTextureCube(), CubeResult.Asset);
	Component->SetTint({0.25f, 0.5f, 0.75f, 1.0f});
	Component->SetIntensity(2.5f);
	Durin::FPackagePath MaterialPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkyBoxAssetTests/AuthoredSkyMaterial", MaterialPath));
	Durin::DMaterial* Override = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(MaterialPath, Override));
	ASSERT_TRUE(Durin::SavePackage(Override->GetPackage()));
	Component->SetMaterialOverride(Override);
	Durin::FTransform Transform = Actor->GetActorTransform();
	Transform.Rotation = Durin::Math::MakeQuaternionFromAxisAngleDegrees(35.0, Durin::FVectorConstants::Up);
	ASSERT_TRUE(Actor->SetActorTransform(Transform));
	ASSERT_TRUE(Durin::SavePackage(Level->GetPackage()));
	Transactions->Reset();
	ASSERT_TRUE(Editor.GetWorld()->SetCurrentLevel(nullptr));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(LevelPath));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(CubePath));

	Durin::DLevel* LoadedLevel = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedValueError;
		auto LoadedValue = Durin::LoadObject<Durin::DLevel>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(LevelPath), nullptr, &LoadedValueError);
		LoadedLevel = LoadedValue;
		ASSERT_TRUE(LoadedValue);
	}
	auto* LoadedActor = Durin::Cast<Durin::ASkyBoxActor>(LoadedLevel->FindActorByName("Sky"));
	ASSERT_NE(LoadedActor, nullptr);
	Durin::DSkyBoxComponent* LoadedComponent = LoadedActor->GetSkyBoxComponent();
	ASSERT_NE(LoadedComponent, nullptr);
	ASSERT_NE(LoadedComponent->GetStaticMesh(), nullptr);
	EXPECT_EQ(LoadedComponent->GetStaticMesh()->GetName(), "Sphere");
	ASSERT_NE(LoadedComponent->GetTextureCube(), nullptr);
	ASSERT_NE(LoadedComponent->GetMaterialOverride(), nullptr);
	EXPECT_EQ(LoadedComponent->GetMaterialOverride(), Durin::LoadObject<Durin::DMaterial>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath)));
	EXPECT_EQ(LoadedComponent->GetTint(), (Durin::FLinearColor(0.25f, 0.5f, 0.75f, 1.0f)));
	EXPECT_FLOAT_EQ(LoadedComponent->GetIntensity(), 2.5f);
	EXPECT_EQ(LoadedComponent->GetWorldRotation(), Transform.Rotation);

	auto* World = Durin::NewObject<Durin::DWorld>(nullptr, "SkyBoxEditorWorkflowWorld");
	EXPECT_NO_THROW(World->InitializeSubsystems());
	World->SetRenderScene(Scene);
	ASSERT_TRUE(World->SetCurrentLevel(LoadedLevel));
	EXPECT_TRUE(LoadedComponent->IsRegistered());


	ASSERT_TRUE(World->SetCurrentLevel(nullptr));
	World->SetRenderScene(nullptr);
	Durin::FlushRenderingCommands();
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	Engine.ResetTestScene();
	Durin::GEngine = nullptr;
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(
		LevelPath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(CubePath));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(MaterialPath, Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(LevelPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(MaterialPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(CubePath));
	Durin::ShutdownRenderingThread();
}

TEST(FSkyBoxEditorWorkflowTests, ImportsPanoramaAssignsSkyAndPersistsSettingsAcrossLevelReload)
{
	InitializeSkyBoxAssetMount();
	Durin::Testing::RegisterMountPointForTests("/Engine/", Durin::FPaths::EngineContentDir());
	ASSERT_TRUE(Durin::RefreshAssetRegistry());
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
	Durin::Tests::FActorEditorFixture Editor;
	ASSERT_TRUE(Editor.GetWorld()->SetCurrentLevel(Level));
	auto* Transactions = Editor.GetTransactions();
	auto* Component = Actor->GetSkyBoxComponent();
	{
		Durin::FScopedTransaction Scope("test", "Assign cubemap", Component);
		Component->SetFlags(Durin::EObjectFlags::Transactional);
		ASSERT_TRUE(Component->Modify(false));
		Component->SetTextureCube(CubeResult.Asset);
	}
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Component->GetTextureCube(), nullptr);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Component->GetTextureCube(), CubeResult.Asset);
	ASSERT_TRUE(Durin::SavePackage(Level->GetPackage()));
	Transactions->Reset();
	ASSERT_TRUE(Editor.GetWorld()->SetCurrentLevel(nullptr));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(LevelPath));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(CubePath));

	Durin::DLevel* LoadedLevel = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedValueError;
		auto LoadedValue = Durin::LoadObject<Durin::DLevel>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(LevelPath), nullptr, &LoadedValueError);
		LoadedLevel = LoadedValue;
		ASSERT_TRUE(LoadedValue);
	}
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
	ASSERT_TRUE(LoadedCube->FinishCachePlatformData());
	EXPECT_EQ(LoadedCube->GetPixelFormat(), Durin::EPixelFormat::BC1_UNORM_SRGB);

	auto* World = Durin::NewObject<Durin::DWorld>(nullptr, "PanoramaWorkflowWorld");
	EXPECT_NO_THROW(World->InitializeSubsystems());
	World->SetRenderScene(Scene);
	ASSERT_TRUE(World->SetCurrentLevel(LoadedLevel));
	EXPECT_TRUE(LoadedActor->GetSkyBoxComponent()->IsRegistered());

	ASSERT_TRUE(World->SetCurrentLevel(nullptr));
	World->SetRenderScene(nullptr);
	Durin::FlushRenderingCommands();
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	Engine.ResetTestScene();
	Durin::GEngine = nullptr;
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(LevelPath));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(CubePath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(LevelPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(CubePath));
	Durin::ShutdownRenderingThread();
}
