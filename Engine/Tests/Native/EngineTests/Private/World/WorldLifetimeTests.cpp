#include "WorldTestSupport.h"
#include "Preview/PreviewScene.h"
#include "IRendererModule.h"
#include "Rendering/LightSceneProxy.h"
#include "Rendering/SplineMeshSceneProxy.h"
#include "Rendering/SkyBoxSceneProxy.h"
#include "Rendering/VolumetricCloudSceneProxy.h"

namespace
{
	class FWorldSceneLifecycleTestScene final : public Durin::FSceneInterface
	{
	public:
		auto Release() -> void override {}
		auto AddPrimitive(Durin::DPrimitiveComponent*) -> void override {}
		auto RemovePrimitive(Durin::DPrimitiveComponent*) -> void override
		{
			++RemovePrimitiveCount;
		}

		auto AddLight(Durin::DLightComponent* Light) -> void override
		{
			if (const Durin::AActor* Owner = Light->GetOwner();
				Owner && Owner->IsHidden()) return;
			auto TokenStorage = std::make_unique<uint8>(0);
			LastAddedLight = reinterpret_cast<Durin::FLightSceneProxy*>(
				TokenStorage.get()
			);
			++AddLightCount;
			Lights.emplace(Light, std::move(TokenStorage));
		}

		auto RemoveLight(Durin::DLightComponent* Light) -> void override
		{
			const auto Found = Lights.find(Light);
			if (Found == Lights.end()) return;
			LastRemovedLight = reinterpret_cast<Durin::FLightSceneProxy*>(
				Found->second.get()
			);
			++RemoveLightCount;
			RetiredLights.push_back(std::move(Found->second));
			Lights.erase(Found);
		}

		auto AddSkyBox(Durin::DSkyBoxComponent*) -> void override {}
		auto RemoveSkyBox(Durin::DSkyBoxComponent*) -> void override {}
		auto AddVolumetricCloud(
			Durin::DVolumetricCloudComponent*
		) -> void override {}
		auto RemoveVolumetricCloud(
			Durin::DVolumetricCloudComponent*
		) -> void override {}

		auto UpdatePrimitiveTransform(Durin::FPrimitiveSceneId, const Durin::FMatrix&) -> void override
		{
		}

		auto UpdatePrimitiveVisibility(Durin::FPrimitiveSceneId, bool) -> void override
		{
		}

		auto UpdatePrimitiveMaterialBinding(
			Durin::FPrimitiveSceneId,
			const Durin::FMaterialRenderProxyBindingUpdate&
		) -> void override
		{
		}

		auto UpdateSplineMeshDynamicData(
			Durin::FPrimitiveSceneId,
			Durin::FSplineMeshRenderDynamicData
		) -> void override
		{
		}

		uint32 RemovePrimitiveCount = 0;
		uint32 AddLightCount = 0;
		uint32 RemoveLightCount = 0;
		Durin::FLightSceneProxy* LastAddedLight = nullptr;
		Durin::FLightSceneProxy* LastRemovedLight = nullptr;
		std::unordered_map<Durin::DLightComponent*, std::unique_ptr<uint8>> Lights;
		std::vector<std::unique_ptr<uint8>> RetiredLights;
	};
} // namespace

TEST(FWorldTests, DestroyAllActorsInvalidatesObjectPointers)
{
	Durin::DWorld* World = CreateWorld();
	Durin::TObjectPtr<Durin::AActor> Camera = World->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::TObjectPtr<Durin::AActor> Mesh = World->SpawnActor<Durin::AStaticMeshActor>("Mesh");
	Durin::TObjectPtr<Durin::DActorComponent> CameraComponent = Camera->FindComponentByClass<Durin::DCameraComponent>();
	Durin::TObjectPtr<Durin::DActorComponent> MeshComponent = Mesh->FindComponentByClass<Durin::DStaticMeshComponent>();

	World->DestroyAllActors();

	EXPECT_TRUE(World->GetActors().empty());
	EXPECT_FALSE(Camera.IsValid());
	EXPECT_FALSE(Mesh.IsValid());
	EXPECT_FALSE(CameraComponent.IsValid());
	EXPECT_FALSE(MeshComponent.IsValid());
	EXPECT_NE(Camera.Get(), nullptr);
	EXPECT_NE(CameraComponent.Get(), nullptr);

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
	EXPECT_EQ(Camera.Get(), nullptr);
	EXPECT_EQ(Mesh.Get(), nullptr);
	EXPECT_EQ(CameraComponent.Get(), nullptr);
	EXPECT_EQ(MeshComponent.Get(), nullptr);
}

TEST(FWorldTests, DestroyingWorldCascadesToActorsAndComponents)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ACameraActor* Camera = World->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::TObjectPtr<Durin::DWorld> WorldPtr = World;
	Durin::TObjectPtr<Durin::AActor> ActorPtr = Camera;
	Durin::TObjectPtr<Durin::DActorComponent> ComponentPtr = Camera->GetCameraComponent();

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();

	EXPECT_EQ(WorldPtr.Get(), nullptr);
	EXPECT_EQ(ActorPtr.Get(), nullptr);
	EXPECT_EQ(ComponentPtr.Get(), nullptr);
}

