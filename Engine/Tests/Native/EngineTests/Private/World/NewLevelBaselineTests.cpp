#include "WorldTestSupport.h"

#include "Actors/SkyBoxActor.h"
#include "Components/SkyBoxComponent.h"
#include "DObject/Package.h"
#include "Materials/MaterialInterface.h"

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
		ASSERT_EQ(Actor->GetOwnedComponents().size(), 1u);
		EXPECT_TRUE(Actor->GetInstanceComponents().empty());
		Durin::DActorComponent* Component = Actor->GetOwnedComponents().front().Get();
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
		EXPECT_EQ(Level->GetName(), "NewLevel");
		ASSERT_EQ(Level->GetActors().size(), 4u);
		EXPECT_EQ(Level->GetActors()[0]->GetName(), "DirectionalLightActor");
		EXPECT_EQ(Level->GetActors()[1]->GetName(), "CameraActor");
		EXPECT_EQ(Level->GetActors()[2]->GetName(), "SkyBoxActor");
		EXPECT_EQ(Level->GetActors()[3]->GetName(), "Box");

		auto* LightActor = Durin::Cast<Durin::ADirectionalLightActor>(
			Level->FindActorByName("DirectionalLightActor"));
		ExpectSingleRootComponent(
			LightActor, "Durin::DDirectionalLightComponent", "DirectionalLightComponent");
		ExpectTransformNear(
			LightActor->GetActorTransform(),
			Durin::FQuat(0.91030458659254632, 0.0, 0.0, -0.41393907719442657),
			{0.0, 2.1259323682006928, 0.86950246609238957},
			{1.0, 1.0, 1.0});
		const Durin::FDirectionalLightSceneData Light =
			LightActor->GetLightComponent()->GetSceneData();
		EXPECT_EQ(Light.Color, Durin::FVector3f(1.0f));
		EXPECT_FLOAT_EQ(Light.Intensity, 1.0f);
		EXPECT_FLOAT_EQ(Light.AmbientIntensity, 0.08f);

		auto* CameraActor = Durin::Cast<Durin::ACameraActor>(
			Level->FindActorByName("CameraActor"));
		ExpectSingleRootComponent(CameraActor, "Durin::DCameraComponent", "DCameraComponent");
		ExpectTransformNear(
			CameraActor->GetActorTransform(), Durin::FQuat(1.0, 0.0, 0.0, 0.0),
			{-1.6669377762075972, 0.0, 0.0}, {1.0, 1.0, 1.0});
		EXPECT_EQ(Level->GetPrimaryCameraActor(), CameraActor);
		const Durin::FCameraProjectionSettings& Projection =
			CameraActor->GetCameraComponent()->GetProjectionSettings();
		EXPECT_FLOAT_EQ(Projection.FieldOfViewDegrees, 60.0f);
		EXPECT_FLOAT_EQ(Projection.NearClip, 0.1f);
		EXPECT_FLOAT_EQ(Projection.FarClip, 1000.0f);
		EXPECT_EQ(Projection.AspectRatioMode, Durin::ECameraAspectRatioMode::Viewport);
		EXPECT_FLOAT_EQ(Projection.CustomAspectRatio, 16.0f / 9.0f);

		auto* SkyBoxActor = Durin::Cast<Durin::ASkyBoxActor>(
			Level->FindActorByName("SkyBoxActor"));
		ExpectSingleRootComponent(SkyBoxActor, "Durin::DSkyBoxComponent", "SkyBoxComponent");
		ExpectTransformNear(
			SkyBoxActor->GetActorTransform(), Durin::FQuat(1.0, 0.0, 0.0, 0.0),
			{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0});
		Durin::DSkyBoxComponent* SkyBox = SkyBoxActor->GetSkyBoxComponent();
		ASSERT_NE(SkyBox->GetTextureCube(), nullptr);
		EXPECT_EQ(SkyBox->GetTint(), Durin::FLinearColor(1.0f, 1.0f, 1.0f, 1.0f));
		EXPECT_FLOAT_EQ(SkyBox->GetIntensity(), 1.0f);
		EXPECT_EQ(SkyBox->GetSkyBoxSceneId().ToString(), "8dcc0ca9-e5c5-42c7-91e2-03716ab9ec56");

		auto* MeshActor = Durin::Cast<Durin::AStaticMeshActor>(
			Level->FindActorByName("Box"));
		ExpectSingleRootComponent(
			MeshActor, "Durin::DStaticMeshComponent", "DStaticMeshComponent");
		ExpectTransformNear(
			MeshActor->GetActorTransform(), Durin::FQuat(1.0, 0.0, 0.0, 0.0),
			{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0});
		EXPECT_EQ(PackagePath(MeshActor->GetStaticMeshComponent()->GetStaticMesh()), "/Engine/Models/Box");
		// Component material state is intentionally outside the reconstruction manifest.
	}
}

TEST(FNewLevelBaselineTests, RecreatedLevelMatchesCapturedLogicalManifest)
{
	InitializeDObjectSystem();
	Durin::PathUtilities::FScopedMountRegistryFixture SavedMountRegistry;
	ASSERT_TRUE(Durin::PathUtilities::InitDefaultMountPoints());
	std::vector<Durin::PathUtilities::FMountPoint> MountDefinitions(
		Durin::PathUtilities::GetRegisteredMountPoints().begin(),
		Durin::PathUtilities::GetRegisteredMountPoints().end());
	MountDefinitions.push_back({
		.VirtualRoot = "/Game/",
		.Owner = Durin::PathUtilities::EMountOwner::Test,
		.Root = DURIN_STATIC_MESH_BASELINE_GAME_CONTENT_DIR,
		.bAutoScan = true,
		.bAuthoringWritable = true,
		.Dependencies = {"/Engine/"}});
	Durin::PathUtilities::FScopedMountRegistryFixture MountRegistry(MountDefinitions);
	ASSERT_TRUE(MountRegistry.IsValid()) << MountRegistry.GetError();

	Durin::FAssetPath LevelPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Game/Levels/NewLevel", LevelPath));
	Durin::DLevel* Level = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(LevelPath, Level));
	ExpectReconstructionManifest(Level);

	auto* MeshActor = Durin::Cast<Durin::AStaticMeshActor>(Level->FindActorByName("Box"));
	ASSERT_NE(MeshActor, nullptr);
	EXPECT_TRUE(MeshActor->GetStaticMeshComponent()->GetOverrideMaterials().empty());

	Durin::Asset::FAssetPackageInspection Inspection;
	const std::string LevelPhysicalPath =
		(std::filesystem::path(DURIN_STATIC_MESH_BASELINE_GAME_CONTENT_DIR)
			/ "Levels/NewLevel.dasset").generic_string();
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(
		LevelPhysicalPath, Inspection));
	std::vector<std::string> DependencyPaths;
	for (const Durin::FAssetPath& Dependency : Inspection.Header.Dependencies)
	{
		DependencyPaths.push_back(Dependency.ToString());
	}
	EXPECT_EQ(
		DependencyPaths,
		(std::vector<std::string>{
			"/Engine/Models/Box",
			"/Game/Textures/TEXCUBE_PureSky_512x512"}));

	EXPECT_TRUE(Durin::Asset::UnloadPackage(LevelPath));
}
