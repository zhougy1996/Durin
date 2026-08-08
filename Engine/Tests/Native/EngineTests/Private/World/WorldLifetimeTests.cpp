#include "WorldTestSupport.h"
#include "Engine/LightSceneProxy.h"
#include "Engine/SkyBoxSceneProxy.h"

namespace
{
	class FWorldSceneLifecycleTestScene final : public Durin::IScene
	{
	public:
		auto AddOrReplacePrimitive(
			Durin::FPrimitiveSceneId,
			std::unique_ptr<Durin::FPrimitiveSceneProxy>,
			const Durin::FMatrix&) -> void override
		{
		}

		auto RemovePrimitive(Durin::FPrimitiveSceneId) -> void override
		{
			++RemovePrimitiveCount;
		}

		auto UpdatePrimitiveTransform(Durin::FPrimitiveSceneId, const Durin::FMatrix&) -> void override
		{
		}

		auto UpdatePrimitiveMaterialBinding(
			Durin::FPrimitiveSceneId,
			const Durin::FMaterialRenderProxyBindingUpdate&) -> void override
		{
		}

		auto Release() -> void override
		{
		}

		auto AddOrReplaceDirectionalLight(Durin::FLightSceneId, std::unique_ptr<Durin::FDirectionalLightSceneProxy>) -> void override
		{
		}

		auto RemoveDirectionalLight(Durin::FLightSceneId) -> void override
		{
		}

		auto GetDirectionalLight(Durin::FDirectionalLightSceneData&) const -> bool override
		{
			return false;
		}

		auto AddOrReplaceSkyBox(Durin::FSkyBoxSceneId, Durin::FGuid, std::string, std::unique_ptr<Durin::FSkyBoxSceneProxy>) -> void override
		{
		}

		auto RemoveSkyBox(Durin::FSkyBoxSceneId) -> void override
		{
		}

		auto GetActiveSkyBox_RenderThread(Durin::FSkyBoxSceneData&) const -> bool override
		{
			return false;
		}

		auto GetSkyBoxCount_RenderThread() const -> size_t override
		{
			return 0;
		}

		Durin::uint32 RemovePrimitiveCount = 0;
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