TEST(FWorldTests, DetachingRenderSceneUnregistersPendingKillComponents)
{
	Durin::DWorld* World = CreateWorld();
	Durin::AStaticMeshActor* Mesh = World->SpawnActor<Durin::AStaticMeshActor>("Mesh");
	ASSERT_NE(Mesh, nullptr);
	Durin::DStaticMeshComponent* Component = Mesh->GetStaticMeshComponent();
	ASSERT_NE(Component, nullptr);

	FWorldSceneLifecycleTestScene Scene;
	World->SetRenderScene(&Scene);
	ASSERT_TRUE(Component->IsRegistered());
	ASSERT_EQ(World->GetRenderScene(), &Scene);
	Scene.RemovePrimitiveCount = 0;

	Durin::MarkObjectHierarchyAsGarbage(World);
	World->SetRenderScene(nullptr);

	EXPECT_FALSE(Component->IsRegistered());
	EXPECT_EQ(World->GetRenderScene(), nullptr);
	EXPECT_EQ(Scene.RemovePrimitiveCount, 1u);
	Durin::CollectGarbage();
}

TEST(FWorldTests, DetachingPendingKillLevelUnregistersComponents)
{
	Durin::DWorld* World = CreateWorld();
	Durin::AStaticMeshActor* Mesh = World->SpawnActor<Durin::AStaticMeshActor>("Mesh");
	ASSERT_NE(Mesh, nullptr);
	Durin::DStaticMeshComponent* Component = Mesh->GetStaticMeshComponent();
	ASSERT_NE(Component, nullptr);

	FWorldSceneLifecycleTestScene Scene;
	World->SetRenderScene(&Scene);
	Scene.RemovePrimitiveCount = 0;

	Durin::MarkObjectHierarchyAsGarbage(World->GetCurrentLevel());
	ASSERT_TRUE(World->SetCurrentLevel(nullptr, false));

	EXPECT_FALSE(Component->IsRegistered());
	EXPECT_EQ(Scene.RemovePrimitiveCount, 1u);
	World->SetRenderScene(nullptr);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldTests, LightComponentRebuildsAndRetiresExactProxyTokens)
{
	Durin::DWorld* World = CreateWorld();
	FWorldSceneLifecycleTestScene Scene;
	World->SetRenderScene(&Scene);
	auto* Light = World->SpawnActor<Durin::ADirectionalLightActor>("LifecycleLight");
	ASSERT_NE(Light, nullptr);
	auto* Component = Light->GetLightComponent();
	ASSERT_NE(Component, nullptr);
	ASSERT_EQ(Scene.AddLightCount, 1u);
	Durin::FLightSceneProxy* FirstToken = Scene.LastAddedLight;

	Component->SetIntensity(2.0f);
	EXPECT_EQ(Scene.RemoveLightCount, 1u);
	EXPECT_EQ(Scene.LastRemovedLight, FirstToken);
	EXPECT_EQ(Scene.AddLightCount, 2u);
	Durin::FLightSceneProxy* RebuiltToken = Scene.LastAddedLight;
	EXPECT_NE(RebuiltToken, FirstToken);

	Light->SetHidden(true);
	EXPECT_EQ(Scene.RemoveLightCount, 2u);
	EXPECT_EQ(Scene.LastRemovedLight, RebuiltToken);
	EXPECT_TRUE(Scene.Lights.empty());

	Light->SetHidden(false);
	EXPECT_EQ(Scene.AddLightCount, 3u);
	Durin::FLightSceneProxy* VisibleToken = Scene.LastAddedLight;
	Component->UnregisterComponent();
	EXPECT_EQ(Scene.RemoveLightCount, 3u);
	EXPECT_EQ(Scene.LastRemovedLight, VisibleToken);
	EXPECT_TRUE(Scene.Lights.empty());

	World->SetRenderScene(nullptr);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FEngineObjectTests, EngineAndWorldHaveReflectedOwnership)
{
	InitializeDObjectSystem();
	Durin::DEngine* Engine = Durin::NewObject<Durin::DEngine>(nullptr, "TestEngine");
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(Engine, "MainWorld");
	EXPECT_TRUE(World->InitializeSubsystems());
	Durin::TObjectPtr<Durin::DEngine> EnginePtr = Engine;
	Durin::TObjectPtr<Durin::DWorld> WorldPtr = World;

	EXPECT_EQ(Engine->GetClass(), Durin::DEngine::StaticClass());
	EXPECT_EQ(World->GetClass(), Durin::DWorld::StaticClass());
	EXPECT_EQ(World->GetOuter(), Engine);

	Durin::MarkObjectHierarchyAsGarbage(Engine);
	EXPECT_FALSE(EnginePtr.IsValid());
	EXPECT_FALSE(WorldPtr.IsValid());
	Durin::CollectGarbage();
	EXPECT_EQ(EnginePtr.Get(), nullptr);
	EXPECT_EQ(WorldPtr.Get(), nullptr);
}

