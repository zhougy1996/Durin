#include "WorldTestSupport.h"
#include "Rendering/LightSceneProxy.h"
#include "Rendering/SplineMeshSceneProxy.h"
#include "Rendering/SkyBoxSceneProxy.h"
#include "Rendering/VolumetricCloudSceneProxy.h"

namespace
{
	class FWorldSceneLifecycleTestScene final : public Durin::FSceneInterface
	{
	public:
		auto AddOrReplacePrimitive(
			Durin::FPrimitiveSceneId,
			std::unique_ptr<Durin::FPrimitiveSceneProxy>,
			const Durin::FMatrix&,
			bool) -> void override
		{
		}

		auto RemovePrimitive(Durin::FPrimitiveSceneId) -> void override
		{
			++RemovePrimitiveCount;
		}

		auto UpdatePrimitiveTransform(Durin::FPrimitiveSceneId, const Durin::FMatrix&) -> void override
		{
		}

		auto UpdatePrimitiveVisibility(Durin::FPrimitiveSceneId, bool) -> void override
		{
		}

		auto UpdatePrimitiveMaterialBinding(
			Durin::FPrimitiveSceneId,
			const Durin::FMaterialRenderProxyBindingUpdate&) -> void override
		{
		}

		auto UpdateSkeletalMeshDynamicData(
			Durin::FPrimitiveSceneId,
			std::shared_ptr<const Durin::FSkeletalPosePalette>) -> void override
		{
		}

		auto UpdateSplineMeshDynamicData(
			Durin::FPrimitiveSceneId,
			Durin::FSplineMeshRenderDynamicData) -> void override
		{
		}

		auto AddLight(std::unique_ptr<Durin::FLightSceneProxy> Proxy) -> bool override
		{
			if (Proxy == nullptr) return false;
			Durin::FLightSceneProxy* Token = Proxy.get();
			LastAddedLight = Token;
			++AddLightCount;
			Lights.emplace(Token, std::move(Proxy));
			return true;
		}

		auto RemoveLight(Durin::FLightSceneProxy* Proxy) -> void override
		{
			LastRemovedLight = Proxy;
			++RemoveLightCount;
			if (const auto Found = Lights.find(Proxy); Found != Lights.end())
			{
				RetiredLights.push_back(std::move(Found->second));
				Lights.erase(Found);
			}
		}

		auto AddSkyBox(std::unique_ptr<Durin::FSkyBoxSceneProxy>) -> bool override
		{
			return false;
		}

		auto RemoveSkyBox(Durin::FSkyBoxSceneProxy*) -> void override
		{
		}

		auto AddVolumetricCloud(
			std::unique_ptr<Durin::FVolumetricCloudSceneProxy>) -> bool override
		{
			return false;
		}

		auto RemoveVolumetricCloud(
			Durin::FVolumetricCloudSceneProxy*) -> void override
		{
		}

		uint32 RemovePrimitiveCount = 0;
		uint32 AddLightCount = 0;
		uint32 RemoveLightCount = 0;
		Durin::FLightSceneProxy* LastAddedLight = nullptr;
		Durin::FLightSceneProxy* LastRemovedLight = nullptr;
		std::unordered_map<Durin::FLightSceneProxy*,
			std::unique_ptr<Durin::FLightSceneProxy>> Lights;
		std::vector<std::unique_ptr<Durin::FLightSceneProxy>> RetiredLights;
	};
}

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
