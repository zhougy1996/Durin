#include "NativeAssetRuntimeTestSupport.h"
#include "AssetRegistry/Catalog.h"
#include "AssetRegistry/Scan.h"
#include "Misc/UnifiedError.h"
#include "NativeAssetTestSupport.h"
#include "NativePackageUnloadTestSupport.h"
#include "SkyBoxTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "Texture/TextureCubeFactoryTestSupport.h"
#include "Math/Operations.h"
#include "Preview/PreviewMeshPaths.h"
#include "Editor/EditorActorSubsystemTestSupport.h"
#include "Texture/TextureCube.h"
#include "Actors/StaticMeshActor.h"
#include "Materials/MaterialInstance.h"
#include "Transactions/Transactor.h"

namespace
{
	auto LoadDefaultCubeSky() -> Durin::DMaterial*
	{
		Durin::FSoftObjectPath Path;
		if (!Durin::FSoftObjectPath::TryCreate("/Engine/Materials/DefaultCubeSky.DefaultCubeSky", Path)) return nullptr;
		return Durin::LoadObject<Durin::DMaterial>(Path);
	}
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
	Durin::FPackagePath MaterialPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkyBoxAssetTests/AuthoredSkyMaterial", MaterialPath));
	Durin::DMaterialInstance* Material = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(MaterialPath, Material));
	auto* Parent = LoadDefaultCubeSky();
	ASSERT_NE(Parent, nullptr);
	ASSERT_TRUE(Material->SetParent(Parent));
	ASSERT_TRUE(Material->SetTextureParameterValue("SkyTexture", CubeResult.Asset));
	ASSERT_TRUE(Material->SetVectorParameterValue("SkyTint", {0.25, 0.5, 0.75}));
	ASSERT_TRUE(Material->SetScalarParameterValue("SkyIntensity", 2.5f));
	ASSERT_TRUE(Durin::SavePackage(Material->GetPackage()));
	const auto MaterialEntry = Durin::FindPackageExact(MaterialPath);
	ASSERT_NE(MaterialEntry, nullptr);
	EXPECT_NE(std::ranges::find(MaterialEntry->Dependencies, CubePath), MaterialEntry->Dependencies.end());
	auto* Actor = Durin::Cast<Durin::AStaticMeshActor>(Editor.GetActorEditor()->SpawnActor(*Level,
		Durin::AStaticMeshActor::StaticClass(), "Sky", {}, {false}, [&](Durin::AActor& Created) {
			auto* Sky = static_cast<Durin::AStaticMeshActor&>(Created).GetStaticMeshComponent();
			Sky->SetStaticMesh(Sphere);
			EXPECT_TRUE(Sky->SetMaterial(Material));
		}));
	ASSERT_NE(Actor, nullptr);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Level->FindActorByName("Sky"), nullptr);
	auto* Conflict = Level->SpawnActor<Durin::AStaticMeshActor>("Sky");
	ASSERT_NE(Conflict, nullptr);
	EXPECT_NE(Conflict->GetFName(), Actor->GetFName());
	EXPECT_EQ(Conflict->GetActorLabel(), "Sky");
	const auto ReservedName = Conflict->GetFName();
	Conflict->Rename("Sky");
	EXPECT_EQ(Conflict->GetFName(), ReservedName);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Level->DestroyActor(Conflict));
	EXPECT_EQ(Level->FindActorByName("Sky"), Actor);
	Actor = Durin::Cast<Durin::AStaticMeshActor>(Level->FindActorByName("Sky"));
	ASSERT_NE(Actor, nullptr);
	EXPECT_EQ(Actor->GetStaticMeshComponent()->GetMaterial(), Material);
	Durin::FTransform Transform = Actor->GetActorTransform();
	Transform.Rotation = Durin::Math::MakeQuaternionFromAxisAngleDegrees(35.0, Durin::FVectorConstants::Up);
	ASSERT_TRUE(Actor->SetActorTransform(Transform));
	ASSERT_TRUE(Durin::SavePackage(Level->GetPackage()));
	Transactions->Reset();
	ASSERT_TRUE(Editor.GetWorld()->SetCurrentLevel(nullptr));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(LevelPath));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(MaterialPath));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(CubePath));

	Durin::DLevel* LoadedLevel = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedValueError;
		auto LoadedValue = Durin::LoadObject<Durin::DLevel>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(LevelPath), nullptr, &LoadedValueError);
		LoadedLevel = LoadedValue;
		ASSERT_TRUE(LoadedValue);
	}
	auto* LoadedActor = Durin::Cast<Durin::AStaticMeshActor>(LoadedLevel->FindActorByName("Sky"));
	ASSERT_NE(LoadedActor, nullptr);
	Durin::DStaticMeshComponent* LoadedComponent = LoadedActor->GetStaticMeshComponent();
	ASSERT_NE(LoadedComponent, nullptr);
	ASSERT_NE(LoadedComponent->GetStaticMesh(), nullptr);
	EXPECT_EQ(LoadedComponent->GetStaticMesh()->GetName(), "Sphere");
	auto* LoadedMaterial = Durin::Cast<Durin::DMaterialInstance>(LoadedComponent->GetMaterial());
	ASSERT_NE(LoadedMaterial, nullptr);
	ASSERT_NE(LoadedMaterial->GetParent(), nullptr);
	EXPECT_EQ(LoadedMaterial->GetParent(), LoadDefaultCubeSky());
	Durin::DTexture* LoadedTexture = nullptr;
	ASSERT_TRUE(LoadedMaterial->GetTextureParameterValue("SkyTexture", LoadedTexture));
	ASSERT_NE(Durin::Cast<Durin::DTextureCube>(LoadedTexture), nullptr);
	EXPECT_EQ(LoadedTexture->GetName(), "EditorWorkflowCube");
	Durin::FVector3 Tint;
	ASSERT_TRUE(LoadedMaterial->GetVectorParameterValue("SkyTint", Tint));
	EXPECT_EQ(Tint, (Durin::FVector3(0.25, 0.5, 0.75)));
	float Intensity = 0.0f;
	ASSERT_TRUE(LoadedMaterial->GetScalarParameterValue("SkyIntensity", Intensity));
	EXPECT_FLOAT_EQ(Intensity, 2.5f);
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
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(MaterialPath));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(CubePath));
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
	auto* Actor = Level->SpawnActor<Durin::AStaticMeshActor>("PanoramaSky");
	ASSERT_NE(Actor, nullptr);
	Durin::Tests::FActorEditorFixture Editor;
	ASSERT_TRUE(Editor.GetWorld()->SetCurrentLevel(Level));
	auto* Transactions = Editor.GetTransactions();
	Durin::FSoftObjectPath SpherePath;
	ASSERT_TRUE(Durin::FSoftObjectPath::TryCreate(Durin::PreviewMeshPaths::SphereAssetPath, SpherePath));
	auto* Sphere = Durin::LoadObject<Durin::DStaticMesh>(SpherePath);
	ASSERT_NE(Sphere, nullptr);
	Durin::FPackagePath MaterialPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkyBoxAssetTests/PanoramaSkyMaterial", MaterialPath));
	Durin::DMaterialInstance* Material = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(MaterialPath, Material));
	auto* Parent = LoadDefaultCubeSky();
	ASSERT_NE(Parent, nullptr);
	ASSERT_TRUE(Material->SetParent(Parent));
	auto* Component = Actor->GetStaticMeshComponent();
	Component->SetStaticMesh(Sphere);
	ASSERT_TRUE(Component->SetMaterial(Material));
	{
		Durin::FScopedTransaction Scope("test", "Assign cubemap", Material);
		Material->SetFlags(Durin::EObjectFlags::Transactional);
		ASSERT_TRUE(Material->Modify(false));
		ASSERT_TRUE(Material->SetTextureParameterValue("SkyTexture", CubeResult.Asset));
	}
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FALSE(Material->HasLocalParameterValueByInfo({.Name = "SkyTexture"}));
	ASSERT_TRUE(Transactions->Redo());
	Durin::DTexture* Texture = nullptr;
	ASSERT_TRUE(Material->GetTextureParameterValue("SkyTexture", Texture));
	EXPECT_EQ(Texture, CubeResult.Asset);
	ASSERT_TRUE(Durin::SavePackage(Material->GetPackage()));
	ASSERT_TRUE(Durin::SavePackage(Level->GetPackage()));
	Transactions->Reset();
	ASSERT_TRUE(Editor.GetWorld()->SetCurrentLevel(nullptr));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(LevelPath));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(MaterialPath));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(CubePath));

	Durin::DLevel* LoadedLevel = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedValueError;
		auto LoadedValue = Durin::LoadObject<Durin::DLevel>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(LevelPath), nullptr, &LoadedValueError);
		LoadedLevel = LoadedValue;
		ASSERT_TRUE(LoadedValue);
	}
	auto* LoadedActor = Durin::Cast<Durin::AStaticMeshActor>(
		LoadedLevel->FindActorByName("PanoramaSky"));
	ASSERT_NE(LoadedActor, nullptr);
	auto* LoadedMaterial = Durin::Cast<Durin::DMaterialInstance>(LoadedActor->GetStaticMeshComponent()->GetMaterial());
	ASSERT_NE(LoadedMaterial, nullptr);
	Durin::DTexture* LoadedTexture = nullptr;
	ASSERT_TRUE(LoadedMaterial->GetTextureParameterValue("SkyTexture", LoadedTexture));
	auto* LoadedCube = Durin::Cast<Durin::DTextureCube>(LoadedTexture);
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
	EXPECT_TRUE(LoadedActor->GetStaticMeshComponent()->IsRegistered());

	ASSERT_TRUE(World->SetCurrentLevel(nullptr));
	World->SetRenderScene(nullptr);
	Durin::FlushRenderingCommands();
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	Engine.ResetTestScene();
	Durin::GEngine = nullptr;
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(LevelPath));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(MaterialPath));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(CubePath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(LevelPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(MaterialPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(CubePath));
	Durin::ShutdownRenderingThread();
}