TEST(FEngineObjectTests, RootedEngineSurvivesGarbageCollection)
{
	InitializeDObjectSystem();
	Durin::DEngine* Engine = Durin::NewObject<Durin::DEngine>(nullptr, "RootedEngine");
	Durin::TObjectPtr<Durin::DEngine> EnginePtr = Engine;

	Durin::AddToRoot(Engine);
	Durin::CollectGarbage();
	EXPECT_EQ(EnginePtr.Get(), Engine);

	Durin::RemoveFromRoot(Engine);
	Durin::CollectGarbage();
	EXPECT_EQ(EnginePtr.Get(), nullptr);
}

TEST(FEngineObjectTests, GameEngineHasConcreteRuntimeClass)
{
	InitializeDObjectSystem();
	Durin::DGameEngine* Engine = Durin::NewObject<Durin::DGameEngine>(nullptr, "GameEngine");

	EXPECT_EQ(Engine->GetClass(), Durin::DGameEngine::StaticClass());
	EXPECT_TRUE(Engine->IsA<Durin::DEngine>());

	Durin::MarkObjectHierarchyAsGarbage(Engine);
	Durin::CollectGarbage();
}

namespace
{
	// Supplies CPU-only scene ownership so preview retirement can be tested without a GPU host.
	class FPreviewLifecycleRenderer : public Durin::IRendererModule
	{
	public:
		auto CreateScene() -> Durin::FScenePtr override
		{
			return {new FWorldSceneLifecycleTestScene(), Durin::FSceneDeleter([](Durin::FSceneInterface* Scene) { delete Scene; })};
		}
		auto CreateViewState() -> Durin::FSceneViewStateOwner override { return {}; }
		auto InvalidateViewState(Durin::FSceneViewStateId) -> void override {}
		auto InvalidateAllViewStates() -> void override {}
		auto RenderView(Durin::FRHICommandListImmediate&, Durin::FSceneInterface*, const Durin::FSceneView&,
			Durin::FRHITexture*, bool, const Durin::FSceneViewRenderOptions&, Durin::FSceneViewStatistics*, Durin::FRDGCapture*) -> Durin::ERenderViewResult override
		{
			return Durin::ERenderViewResult::Success;
		}
	};
	class FPreviewLifecycleEngine : public Durin::DEngine
	{
	public:
		FPreviewLifecycleEngine(Durin::IRendererModule& Renderer) : DEngine(Durin::FObjectInitializer::Get()) { RendererModule = &Renderer; }
	};
}

TEST(FWorldTests, PreviewHostInitializesAndRetiresServicesWithoutAPlayLifetime)
{
	InitializeDObjectSystem();
	FPreviewLifecycleRenderer Renderer;
	FPreviewLifecycleEngine Engine(Renderer);
	auto* PreviousEngine = std::exchange(Durin::GEngine, &Engine);
	std::shared_ptr<const Durin::FWorldSubsystemWorkGate> Gate;
	{
		Durin::Editor::FPreviewScene Preview("SubsystemPreview");
		EXPECT_TRUE(Preview.IsAvailable()) << Preview.GetDiagnostic();
		if (Preview.IsAvailable())
		{
			EXPECT_EQ(Preview.GetWorld()->GetWorldType(), Durin::EWorldType::Preview);
			EXPECT_EQ(Preview.GetWorld()->GetSubsystemState(), Durin::EWorldSubsystemState::Ready);
			EXPECT_FALSE(Preview.GetWorld()->HasBegunPlay());
			auto* Debug = Preview.GetWorld()->GetSubsystem<Durin::DCollisionDebugSubsystem>();
			EXPECT_NE(Debug, nullptr);
			if (Debug) Gate = Debug->GetWorkGate();
		}
	}
	EXPECT_TRUE(Gate && !Gate->IsOpen());
	Durin::CollectGarbage();
	Durin::GEngine = PreviousEngine;
}

TEST(FWorldTests, PreviewHostReportsInitializationFailureAndClosesItsWorld)
{
	InitializeDObjectSystem();
	FPreviewLifecycleRenderer Renderer;
	FPreviewLifecycleEngine Engine(Renderer);
	auto* PreviousEngine = std::exchange(Durin::GEngine, &Engine);
	{
		Durin::FWorldSubsystemRegistration Invalid({.Type = Durin::AActor::StaticClass(), .WorldTypes = {Durin::EWorldType::Preview}});
		Durin::Editor::FPreviewScene Preview("FailedSubsystemPreview");
		EXPECT_FALSE(Preview.IsAvailable());
		EXPECT_FALSE(Preview.GetDiagnostic().empty());
		EXPECT_EQ(Preview.GetWorld()->GetSubsystemState(), Durin::EWorldSubsystemState::Shutdown);
		EXPECT_EQ(Preview.GetWorld()->GetCurrentLevel(), nullptr);
		EXPECT_EQ(Preview.GetWorld()->GetRenderScene(), nullptr);
	}
	Durin::CollectGarbage();
	Durin::GEngine = PreviousEngine;
}
