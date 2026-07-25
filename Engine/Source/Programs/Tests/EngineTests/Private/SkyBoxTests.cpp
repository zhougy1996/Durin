#include "Actors/SkyBoxActor.h"
#include "AssetSystem.h"
#include "Components/SkyBoxComponent.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "EngineTestSupport.h"
#include "Engine/Engine.h"
#include "Misc/Paths.h"
#include "Scene.h"
#include "RenderingThread.h"
#include "SkyBoxRendering.h"
#include "Texture/TextureCube.h"

#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace
{
	struct FObserveSkyBoxCommand
	{
		static constexpr auto GetName() -> const char* { return "ObserveSkyBox"; }
	};

	struct FSkyBoxObservation
	{
		bool bHasActive = false;
		Durin::FSkyBoxSceneData Active;
		size_t Count = 0;
	};

	class FSkyBoxTestEngine final : public Durin::DEngine
	{
	public:
		FSkyBoxTestEngine()
			: DEngine(Durin::FObjectInitializer::Get())
		{
		}

		auto CreateTestScene() -> Durin::FScene*
		{
			auto Scene = std::make_unique<Durin::FScene>();
			Durin::FScene* Result = Scene.get();
			MainScene = std::move(Scene);
			return Result;
		}

		auto ResetTestScene() -> void { MainScene.reset(); }
	};

	auto ObserveSkyBoxes(const Durin::FScene& Scene) -> FSkyBoxObservation
	{
		auto Result = std::make_shared<FSkyBoxObservation>();
		Durin::EnqueueRenderCommand<FObserveSkyBoxCommand>([&Scene, Result](Durin::FRHICommandListImmediate&) {
			Result->bHasActive = Scene.GetActiveSkyBox_RenderThread(Result->Active);
			Result->Count = Scene.GetSkyBoxCount_RenderThread();
		});
		Durin::FlushRenderingCommands();
		return *Result;
	}

	auto GetSkyBoxConventionFaces() -> std::array<std::string, Durin::TextureCubeFaceCount>
	{
		constexpr std::array<std::string_view, Durin::TextureCubeFaceCount> FaceNames = {
			"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};
		std::array<std::string, Durin::TextureCubeFaceCount> Result;
		for (size_t FaceIndex = 0; FaceIndex < Result.size(); ++FaceIndex)
		{
			Result[FaceIndex] = (std::filesystem::path(DURIN_TEST_DATA_DIR) / "SkyBoxConvention" /
				std::format("{}.png", FaceNames[FaceIndex])).generic_string();
		}
		return Result;
	}

	auto InitializeSkyBoxAssetMount() -> std::filesystem::path
	{
		InitializeDObjectSystem();
		static const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "SkyBoxAssets";
		static const bool bInitialized = [] {
			std::filesystem::remove_all(Root);
			Durin::PathUtilities::RegisterMountPoint("/SkyBoxAssetTests/", Root.generic_string() + "/");
			return true;
		}();
		(void)bInitialized;
		return Root;
	}

	auto ReconstructSampleDirection(const Durin::SkyBoxRendering::FSkyBoxUniform& Uniform, const Durin::FVector2& ClipPosition)
		-> Durin::FVector3
	{
		const Durin::FMatrix ClipToWorld = glm::transpose(Durin::FMatrix(Uniform.ClipToWorld));
		const Durin::FMatrix WorldToSky = glm::transpose(Durin::FMatrix(Uniform.WorldToSky));
		const Durin::FVector4 WorldPositionH = ClipToWorld * Durin::FVector4(ClipPosition, 1.0, 1.0);
		const Durin::FVector3 WorldPosition = Durin::FVector3(WorldPositionH) / WorldPositionH.w;
		const Durin::FVector3 ViewPosition(Uniform.ViewPosition);
		return glm::normalize(Durin::FVector3(WorldToSky * Durin::FVector4(
			glm::normalize(WorldPosition - ViewPosition), 0.0)));
	}
}

TEST(FSkyBoxRenderingTests, ReconstructsTranslationInvariantDirectionAndInverseComponentRotation)
{
	Durin::FSkyBoxSceneData SkyBox;
	SkyBox.Rotation = glm::angleAxis(glm::half_pi<double>(), Durin::FVectorConstants::Up);
	SkyBox.Tint = {0.25f, 0.5f, 0.75f};
	SkyBox.Intensity = 2.0f;

	Durin::FSceneView OriginView;
	OriginView.ViewProjectionMatrix = Durin::FMatrix(1.0);
	Durin::SkyBoxRendering::FSkyBoxUniform OriginUniform;
	ASSERT_TRUE(Durin::SkyBoxRendering::BuildUniform(OriginView, SkyBox, OriginUniform));

	Durin::FSceneView TranslatedView = OriginView;
	TranslatedView.ViewLocation = {7.0, -3.0, 11.0};
	TranslatedView.ViewProjectionMatrix = glm::translate(Durin::FMatrix(1.0), -TranslatedView.ViewLocation);
	Durin::SkyBoxRendering::FSkyBoxUniform TranslatedUniform;
	ASSERT_TRUE(Durin::SkyBoxRendering::BuildUniform(TranslatedView, SkyBox, TranslatedUniform));

	const Durin::FVector3 OriginDirection = ReconstructSampleDirection(OriginUniform, {0.0, 0.0});
	const Durin::FVector3 TranslatedDirection = ReconstructSampleDirection(TranslatedUniform, {0.0, 0.0});
	for (Durin::uint32 Axis = 0; Axis < 3; ++Axis)
	{
		EXPECT_NEAR(OriginDirection[Axis], TranslatedDirection[Axis], 1.e-6);
	}
	const Durin::FVector3 ExpectedDirection = glm::inverse(glm::normalize(SkyBox.Rotation)) * Durin::FVectorConstants::Up;
	for (Durin::uint32 Axis = 0; Axis < 3; ++Axis)
	{
		EXPECT_NEAR(OriginDirection[Axis], ExpectedDirection[Axis], 1.e-6);
	}
	EXPECT_EQ(OriginUniform.TintIntensity, Durin::FVector4f(0.25f, 0.5f, 0.75f, 2.0f));
}

TEST(FSkyBoxRenderingTests, RejectsInvalidTransformsAndClampsIntensity)
{
	Durin::FSceneView View;
	Durin::FSkyBoxSceneData SkyBox;
	Durin::SkyBoxRendering::FSkyBoxUniform Uniform;

	View.ViewProjectionMatrix = Durin::FMatrix(0.0);
	EXPECT_FALSE(Durin::SkyBoxRendering::BuildUniform(View, SkyBox, Uniform));

	View.ViewProjectionMatrix = Durin::FMatrix(1.0);
	SkyBox.Rotation = Durin::FQuat(0.0, 0.0, 0.0, 0.0);
	EXPECT_FALSE(Durin::SkyBoxRendering::BuildUniform(View, SkyBox, Uniform));

	SkyBox.Rotation = glm::identity<Durin::FQuat>();
	SkyBox.Intensity = -3.0f;
	ASSERT_TRUE(Durin::SkyBoxRendering::BuildUniform(View, SkyBox, Uniform));
	EXPECT_FLOAT_EQ(Uniform.TintIntensity.w, 0.0f);
}

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
	auto* Actor = Durin::NewObject<Durin::ASkyBoxActor>(nullptr, "LiveSkyBoxActor");
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
	EXPECT_EQ(Observation.Active.TextureResource, Cube->GetRenderResource());
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

	Scene->Release();
	Durin::FlushRenderingCommands();
	Engine.ResetTestScene();
	Durin::GEngine = nullptr;
	Durin::MarkAsGarbage(Actor);
	Durin::MarkAsGarbage(Cube);
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
