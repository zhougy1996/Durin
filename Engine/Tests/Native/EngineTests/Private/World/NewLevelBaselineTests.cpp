#include "Asset/AssetCompilingManager.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "WorldTestSupport.h"

#include "Rendering/LightSceneProxy.h"

#include "DObject/Package.h"
#include "Modules/ModuleManager.h"

namespace
{
	auto ExpectTransformNear(
		const Durin::FTransform& Actual,
		const Durin::FQuat& ExpectedRotation,
		const Durin::FVector3& ExpectedTranslation,
		const Durin::FVector3& ExpectedScale) -> void
	{
		constexpr double Tolerance = 1.e-12;
		EXPECT_NEAR(Actual.Rotation.w, ExpectedRotation.w, Tolerance);
		EXPECT_NEAR(Actual.Rotation.x, ExpectedRotation.x, Tolerance);
		EXPECT_NEAR(Actual.Rotation.y, ExpectedRotation.y, Tolerance);
		EXPECT_NEAR(Actual.Rotation.z, ExpectedRotation.z, Tolerance);
		ExpectVectorNear(Actual.Translation, ExpectedTranslation, Tolerance);
		ExpectVectorNear(Actual.Scale3D, ExpectedScale, Tolerance);
	}

	auto ExpectSingleRootComponent(
		Durin::AActor* Actor,
		std::string_view ExpectedClass,
		std::string_view ExpectedName) -> void
	{
		ASSERT_NE(Actor, nullptr);
		ASSERT_EQ(Actor->GetComponents().size(), 1u);
		EXPECT_TRUE(Actor->GetInstanceComponents().empty());
		Durin::DActorComponent* Component = Actor->GetComponents().front().Get();
		ASSERT_NE(Component, nullptr);
		EXPECT_EQ(Component, Actor->GetRootComponent());
		EXPECT_EQ(Component->GetClass()->GetName(), ExpectedClass);
		EXPECT_EQ(Component->GetName(), ExpectedName);
		EXPECT_EQ(Actor->GetAttachParentActor(), nullptr);
		EXPECT_FALSE(Actor->IsHidden());
	}

	auto PackagePath(const Durin::DObject* Object) -> std::string
	{
		return Object && Object->GetPackage() ? Object->GetPackage()->GetPackagePath() : "None";
	}

	auto ExpectReconstructionManifest(Durin::DLevel* Level) -> void
	{
		ASSERT_NE(Level, nullptr);
		EXPECT_EQ(Level->GetName(), "Reconstruction");
		ASSERT_EQ(Level->GetActors().size(), 3u);
		EXPECT_EQ(Level->GetActors()[0]->GetName(), "DirectionalLight");
		EXPECT_EQ(Level->GetActors()[1]->GetName(), "Camera");
		EXPECT_EQ(Level->GetActors()[2]->GetName(), "StaticMesh");

		auto* LightActor = Durin::Cast<Durin::ADirectionalLightActor>(
			Level->FindActorByName("DirectionalLight"));
		ASSERT_NO_FATAL_FAILURE(ExpectSingleRootComponent(
			LightActor, "Durin::DDirectionalLightComponent", "DirectionalLightComponent"));
		ExpectTransformNear(
			LightActor->GetActorTransform(),
			Durin::FQuat(0.7071067811865476, 0.0, 0.0, -0.7071067811865475),
			{10.0, 20.0, 30.0}, {1.0, 1.0, 1.0});
		const Durin::FDirectionalLightSceneData Light =
			LightActor->GetLightComponent()->GetSceneData();
		EXPECT_EQ(Light.Color, Durin::FVector3f(1.0f));
		EXPECT_FLOAT_EQ(Light.Intensity, 2.0f);
		EXPECT_FLOAT_EQ(Light.AmbientIntensity, 0.25f);

		auto* CameraActor = Durin::Cast<Durin::ACameraActor>(
			Level->FindActorByName("Camera"));
		ASSERT_NO_FATAL_FAILURE(ExpectSingleRootComponent(
			CameraActor, "Durin::DCameraComponent", "DCameraComponent"));
		ExpectTransformNear(
			CameraActor->GetActorTransform(), Durin::FQuat(1.0, 0.0, 0.0, 0.0),
			{-5.0, 4.0, 3.0}, {1.0, 1.0, 1.0});
		EXPECT_EQ(Level->GetPrimaryCameraActor(), CameraActor);
		const Durin::FCameraProjectionSettings& Projection =
			CameraActor->GetCameraComponent()->GetProjectionSettings();
		EXPECT_FLOAT_EQ(Projection.FieldOfViewDegrees, 75.0f);
		EXPECT_FLOAT_EQ(Projection.NearClip, 0.25f);
		EXPECT_FLOAT_EQ(Projection.FarClip, 2500.0f);
		EXPECT_EQ(Projection.AspectRatioMode, Durin::ECameraAspectRatioMode::Custom);
		EXPECT_FLOAT_EQ(Projection.CustomAspectRatio, 2.39f);

		auto* MeshActor = Durin::Cast<Durin::AStaticMeshActor>(
			Level->FindActorByName("StaticMesh"));
		ASSERT_NO_FATAL_FAILURE(ExpectSingleRootComponent(
			MeshActor, "Durin::DStaticMeshComponent", "DStaticMeshComponent"));
		ExpectTransformNear(
			MeshActor->GetActorTransform(), Durin::FQuat(1.0, 0.0, 0.0, 0.0),
			{1.0, 2.0, 3.0}, {2.0, 2.0, 2.0});
		EXPECT_EQ(
			PackagePath(MeshActor->GetStaticMeshComponent()->GetStaticMesh()),
			"/Engine/Models/Box");
		EXPECT_TRUE(MeshActor->GetStaticMeshComponent()->GetOverrideMaterials().empty());
	}
}

