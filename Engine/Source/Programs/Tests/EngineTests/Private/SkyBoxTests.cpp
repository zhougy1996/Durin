#include "Actors/SkyBoxActor.h"
#include "AssetSystem.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkyBoxComponent.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DynamicRHI.h"
#include "EngineTestSupport.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Misc/Paths.h"
#include "RendererModule.h"
#include "RHIGlobals.h"
#include "RHICommandList.h"
#include "Scene.h"
#include "RenderingThread.h"
#include "SkyBoxDetails.h"
#include "SkyBoxRendering.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureCubeRenderResource.h"

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

	auto MakePrincipalAxisView(
		const Durin::FVector3& Direction,
		const Durin::FVector3& Location,
		Durin::uint32 Width,
		Durin::uint32 Height
	) -> Durin::FSceneView
	{
		const Durin::FVector3 Forward = glm::normalize(Direction);
		const Durin::FVector3 UpHint = std::abs(glm::dot(Forward, Durin::FVectorConstants::Up)) > 0.99
			? Durin::FVectorConstants::Right : Durin::FVectorConstants::Up;
		const Durin::FVector3 Right = glm::normalize(glm::cross(UpHint, Forward));
		const Durin::FVector3 Up = glm::cross(Forward, Right);
		Durin::FMatrix ClipToWorld(1.0);
		ClipToWorld[0] = Durin::FVector4(Right, 0.0);
		ClipToWorld[1] = Durin::FVector4(Up, 0.0);
		ClipToWorld[2] = Durin::FVector4(Forward, 0.0);
		ClipToWorld[3] = Durin::FVector4(Location, 1.0);
		Durin::FSceneView View;
		View.ViewLocation = Location;
		View.ViewProjectionMatrix = glm::inverse(ClipToWorld);
		View.ViewportWidth = Width;
		View.ViewportHeight = Height;
		return View;
	}

	auto GetSourceColor(
		const Durin::DTextureCube& Cube,
		Durin::ETextureCubeFace Face,
		Durin::uint32 X,
		Durin::uint32 Y
	) -> std::array<Durin::uint8, 4>
	{
		const Durin::FTextureSourceData& Source = Cube.GetSourceData()->Faces[static_cast<size_t>(Face)];
		const size_t PixelOffset = (static_cast<size_t>(Y) * Source.Width + X) * 4;
		return {
			Source.Pixels[PixelOffset],
			Source.Pixels[PixelOffset + 1],
			Source.Pixels[PixelOffset + 2],
			Source.Pixels[PixelOffset + 3]
		};
	}

	auto ExpectRgbNear(
		const std::vector<Durin::uint8>& Pixels,
		Durin::uint32 Width,
		Durin::uint32 X,
		Durin::uint32 Y,
		const std::array<Durin::uint8, 4>& Expected,
		int Tolerance = 20
	) -> void
	{
		const size_t Offset = (static_cast<size_t>(Y) * Width + X) * 4;
		ASSERT_LE(Offset + 4, Pixels.size());
		for (size_t Channel = 0; Channel < 3; ++Channel)
		{
			EXPECT_NEAR(static_cast<int>(Pixels[Offset + Channel]), static_cast<int>(Expected[Channel]), Tolerance);
		}
	}

	auto ExpectRgbMatch(
		const std::vector<Durin::uint8>& Actual,
		const std::vector<Durin::uint8>& Expected,
		Durin::uint32 Width,
		Durin::uint32 X,
		Durin::uint32 Y,
		int Tolerance = 2
	) -> void
	{
		const size_t Offset = (static_cast<size_t>(Y) * Width + X) * 4;
		ASSERT_LE(Offset + 4, Actual.size());
		ASSERT_LE(Offset + 4, Expected.size());
		for (size_t Channel = 0; Channel < 3; ++Channel)
		{
			EXPECT_NEAR(static_cast<int>(Actual[Offset + Channel]), static_cast<int>(Expected[Offset + Channel]), Tolerance);
		}
	}

	auto FindClosestCenterRgb(
		const std::vector<Durin::uint8>& Actual,
		const std::array<std::vector<Durin::uint8>, Durin::TextureCubeFaceCount>& Candidates,
		Durin::uint32 Width
	) -> size_t
	{
		const size_t Offset = (static_cast<size_t>(Width / 2) * Width + Width / 2) * 4;
		size_t ClosestIndex = 0;
		Durin::uint32 ClosestDistance = std::numeric_limits<Durin::uint32>::max();
		for (size_t CandidateIndex = 0; CandidateIndex < Candidates.size(); ++CandidateIndex)
		{
			Durin::uint32 Distance = 0;
			for (size_t Channel = 0; Channel < 3; ++Channel)
			{
				const int Difference = static_cast<int>(Actual[Offset + Channel])
					- static_cast<int>(Candidates[CandidateIndex][Offset + Channel]);
				Distance += static_cast<Durin::uint32>(Difference * Difference);
			}
			if (Distance < ClosestDistance)
			{
				ClosestDistance = Distance;
				ClosestIndex = CandidateIndex;
			}
		}
		return ClosestIndex;
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

TEST(FSkyBoxVulkanTests, SamplesEveryFaceAndMipWithoutParallaxAndPreservesOcclusion)
{
	InitializeSkyBoxAssetMount();
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit();
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();

	Durin::FRendererModule Renderer;
	Durin::FScene Scene;
	struct FBeginSkyBoxValidationFrame
	{
		static constexpr auto GetName() -> const char* { return "BeginSkyBoxValidationFrame"; }
	};
	Durin::EnqueueRenderCommand<FBeginSkyBoxValidationFrame>([](Durin::FRHICommandListImmediate& CommandList) {
		CommandList.SwitchPipeline(Durin::ERHIPipeline::Graphics);
		Durin::GDynamicRHI->RHIBeginFrame();
	});
	Renderer.StartupModule();
	Renderer.SetRenderMode(Durin::ERenderMode::Unlit);

	Durin::FTextureCubeImportResult CubeResult = Durin::DTextureCube::ImportAsset(
		GetSkyBoxConventionFaces(), "/SkyBoxAssetTests/VulkanCube");
	ASSERT_TRUE(CubeResult) << CubeResult.Message;
	auto CubeResource = CubeResult.Asset->GetRenderResource();
	ASSERT_NE(CubeResource, nullptr);
	auto PlatformData = std::make_shared<Durin::FTextureCubePlatformData>(*CubeResult.Asset->GetPlatformData());
	auto* OcclusionMesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* OcclusionMaterial = Durin::NewObject<Durin::DMaterial>(nullptr, "SkyBoxOcclusionMaterial");
	OcclusionMaterial->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), {1.0, 0.0, 0.0});
	auto* OcclusionComponent = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "SkyBoxOcclusionMesh");
	OcclusionComponent->SetStaticMesh(OcclusionMesh);
	OcclusionComponent->SetMaterial(OcclusionMaterial);
	auto OcclusionProxy = std::make_shared<std::unique_ptr<Durin::PrimitiveSceneProxy>>(
		OcclusionComponent->CreateSceneProxy());
	ASSERT_NE(*OcclusionProxy, nullptr);

	Durin::FSkyBoxSceneData SkyBox;
	SkyBox.SceneId = Durin::FGuid(1, 0, 0, 0);
	SkyBox.SelectionKey = "VulkanSky";
	SkyBox.InstanceId = 1;
	SkyBox.TextureResource = CubeResource;
	SkyBox.Revision = 1;
	Scene.AddOrReplaceSkyBox(SkyBox);

	struct FEndSkyBoxValidationSetupFrame
	{
		static constexpr auto GetName() -> const char* { return "EndSkyBoxValidationSetupFrame"; }
	};
	Durin::EnqueueRenderCommand<FEndSkyBoxValidationSetupFrame>([](Durin::FRHICommandListImmediate& CommandList) {
		Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
	});
	Durin::FlushRenderingCommands();

	struct FValidationResult
	{
		bool bSucceeded = true;
		std::string Error;
		std::array<std::vector<Durin::uint8>, Durin::TextureCubeFaceCount> PrincipalAxes;
		std::vector<Durin::uint8> Translated;
		std::vector<Durin::uint8> ComponentRotated;
		std::vector<Durin::uint8> Letterboxed;
		std::vector<Durin::uint8> Occluded;
	};
	auto Result = std::make_shared<FValidationResult>();

	struct FRenderSkyBoxValidationFrame
	{
		static constexpr auto GetName() -> const char* { return "RenderSkyBoxValidationFrame"; }
	};
	Durin::EnqueueRenderCommand<FRenderSkyBoxValidationFrame>(
		[&Renderer, &Scene, CubeResource, PlatformData, Result, OcclusionProxy]
		(Durin::FRHICommandListImmediate& CommandList) {
			Durin::GRenderFrameCounterRenderThread++;
			Durin::GDynamicRHI->RHIBeginFrame();
			struct FEndFrameGuard
			{
				Durin::FRHICommandListImmediate& CommandList;
				~FEndFrameGuard() { Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList); }
			} EndFrameGuard{CommandList};

			Durin::FRHITexture* CubeTexture = CubeResource->GetTextureRHI_RenderThread();
			if (CubeTexture == nullptr)
			{
				Result->bSucceeded = false;
				Result->Error = "Cube render resource was not ready.";
				return;
			}
			for (Durin::uint32 FaceIndex = 0; FaceIndex < Durin::TextureCubeFaceCount; ++FaceIndex)
			{
				for (Durin::uint32 MipIndex = 0; MipIndex < PlatformData->Faces[FaceIndex].Mips.size(); ++MipIndex)
				{
					std::vector<Durin::uint8> MipPixels;
					if (!Durin::GDynamicRHI->RHIReadTexture2D(
						CommandList, CubeTexture, MipIndex, FaceIndex, MipPixels)
						|| MipPixels != PlatformData->Faces[FaceIndex].Mips[MipIndex].Pixels)
					{
						Result->bSucceeded = false;
						Result->Error = std::format(
							"Cube readback mismatch for face {} mip {}.", FaceIndex, MipIndex);
						return;
					}
				}
			}

			Durin::FRHITextureCreateDesc ColorDesc = Durin::FRHITextureCreateDesc::Create2D(
				"SkyBoxValidationColor", 17, 17, Durin::EPixelFormat::SRGBA8_UNORM)
				.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
					| Durin::ETextureCreateFlags::ShaderResource
					| Durin::ETextureCreateFlags::CPUReadback);
			Durin::FTextureRHIRef Color = Durin::GDynamicRHI->RHICreateTexture(CommandList, ColorDesc);
			if (Color == nullptr)
			{
				Result->bSucceeded = false;
				Result->Error = "Failed to create the validation output target.";
				return;
			}

			auto Render = [&](const Durin::FSceneView& View, std::vector<Durin::uint8>& OutPixels) {
				Renderer.RenderView(CommandList, &Scene, View, Color, false);
				if (!Durin::GDynamicRHI->RHIReadTexture2D(CommandList, Color, 0, 0, OutPixels))
				{
					Result->bSucceeded = false;
					Result->Error = "Failed to read the validation render target.";
					return false;
				}
				return true;
			};

			constexpr std::array<Durin::FVector3, Durin::TextureCubeFaceCount> Directions = {
				Durin::FVector3(1.0, 0.0, 0.0),
				Durin::FVector3(-1.0, 0.0, 0.0),
				Durin::FVector3(0.0, 1.0, 0.0),
				Durin::FVector3(0.0, -1.0, 0.0),
				Durin::FVector3(0.0, 0.0, 1.0),
				Durin::FVector3(0.0, 0.0, -1.0)
			};
			for (size_t FaceIndex = 0; FaceIndex < Directions.size(); ++FaceIndex)
			{
				if (!Render(MakePrincipalAxisView(Directions[FaceIndex], {}, 17, 17), Result->PrincipalAxes[FaceIndex])) return;
			}
			if (!Render(MakePrincipalAxisView(Directions[0], {19.0, -7.0, 4.0}, 17, 17), Result->Translated)) return;

			Durin::FSkyBoxSceneData RotatedSky;
			RotatedSky.SceneId = Durin::FGuid(1, 0, 0, 0);
			RotatedSky.SelectionKey = "VulkanSky";
			RotatedSky.InstanceId = 1;
			RotatedSky.TextureResource = CubeResource;
			RotatedSky.Rotation = glm::angleAxis(glm::half_pi<double>(), Durin::FVectorConstants::Up);
			RotatedSky.Revision = 2;
			Scene.AddOrReplaceSkyBox(RotatedSky);
			if (!Render(MakePrincipalAxisView(Directions[0], {}, 17, 17), Result->ComponentRotated)) return;

			Durin::FSceneView LetterboxView = MakePrincipalAxisView(Directions[0], {}, 17, 17);
			LetterboxView.AspectRatioConstraint = 0.5f;
			if (!Render(LetterboxView, Result->Letterboxed)) return;

			RotatedSky.Rotation = glm::identity<Durin::FQuat>();
			RotatedSky.Revision = 3;
			Scene.AddOrReplaceSkyBox(RotatedSky);
			Durin::FMatrix OccluderTransform = glm::translate(
				Durin::FMatrix(1.0), Durin::FVector3(0.0, 0.0, 0.5));
			OccluderTransform = glm::rotate(
				OccluderTransform, glm::pi<double>(), Durin::FVectorConstants::Right);
			Scene.AddOrReplacePrimitive(1, std::move(*OcclusionProxy), OccluderTransform);
			Renderer.PrepareSceneResources(CommandList, &Scene);
			Render(MakePrincipalAxisView(Directions[4], {}, 17, 17), Result->Occluded);
		});
	Durin::FlushRenderingCommands();

	EXPECT_TRUE(Result->bSucceeded) << Result->Error;
	if (Result->bSucceeded)
	{
		for (size_t FaceIndex = 0; FaceIndex < Durin::TextureCubeFaceCount; ++FaceIndex)
		{
			SCOPED_TRACE(std::format("principal face {}", FaceIndex));
			ExpectRgbNear(
				Result->PrincipalAxes[FaceIndex],
				17,
				4,
				4,
				GetSourceColor(*CubeResult.Asset, static_cast<Durin::ETextureCubeFace>(FaceIndex), 32, 32),
				8);
		}
		ExpectRgbMatch(Result->Translated, Result->PrincipalAxes[0], 17, 8, 8);
		EXPECT_EQ(FindClosestCenterRgb(Result->ComponentRotated, Result->PrincipalAxes, 17), 3u);
		ExpectRgbNear(Result->Letterboxed, 17, 1, 8, {0, 0, 0, 255}, 2);
		EXPECT_EQ(FindClosestCenterRgb(Result->Letterboxed, Result->PrincipalAxes, 17), 3u);
		ExpectRgbNear(Result->Occluded, 17, 8, 8, {255, 0, 0, 255}, 8);
	}

	Scene.Release();
	Durin::FAssetPath CubePath;
	if (Durin::FAssetPath::TryCreate("/SkyBoxAssetTests/VulkanCube", CubePath))
	{
		EXPECT_TRUE(Durin::Asset::DeleteAsset(CubePath));
	}
	else
	{
		ADD_FAILURE() << "Failed to create the Vulkan cube cleanup path.";
	}
	Durin::MarkAsGarbage(OcclusionComponent);
	Durin::MarkAsGarbage(OcclusionMesh);
	Durin::MarkAsGarbage(OcclusionMaterial);
	Durin::CollectGarbage();
	SkyBox.TextureResource.reset();
	Renderer.ReleaseResources();
	struct FRetireSkyBoxValidationResource
	{
		static constexpr auto GetName() -> const char* { return "RetireSkyBoxValidationResource"; }
	};
	Durin::EnqueueRenderCommand<FRetireSkyBoxValidationResource>(
		[Resource = std::move(CubeResource)](Durin::FRHICommandListImmediate&) {});
	Durin::FlushRenderingCommands();
	Renderer.SetRenderMode(Durin::ERenderMode::Lit);
	Renderer.ShutdownModule();
	Durin::ShutdownRenderingThread();
	Durin::RHIExit();
}