TEST(FLevelAssetTests, ReconstructsIsolatedStaticMeshLevelAndDependencies)
{
	InitializeDObjectSystem();
	if (!Durin::FAssetCompilingManager::Get().IsAcceptingRequests())
		ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
	Durin::FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
	Durin::FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "LevelReconstruction";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	ASSERT_TRUE(std::filesystem::create_directories(Root));

	Durin::Testing::FScopedMountRegistryFixture SavedMountRegistry;
	ASSERT_TRUE(Durin::FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(Durin::RefreshAssetRegistry());
	std::vector<Durin::FMountPoint> MountDefinitions(
		Durin::FMountPaths::GetRegisteredMountPoints().begin(),
		Durin::FMountPaths::GetRegisteredMountPoints().end());
	MountDefinitions.push_back({
		.VirtualRoot = "/LevelReconstruction/",
		.Owner = Durin::EMountOwner::Test,
		.Root = Root.generic_string() + "/",
		.bAutoScan = true,
		.bContentWritable = true,
		.Dependencies = {"/Engine/"}});
	Durin::Testing::FScopedMountRegistryFixture MountRegistry(MountDefinitions);
	ASSERT_TRUE(MountRegistry.IsValid()) << MountRegistry.GetError();

	Durin::FPackagePath MeshPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Engine/Models/Box", MeshPath));
	Durin::DStaticMesh* Mesh = nullptr;
	const Durin::FAssetResult MeshLoad = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(MeshPath), Mesh);
	ASSERT_TRUE(MeshLoad) << MeshLoad.Message;
	Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Mesh);

	Durin::FPackagePath LevelPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/LevelReconstruction/Reconstruction", LevelPath));
	Durin::DLevel* Level = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(LevelPath, Level));

	Durin::ADirectionalLightActor* Light =
		Level->SpawnActor<Durin::ADirectionalLightActor>("DirectionalLight");
	Durin::ACameraActor* Camera = Level->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::AStaticMeshActor* MeshActor =
		Level->SpawnActor<Durin::AStaticMeshActor>("StaticMesh");
	ASSERT_NE(Light, nullptr);
	ASSERT_NE(Camera, nullptr);
	ASSERT_NE(MeshActor, nullptr);

	Durin::FTransform LightTransform;
	LightTransform.Rotation =
		Durin::FQuat(0.7071067811865476, 0.0, 0.0, -0.7071067811865475);
	LightTransform.Translation = {10.0, 20.0, 30.0};
	ASSERT_TRUE(Light->SetActorTransform(LightTransform));
	Light->GetLightComponent()->SetIntensity(2.0f);
	Light->GetLightComponent()->SetAmbientIntensity(0.25f);

	Durin::FTransform CameraTransform;
	CameraTransform.Translation = {-5.0, 4.0, 3.0};
	ASSERT_TRUE(Camera->SetActorTransform(CameraTransform));
	Camera->GetCameraComponent()->SetFieldOfViewDegrees(75.0f);
	Camera->GetCameraComponent()->SetNearClip(0.25f);
	Camera->GetCameraComponent()->SetFarClip(2500.0f);
	Camera->GetCameraComponent()->SetAspectRatio(
		Durin::ECameraAspectRatioMode::Custom, 2.39f);
	ASSERT_TRUE(Level->SetPrimaryCameraActor(Camera));

	Durin::FTransform MeshTransform;
	MeshTransform.Translation = {1.0, 2.0, 3.0};
	MeshTransform.Scale3D = {2.0, 2.0, 2.0};
	ASSERT_TRUE(MeshActor->SetActorTransform(MeshTransform));
	MeshActor->GetStaticMeshComponent()->SetStaticMesh(Mesh);

	ASSERT_TRUE(Durin::SavePackage(Level->GetPackage()));
	Durin::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::InspectAssetPackage(
		(Root / "Reconstruction.dasset").generic_string(), Inspection));
	ASSERT_EQ(Inspection.Header.Dependencies.size(), 1u);
	EXPECT_EQ(Inspection.Header.Dependencies.front(), MeshPath);

	ASSERT_TRUE(Durin::UnloadPackage(LevelPath));
	ASSERT_TRUE(Durin::UnloadPackage(MeshPath));
	EXPECT_EQ(Durin::FindResidentPackage(MeshPath), nullptr);

	Durin::DLevel* Loaded = nullptr;
	const Durin::FAssetResult LevelLoad = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(LevelPath), Loaded);
	ASSERT_TRUE(LevelLoad) << LevelLoad.Message;
	EXPECT_NE(Durin::FindResidentPackage(MeshPath), nullptr);
	ASSERT_NO_FATAL_FAILURE(ExpectReconstructionManifest(Loaded));

	EXPECT_TRUE(Durin::UnloadPackage(LevelPath));
	EXPECT_TRUE(Durin::UnloadPackage(MeshPath));
}
