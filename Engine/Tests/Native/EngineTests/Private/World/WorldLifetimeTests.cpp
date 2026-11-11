#include "WorldTestSupport.h"
#include "NativePackageUnloadTestSupport.h"
#include "ComponentPSOPrecacheTestSupport.h"
#include "Engine/MapChange.h"
#include "Engine/WorldSubsystem.h"
#include "Engine/AssetManager.h"
#include "AssetRegistry/Catalog.h"
#include "Asset/StreamableHandle.h"
#include "Texture/Texture2D.h"
#include "NativeDObjectTestSupport.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/Class.h"
#include "Misc/MountPathTestSupport.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "PropertyEditor/PropertyEditing.h"
#include "Preview/PreviewScene.h"
#include "IRendererModule.h"
#include "Rendering/LightSceneProxy.h"
#include "Rendering/SplineMeshSceneProxy.h"
#include "Rendering/SkyBoxSceneProxy.h"
#include "Rendering/VolumetricCloudSceneProxy.h"

namespace Durin
{
	class FThrowingWorldSubsystem : public DWorldSubsystem
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FThrowingWorldSubsystem, DWorldSubsystem, FThrowingWorldSubsystem::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FThrowingWorldSubsystem)
	public:
		explicit FThrowingWorldSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}
		auto DoesSupportWorldType(EWorldType) const -> bool override { return true; }
		auto Initialize(FSubsystemCollection&) -> void override
		{
			LastWorld = GetWorld();
			throw std::runtime_error("World callback exception");
		}
		auto Deinitialize() noexcept -> void override { ++Cleanups; }
		inline static TWeakObjectPtr<DWorld> LastWorld;
		inline static int Cleanups = 0;
	};
	auto FThrowingWorldSubsystem::GetPrivateStaticClass() -> DClass*
	{
		static DClass* Class = nullptr;
		if (!Class)
			GetPrivateStaticClassBody("", "FThrowingWorldSubsystem", Class, nullptr, sizeof(FThrowingWorldSubsystem), alignof(FThrowingWorldSubsystem),
				EClassFlags::None, InternalConstructor<FThrowingWorldSubsystem>, &DWorldSubsystem::StaticClass);
		return Class;
	}
}

namespace
{
	auto CountAttachedChildEntries(const Durin::DSceneComponent* Parent, const Durin::DSceneComponent* Child) -> size_t
	{
		return static_cast<size_t>(std::ranges::count_if(Parent->GetAttachChildren(),
			[Child](const auto& Entry) { return Entry.Get() == Child; }));
	}

	class FWorldSceneLifecycleTestScene final : public Durin::FSceneInterface
	{
	public:
		explicit FWorldSceneLifecycleTestScene(int* InDestructions = nullptr) : Destructions(InDestructions) {}
		~FWorldSceneLifecycleTestScene() override { if (Destructions) ++*Destructions; }
		int* Destructions;
		auto Release() -> void override {}
		auto UpdatePrimitiveCustomData(Durin::FPrimitiveComponentId, Durin::FPrimitiveCustomData) -> void override {}
		auto UpdateStaticMeshRenderInstances(Durin::FPrimitiveComponentId, Durin::FStaticMeshRenderInstances) -> void override {}
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
			LightLocations[Light] = Light->GetWorldLocation();
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
			LightLocations.erase(Light);
		}

		auto AddSkyBox(Durin::DSkyBoxComponent*) -> void override {}
		auto RemoveSkyBox(Durin::DSkyBoxComponent*) -> void override {}
		auto AddProceduralSky(Durin::DProceduralSkyComponent*) -> void override {}
		auto RemoveProceduralSky(Durin::DProceduralSkyComponent*) -> void override {}
		auto UpdateSkyLighting() -> void override {}
		auto AddSkyLight(Durin::DSkyLightComponent*) -> void override {}
		auto RemoveSkyLight(Durin::DSkyLightComponent*) -> void override {}
		auto AddVolumetricCloud(
			Durin::DVolumetricCloudComponent*
		) -> void override {}
		auto RemoveVolumetricCloud(
			Durin::DVolumetricCloudComponent*
		) -> void override {}

		auto UpdatePrimitiveTransform(Durin::FPrimitiveComponentId, const Durin::FMatrix&) -> void override
		{
		}

		auto UpdatePrimitiveVisibility(Durin::FPrimitiveComponentId, bool) -> void override
		{
		}

		auto UpdatePrimitiveMaterialBinding(
			Durin::FPrimitiveComponentId,
			const Durin::FMaterialRenderProxyBindingUpdate&
		) -> void override
		{
		}

		auto UpdateMaterialParameterCollection(
			std::shared_ptr<const Durin::FMaterialParameterCollectionSnapshot>)
			-> void override
		{
		}

		auto RemoveMaterialParameterCollection(Durin::FGuid) -> void override
		{
		}

		auto UpdateSplineMeshDynamicData(
			Durin::FPrimitiveComponentId,
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
		std::unordered_map<Durin::DLightComponent*, Durin::FVector3> LightLocations;
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
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
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
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);

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
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
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
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
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

	Light->SetActorHiddenInGame(true);
	EXPECT_EQ(Scene.RemoveLightCount, 2u);
	EXPECT_EQ(Scene.LastRemovedLight, RebuiltToken);
	EXPECT_TRUE(Scene.Lights.empty());

	Light->SetActorHiddenInGame(false);
	EXPECT_EQ(Scene.AddLightCount, 3u);
	Durin::FLightSceneProxy* VisibleToken = Scene.LastAddedLight;
	Component->UnregisterComponent();
	EXPECT_EQ(Scene.RemoveLightCount, 3u);
	EXPECT_EQ(Scene.LastRemovedLight, VisibleToken);
	EXPECT_TRUE(Scene.Lights.empty());

	World->SetRenderScene(nullptr);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FWorldTests, ComponentReparentUndoRedoRestoresHierarchyTransformsAndLightPublication)
{
	for (const auto Rule : {Durin::EAttachmentTransformRule::KeepRelative, Durin::EAttachmentTransformRule::KeepWorld,
		Durin::EAttachmentTransformRule::SnapToTarget})
	{
		auto* World = CreateWorld(nullptr, Durin::EWorldType::Editor);
		FWorldSceneLifecycleTestScene Scene;
		World->SetRenderScene(&Scene);
		auto* First = World->SpawnActor<Durin::AStaticMeshActor>("FirstParent")->GetRootComponent();
		auto* Second = World->SpawnActor<Durin::AStaticMeshActor>("SecondParent")->GetRootComponent();
		auto* Light = World->SpawnActor<Durin::ADirectionalLightActor>("AttachedLight");
		auto* Child = Light->GetLightComponent();
		auto* Descendant = World->SpawnActor<Durin::AStaticMeshActor>("Descendant")->GetRootComponent();
		First->SetWorldLocation({10.0, 0.0, 0.0});
		Second->SetWorldLocation({20.0, 0.0, 0.0});
		Child->SetRelativeLocation({2.0, 0.0, 0.0});
		Descendant->SetRelativeLocation({3.0, 0.0, 0.0});
		ASSERT_TRUE(Child->AttachToComponent(First, Durin::EAttachmentTransformRule::KeepRelative));
		ASSERT_TRUE(Descendant->AttachToComponent(Child, Durin::EAttachmentTransformRule::KeepRelative));
		Durin::Tests::FTestTransactorOwner Transactions;
		{
			Durin::FScopedTransaction Transaction(Transactions.Get(), {"test", "Reparent component"});
			ASSERT_TRUE(Child->HasAnyObjectFlags(Durin::EObjectFlags::Transactional)) << "child=" << int(Child->GetObjectFlags()) << " outer=" << int(Child->GetOuter()->GetObjectFlags()) << " world=" << int(World->GetWorldType());
			ASSERT_TRUE(Child->Modify());
			ASSERT_TRUE(Child->AttachToComponent(Second, Rule));
		}
		const auto AfterRelative = Child->GetRelativeTransform();
		auto ExpectAttachment = [&](Durin::DSceneComponent* Parent, Durin::DSceneComponent* Other, double X) {
			EXPECT_EQ(Child->GetAttachParent(), Parent);
			EXPECT_EQ(CountAttachedChildEntries(Parent, Child), 1u);
			EXPECT_EQ(CountAttachedChildEntries(Other, Child), 0u);
			EXPECT_EQ(Child->GetWorldLocation(), Durin::FVector3(X, 0.0, 0.0));
			EXPECT_EQ(Descendant->GetWorldLocation(), Durin::FVector3(X + 3.0, 0.0, 0.0));
			ASSERT_TRUE(Scene.LightLocations.contains(Child));
			EXPECT_EQ(Scene.LightLocations.at(Child), Child->GetWorldLocation());
		};
		uint64 Revision = World->GetCurrentLevel()->GetEditorActorHierarchyRevision();
		ASSERT_TRUE(Transactions->Undo()) << Durin::FormatTransactorResult(Transactions->GetLastResult());
		EXPECT_GT(World->GetCurrentLevel()->GetEditorActorHierarchyRevision(), Revision);
		ExpectAttachment(First, Second, 12.0);
		Second->SetWorldLocation({120.0, 0.0, 0.0});
		ExpectAttachment(First, Second, 12.0);
		First->SetWorldLocation({30.0, 0.0, 0.0});
		ExpectAttachment(First, Second, 32.0);
		Revision = World->GetCurrentLevel()->GetEditorActorHierarchyRevision();
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_GT(World->GetCurrentLevel()->GetEditorActorHierarchyRevision(), Revision);
		const double ReappliedX = 120.0 + AfterRelative.Translation.x;
		ExpectAttachment(Second, First, ReappliedX);
		First->SetWorldLocation({40.0, 0.0, 0.0});
		ExpectAttachment(Second, First, ReappliedX);
		Second->SetWorldLocation({130.0, 0.0, 0.0});
		ExpectAttachment(Second, First, ReappliedX + 10.0);
		ASSERT_TRUE(Transactions->Undo()) << Durin::FormatTransactorResult(Transactions->GetLastResult());
		ExpectAttachment(First, Second, 42.0);
		ASSERT_TRUE(Transactions->TryReset());
		World->SetRenderScene(nullptr);
		Durin::MarkObjectHierarchyAsGarbage(World);
		Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	}
}

TEST(FWorldTests, ComponentDetachUndoRedoRestoresParentAndTransform)
{
	for (const auto Rule : {Durin::EDetachmentTransformRule::KeepRelative, Durin::EDetachmentTransformRule::KeepWorld})
	{
		auto* World = CreateWorld(nullptr, Durin::EWorldType::Editor);
		auto* Parent = World->SpawnActor<Durin::AStaticMeshActor>("Parent")->GetRootComponent();
		auto* Child = World->SpawnActor<Durin::AStaticMeshActor>("Child")->GetRootComponent();
		Parent->SetWorldLocation({10.0, 0.0, 0.0});
		Child->SetRelativeLocation({2.0, 0.0, 0.0});
		ASSERT_TRUE(Child->AttachToComponent(Parent, Durin::EAttachmentTransformRule::KeepRelative));
		Durin::Tests::FTestTransactorOwner Transactions;
		{
			Durin::FScopedTransaction Transaction(Transactions.Get(), {"test", "Detach component"});
			ASSERT_TRUE(Child->HasAnyObjectFlags(Durin::EObjectFlags::Transactional)) << "child=" << int(Child->GetObjectFlags()) << " outer=" << int(Child->GetOuter()->GetObjectFlags()) << " world=" << int(World->GetWorldType());
			ASSERT_TRUE(Child->Modify());
			ASSERT_TRUE(Child->DetachFromComponent(Rule));
		}
		const auto DetachedLocation = Child->GetWorldLocation();
		for (int32 Repeat = 0; Repeat < 2; ++Repeat)
		{
			ASSERT_TRUE(Transactions->Undo()) << Durin::FormatTransactorResult(Transactions->GetLastResult());
			EXPECT_EQ(Child->GetAttachParent(), Parent);
			EXPECT_EQ(CountAttachedChildEntries(Parent, Child), 1u);
			EXPECT_EQ(Child->GetWorldLocation(), Parent->GetWorldLocation() + Durin::FVector3(2.0, 0.0, 0.0));
			ASSERT_TRUE(Transactions->Redo());
			EXPECT_EQ(Child->GetAttachParent(), nullptr);
			EXPECT_TRUE(Parent->GetAttachChildren().empty());
			EXPECT_EQ(Child->GetWorldLocation(), DetachedLocation);
			Parent->SetWorldLocation({30.0 + Repeat, 0.0, 0.0});
			EXPECT_EQ(Child->GetWorldLocation(), DetachedLocation);
		}
		ASSERT_TRUE(Transactions->TryReset());
		Durin::MarkObjectHierarchyAsGarbage(World);
		Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	}
}

TEST(FWorldTests, ComponentHierarchyUndoRedoHandlesBothRecordOrders)
{
	for (const bool bParentFirst : {false, true})
	{
		auto* World = CreateWorld(nullptr, Durin::EWorldType::Editor);
		auto* First = World->SpawnActor<Durin::AStaticMeshActor>("First")->GetRootComponent();
		auto* Second = World->SpawnActor<Durin::AStaticMeshActor>("Second")->GetRootComponent();
		First->SetRelativeLocation({1.0, 0.0, 0.0});
		Second->SetWorldLocation({10.0, 0.0, 0.0});
		ASSERT_TRUE(First->AttachToComponent(Second, Durin::EAttachmentTransformRule::KeepRelative));
		Durin::Tests::FTestTransactorOwner Transactions;
		{
			Durin::FScopedTransaction Transaction(Transactions.Get(), {"test", "Reverse hierarchy"});
			(void)(bParentFirst ? Second : First)->Modify();
			(void)(bParentFirst ? First : Second)->Modify();
			ASSERT_TRUE(First->DetachFromComponent(Durin::EDetachmentTransformRule::KeepWorld));
			ASSERT_TRUE(Second->AttachToComponent(First, Durin::EAttachmentTransformRule::KeepWorld));
		}
		ASSERT_TRUE(Transactions->Undo()) << Durin::FormatTransactorResult(Transactions->GetLastResult());
		EXPECT_EQ(First->GetAttachParent(), Second);
		EXPECT_EQ(Second->GetAttachParent(), nullptr);
		EXPECT_EQ(CountAttachedChildEntries(Second, First), 1u);
		EXPECT_TRUE(First->GetAttachChildren().empty());
		EXPECT_EQ(First->GetWorldLocation(), Durin::FVector3(11.0, 0.0, 0.0));
		EXPECT_EQ(Second->GetWorldLocation(), Durin::FVector3(10.0, 0.0, 0.0));
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(Second->GetAttachParent(), First);
		EXPECT_EQ(First->GetAttachParent(), nullptr);
		EXPECT_EQ(CountAttachedChildEntries(First, Second), 1u);
		EXPECT_TRUE(Second->GetAttachChildren().empty());
		First->SetWorldLocation({50.0, 0.0, 0.0});
		EXPECT_EQ(Second->GetWorldLocation(), Durin::FVector3(49.0, 0.0, 0.0));
		ASSERT_TRUE(Transactions->TryReset());
		Durin::MarkObjectHierarchyAsGarbage(World);
		Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	}
}

TEST(FWorldTests, UnregisteredComponentAttachmentUndoRedoRestoresRotationAndScale)
{
	auto* World = CreateWorld(nullptr, Durin::EWorldType::Editor);
	auto* First = World->SpawnActor<Durin::AStaticMeshActor>("First")->GetRootComponent();
	auto* Second = World->SpawnActor<Durin::AStaticMeshActor>("Second")->GetRootComponent();
	auto* Child = World->SpawnActor<Durin::AStaticMeshActor>("Child")->GetRootComponent();
	Child->UnregisterComponent();
	Durin::FTransform ParentTransform;
	ParentTransform.Translation = {10.0, 20.0, 30.0};
	ParentTransform.Rotation = {0.7071067811865476, 0.0, 0.0, 0.7071067811865476};
	ParentTransform.Scale3D = {2.0, 2.0, 2.0};
	First->SetWorldTransform(ParentTransform);
	ParentTransform.Translation = {-10.0, 5.0, 0.0};
	ParentTransform.Rotation = {0.7071067811865476, 0.0, 0.0, -0.7071067811865476};
	ParentTransform.Scale3D = {3.0, 3.0, 3.0};
	Second->SetWorldTransform(ParentTransform);
	Child->SetRelativeLocation({1.0, 2.0, 3.0});
	ASSERT_TRUE(Child->AttachToComponent(First, Durin::EAttachmentTransformRule::KeepRelative));
	const auto BeforeWorld = Child->GetComponentToWorldMatrix();
	Durin::Tests::FTestTransactorOwner Transactions;
	{
		Durin::FScopedTransaction Transaction(Transactions.Get(), {"test", "Reparent unregistered component"});
		ASSERT_TRUE(Child->HasAnyObjectFlags(Durin::EObjectFlags::Transactional)) << "child=" << int(Child->GetObjectFlags()) << " outer=" << int(Child->GetOuter()->GetObjectFlags()) << " world=" << int(World->GetWorldType());
			ASSERT_TRUE(Child->Modify());
		ASSERT_TRUE(Child->AttachToComponent(Second, Durin::EAttachmentTransformRule::KeepRelative));
	}
	const auto AfterWorld = Child->GetComponentToWorldMatrix();
	auto ExpectWorld = [&](const Durin::FMatrix& Expected) {
		const auto Actual = Child->GetComponentToWorldMatrix();
		for (uint32 Column = 0; Column < 4; ++Column)
			for (uint32 Row = 0; Row < 4; ++Row)
				EXPECT_NEAR(Actual[Column][Row], Expected[Column][Row], 1.e-8);
		EXPECT_FALSE(Child->IsRegistered());
	};
	ASSERT_TRUE(Transactions->Undo()) << Durin::FormatTransactorResult(Transactions->GetLastResult());
	ExpectWorld(BeforeWorld);
	ASSERT_TRUE(Transactions->Redo());
	ExpectWorld(AfterWorld);
	ASSERT_TRUE(Transactions->TryReset());
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FWorldTests, RawHiddenAssignmentHasNoComponentSideEffects)
{
	Durin::DWorld* World = CreateWorld();
	FWorldSceneLifecycleTestScene Scene;
	World->SetRenderScene(&Scene);
	auto* Light = World->SpawnActor<Durin::ADirectionalLightActor>("RawHiddenLight");
	ASSERT_TRUE(Scene.Lights.contains(Light->GetLightComponent()));
	Light->SetHidden(true);
	EXPECT_TRUE(Light->IsHidden());
	EXPECT_TRUE(Scene.Lights.contains(Light->GetLightComponent()));
	Light->PostEditChange();
	EXPECT_TRUE(Scene.Lights.empty());
	Light->SetActorHiddenInGame(false);
	EXPECT_TRUE(Scene.Lights.contains(Light->GetLightComponent()));
	const uint32 Added = Scene.AddLightCount;
	Light->SetActorHiddenInGame(false);
	EXPECT_EQ(Scene.AddLightCount, Added);
	World->SetRenderScene(nullptr);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FWorldTests, ActorVisibilityUndoRedoSynchronizesComponentSceneState)
{
	for (const bool bInitiallyHidden : {false, true})
	{
		Durin::DWorld* World = CreateWorld(nullptr, Durin::EWorldType::Editor);
		FWorldSceneLifecycleTestScene Scene;
		World->SetRenderScene(&Scene);
		auto* Light = World->SpawnActor<Durin::ADirectionalLightActor>("VisibilityLight");
		auto* Mesh = World->SpawnActor<Durin::AStaticMeshActor>("VisibilityMesh");
		// Actor visibility must not overwrite the component's independent visibility.
		Mesh->GetStaticMeshComponent()->SetVisible(false);
		Light->SetActorHiddenInGame(bInitiallyHidden);
		Mesh->SetActorHiddenInGame(bInitiallyHidden);
		Durin::Tests::FTestTransactorOwner Transactions;
		{
			Durin::FScopedTransaction Transaction(Transactions.Get(), {"test", "Toggle actor visibility"});
			(void)Light->Modify();
			(void)Mesh->Modify();
			Light->SetActorHiddenInGame(!bInitiallyHidden);
			Mesh->SetActorHiddenInGame(!bInitiallyHidden);
		}
		auto ExpectVisibility = [&](bool bHidden) {
			EXPECT_EQ(Light->IsHidden(), bHidden);
			EXPECT_EQ(Mesh->IsHidden(), bHidden);
			EXPECT_EQ(Scene.Lights.contains(Light->GetLightComponent()), !bHidden);
			EXPECT_FALSE(Mesh->GetStaticMeshComponent()->IsVisible());
		};
		ExpectVisibility(!bInitiallyHidden);
		ASSERT_TRUE(Transactions->Undo()) << Durin::FormatTransactorResult(Transactions->GetLastResult());
		ExpectVisibility(bInitiallyHidden);
		ASSERT_TRUE(Transactions->Redo());
		ExpectVisibility(!bInitiallyHidden);
		ASSERT_TRUE(Transactions->Undo()) << Durin::FormatTransactorResult(Transactions->GetLastResult());
		ExpectVisibility(bInitiallyHidden);
		ASSERT_TRUE(Transactions->TryReset());
		World->SetRenderScene(nullptr);
		Durin::MarkObjectHierarchyAsGarbage(World);
		Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	}
}

TEST(FWorldTests, ReflectedActorVisibilitySynchronizesInteractiveCancelAndHistory)
{
	Durin::DWorld* World = CreateWorld();
	FWorldSceneLifecycleTestScene Scene;
	World->SetRenderScene(&Scene);
	auto* Light = World->SpawnActor<Durin::ADirectionalLightActor>("ReflectedVisibilityLight");
	auto* Hidden = Light->GetClass()->FindPropertyByName("bHidden");
	ASSERT_NE(Hidden, nullptr);
	Light->SetActorHiddenInGame(true);
	Durin::FPropertyValueSnapshot HiddenValue;
	ASSERT_TRUE(Durin::CapturePropertyValue(Hidden, Light, 0, HiddenValue));
	Light->SetActorHiddenInGame(false);
	Durin::Tests::FTestTransactorOwner Transactions;
	Durin::FPropertyEditSession Session;
	const auto Target = Durin::FPropertyEditTarget::ForMember(Light, Hidden);
	ASSERT_TRUE(Session.Begin(Target, "Hide light", *Transactions.Get()));
	ASSERT_TRUE(Session.Apply(HiddenValue));
	EXPECT_TRUE(Light->IsHidden());
	EXPECT_TRUE(Scene.Lights.empty());
	ASSERT_TRUE(Session.Cancel());
	EXPECT_FALSE(Light->IsHidden());
	EXPECT_TRUE(Scene.Lights.contains(Light->GetLightComponent()));
	EXPECT_FALSE(Transactions->CanUndo());
	ASSERT_TRUE(Session.Begin(Target, "Hide light", *Transactions.Get()));
	ASSERT_TRUE(Session.Apply(HiddenValue));
	ASSERT_TRUE(Session.Commit());
	EXPECT_TRUE(Scene.Lights.empty());
	ASSERT_TRUE(Transactions->Undo()) << Durin::FormatTransactorResult(Transactions->GetLastResult());
	EXPECT_TRUE(Scene.Lights.contains(Light->GetLightComponent()));
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_TRUE(Scene.Lights.empty());
	ASSERT_TRUE(Transactions->TryReset());
	World->SetRenderScene(nullptr);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FWorldTests, ComponentIdsRemainStableAcrossRenderStateAndRegistrationChanges)
{
	Durin::DWorld* World = CreateWorld();
	FWorldSceneLifecycleTestScene Scene;
	World->SetRenderScene(&Scene);
	auto* Mesh = World->SpawnActor<Durin::AStaticMeshActor>("IdentityMesh")->GetStaticMeshComponent();
	auto* Light = World->SpawnActor<Durin::ADirectionalLightActor>("IdentityLight")->GetLightComponent();
	const auto PrimitiveId = Mesh->GetPrimitiveComponentId();
	const auto LightId = Light->GetLightComponentId();
	ASSERT_TRUE(PrimitiveId.IsValid());
	ASSERT_TRUE(LightId.IsValid());

	Mesh->RecreateRenderState();
	Light->SetIntensity(2.0f);
	EXPECT_EQ(Mesh->GetPrimitiveComponentId(), PrimitiveId);
	EXPECT_EQ(Light->GetLightComponentId(), LightId);
	Mesh->UnregisterComponent();
	Light->UnregisterComponent();
	EXPECT_EQ(Mesh->GetPrimitiveComponentId(), PrimitiveId);
	EXPECT_EQ(Light->GetLightComponentId(), LightId);
	Mesh->RegisterComponent();
	Light->RegisterComponent();
	EXPECT_EQ(Mesh->GetPrimitiveComponentId(), PrimitiveId);
	EXPECT_EQ(Light->GetLightComponentId(), LightId);

	auto* OtherMesh = World->SpawnActor<Durin::AStaticMeshActor>("OtherMesh")->GetStaticMeshComponent();
	auto* OtherLight = World->SpawnActor<Durin::ADirectionalLightActor>("OtherLight")->GetLightComponent();
	EXPECT_NE(OtherMesh->GetPrimitiveComponentId(), PrimitiveId);
	EXPECT_NE(OtherLight->GetLightComponentId(), LightId);
	World->SetRenderScene(nullptr);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FEngineObjectTests, EngineAndWorldHaveReflectedOwnership)
{
	InitializeDObjectSystem();
	Durin::DEngine* Engine = Durin::NewObject<Durin::DEngine>(nullptr, "TestEngine");
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(Engine, "MainWorld");
	EXPECT_NO_THROW(World->InitializeSubsystems());
	Durin::TObjectPtr<Durin::DEngine> EnginePtr = Engine;
	Durin::TObjectPtr<Durin::DWorld> WorldPtr = World;

	EXPECT_EQ(Engine->GetClass(), Durin::DEngine::StaticClass());
	EXPECT_EQ(World->GetClass(), Durin::DWorld::StaticClass());
	EXPECT_EQ(World->GetOuter(), Engine);

	Durin::MarkObjectHierarchyAsGarbage(Engine);
	EXPECT_FALSE(EnginePtr.IsValid());
	EXPECT_FALSE(WorldPtr.IsValid());
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(EnginePtr.Get(), nullptr);
	EXPECT_EQ(WorldPtr.Get(), nullptr);
}

TEST(FEngineObjectTests, RootedEngineSurvivesGarbageCollection)
{
	InitializeDObjectSystem();
	Durin::DEngine* Engine = Durin::NewObject<Durin::DEngine>(nullptr, "RootedEngine");
	Durin::TObjectPtr<Durin::DEngine> EnginePtr = Engine;

	Durin::AddToRoot(Engine);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(EnginePtr.Get(), Engine);

	Durin::RemoveFromRoot(Engine);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(EnginePtr.Get(), nullptr);
}

TEST(FEngineObjectTests, GameEngineHasConcreteRuntimeClass)
{
	InitializeDObjectSystem();
	Durin::DGameEngine* Engine = Durin::NewObject<Durin::DGameEngine>(nullptr, "GameEngine");

	EXPECT_EQ(Engine->GetClass(), Durin::DGameEngine::StaticClass());
	EXPECT_TRUE(Engine->IsA<Durin::DEngine>());

	Durin::MarkObjectHierarchyAsGarbage(Engine);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

namespace
{
	// Supplies CPU-only scene ownership so preview retirement can be tested without a GPU host.
	class FPreviewLifecycleRenderer : public Durin::IRendererModule
	{
	public:
		int RenderCalls = 0;
		int DestroyedScenes = 0;
		auto CreateScene() -> Durin::FScenePtr override
		{
			return {new FWorldSceneLifecycleTestScene(&DestroyedScenes), Durin::FSceneDeleter([](Durin::FSceneInterface* Scene) { delete Scene; })};
		}
		auto CreateViewState() -> Durin::FSceneViewStateOwner override { return {}; }
		auto InvalidateViewState(Durin::FSceneViewStateId) -> void override {}
		auto InvalidateAllViewStates() -> void override {}
		auto RenderView(Durin::FRHICommandListImmediate&, const Durin::FSceneViewRenderRequest&) -> Durin::ERenderViewResult override
		{
			++RenderCalls;
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
	Durin::DCollisionDebugSubsystem* Service = nullptr;
	bool bRetired = false;
	Durin::FObjectReferencePurgeRegistration Observer([&](Durin::DObject& Object) {
		if (&Object != Service) return;
		bRetired = true;
		EXPECT_TRUE(Service->GetWorld()->IsShutdownRequested());
	});
	{
		Durin::FPreviewScene Preview("SubsystemPreview");
		EXPECT_TRUE(Preview.IsAvailable()) << Preview.GetDiagnostic();
		if (Preview.IsAvailable())
		{
			EXPECT_EQ(Preview.GetWorld()->GetWorldType(), Durin::EWorldType::EditorPreview);
			EXPECT_TRUE(Preview.GetWorld()->AreSubsystemsInitialized());
			EXPECT_FALSE(Preview.GetWorld()->HasBegunPlay());
			auto* Debug = Preview.GetWorld()->GetSubsystem<Durin::DCollisionDebugSubsystem>();
			EXPECT_NE(Debug, nullptr);
			Service = Debug;
		}
	}
	EXPECT_TRUE(bRetired);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	Durin::GEngine = PreviousEngine;
}

TEST(FWorldTests, WorldShutdownCompletesAfterSubsystemInitializationAlreadyCleanedTheCollection)
{
	using namespace Durin;
	InitializeDObjectSystem();
	const std::array Classes{FThrowingWorldSubsystem::StaticClass()};
	DObjectForceRegistration(Classes.front());
	ASSERT_TRUE(Private::CreateClassDefaultObjectsForBatch(Classes));
	FWorldSubsystemRegistration Throwing({.Type = Classes.front()});
	FThrowingWorldSubsystem::Cleanups = 0;
	FWorldSceneLifecycleTestScene Scene;
	TStrongObjectPtr<DEngine> Host(NewObject<DEngine>(nullptr, "PartialWorldShutdownHost"));
	TStrongObjectPtr<DWorld> World(NewObject<DWorld>(Host.Get(), "PartialWorldShutdown"));
	World->SetRenderScene(&Scene);
	EXPECT_THROW(World->InitializeSubsystems(), std::runtime_error);
	EXPECT_FALSE(World->AreSubsystemsInitialized());
	EXPECT_EQ(World->GetRenderScene(), &Scene);
	World->CleanupWorld();
	EXPECT_EQ(World->GetRenderScene(), nullptr);
	EXPECT_TRUE(Host->GetWorldContexts().empty());
	World->CleanupWorld();
	EXPECT_EQ(FThrowingWorldSubsystem::Cleanups, 1);
	Host->PreExit();
	MarkObjectHierarchyAsGarbage(Host.Get());
	World.Reset(); Host.Reset(); CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FWorldTests, PreviewConstructionExceptionCleansWorldAndSceneThenPropagates)
{
	InitializeDObjectSystem();
	FPreviewLifecycleRenderer Renderer;
	FPreviewLifecycleEngine Engine(Renderer);
	auto* PreviousEngine = std::exchange(Durin::GEngine, &Engine);
	const std::array Classes{Durin::FThrowingWorldSubsystem::StaticClass()};
	Durin::DObjectForceRegistration(Classes.front());
	ASSERT_TRUE(Durin::Private::CreateClassDefaultObjectsForBatch(Classes));
	Durin::FWorldSubsystemRegistration Throwing({.Type = Classes.front()});
	Durin::FThrowingWorldSubsystem::Cleanups = 0;
	EXPECT_THROW(Durin::FPreviewScene("FailedSubsystemPreview"), std::runtime_error);
	EXPECT_EQ(Durin::FThrowingWorldSubsystem::Cleanups, 1);
	EXPECT_FALSE(Durin::FThrowingWorldSubsystem::LastWorld.IsValid());
	EXPECT_TRUE(Engine.GetWorldContexts().empty());
	EXPECT_EQ(Renderer.DestroyedScenes, 1);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	Durin::GEngine = PreviousEngine;
}

TEST(FWorldTests, ActorModifyRecordsTheNativeRootTransform)
{
	using namespace Durin; using namespace Durin;
	auto* World = CreateWorld(nullptr, EWorldType::Editor);
	auto* Actor = World->SpawnActor<AStaticMeshActor>("ActorModifyTransform");
	Tests::FTestTransactorOwner Transactions;
	{
		FScopedTransaction Scope(Transactions.Get(), {"test", "Move actor through Modify"});
		ASSERT_TRUE(Actor->Modify());
		FTransform Transform = Actor->GetActorTransform(); Transform.Translation = {7.0, 8.0, 9.0};
		Actor->SetActorTransform(Transform);
	}
	ASSERT_TRUE(Transactions->Undo()); EXPECT_EQ(Actor->GetActorTransform().Translation, FVector3(0.0));
	ASSERT_TRUE(Transactions->Redo()); EXPECT_EQ(Actor->GetActorTransform().Translation, FVector3(7.0, 8.0, 9.0));
	ASSERT_TRUE(Transactions->TryReset()); MarkObjectHierarchyAsGarbage(World); CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FWorldTests, SceneReplacementPreservesPlayingComponentLifecycleAndBoundTimers)
{
	auto* World = CreateWorld(); FWorldSceneLifecycleTestScene First, Second;
	World->SetRenderScene(&First);
	auto* Actor = World->SpawnActor<Durin::ADirectionalLightActor>("ReregisterLight");
	auto* Component = Actor->GetLightComponent();
	ASSERT_TRUE(World->BeginPlay({}));
	const auto Before = Component->CaptureLifecycleState();
	int Calls = 0;
	const auto Timer = World->GetTimerManager().SetTimerForObjectNextTick(Component, [&](Durin::DObject&) { ++Calls; });
	World->SetRenderScene(&Second);
	EXPECT_EQ(Component->CaptureLifecycleState(), Before);
	EXPECT_EQ(First.RemoveLightCount, 1u); EXPECT_EQ(Second.AddLightCount, 1u);
	EXPECT_TRUE(First.Lights.empty()); EXPECT_EQ(Second.Lights.size(), 1u);
	EXPECT_TRUE(World->GetTimerManager().IsTimerActive(Timer));
	World->Tick({.DeltaSeconds = 0.1f}); EXPECT_EQ(Calls, 1);
	World->SetRenderScene(nullptr); EXPECT_TRUE(Second.Lights.empty());
	World->EndPlay(); Durin::MarkObjectHierarchyAsGarbage(World); Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

namespace Durin
{
	struct FMapChangeTestAccess
	{
		static auto Tick(DEngine& Engine) -> void { Engine.TickMapChange(); }
		static auto SetRenderer(DEngine& Engine, IRendererModule& Renderer) -> void { Engine.RendererModule = &Renderer; }
	};
}

TEST(FWorldTests, CanceledMapChangePreservesTheActiveWorldAndCompletesAtTheFrameBoundary)
{
	using namespace Durin;
	const std::array MountDefinitions{FMountPoint{.VirtualRoot = "/SceneCancellation/", .Owner = EMountOwner::Test,
		.Root = std::filesystem::current_path()}};
	Testing::FScopedMountRegistryFixture Mounts(MountDefinitions);
	ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
	InitializeDObjectSystem();
	TStrongObjectPtr<DEngine> Host(NewObject<DEngine>(nullptr, "SceneCancellationHost"));
	auto* Previous = CreateWorld(Host.Get(), EWorldType::Editor);
	Host->SetWorld(Previous);
	auto* Level = Previous->GetCurrentLevel();
	FSoftObjectPath Path;
	ASSERT_TRUE(FSoftObjectPath::TryCreate("/SceneCancellation/Candidate.Candidate", Path));
	int Completions = 0;
	ASSERT_TRUE(Host->OpenLevel({.Path = Path,
		.OnComplete = [&](const FMapChangeProgress& Progress) {
			++Completions;
			EXPECT_EQ(Progress.Phase, EMapChangePhase::Canceled);
			EXPECT_FALSE(Host->IsPreparingMapChange());
		}}));
	EXPECT_FALSE(Host->OpenLevel({.Path = Path}));
	EXPECT_FALSE(Host->OpenLevel(Path));
	EXPECT_TRUE(Host->IsPreparingMapChange());
	EXPECT_EQ(Host->GetMapChangeProgress()->Phase, EMapChangePhase::LoadingAssets);
	EXPECT_EQ(Host->GetWorld(), Previous);
	Host->CancelPendingMapChange();
	Host->CancelPendingMapChange();
	EXPECT_EQ(Completions, 0);
	EXPECT_EQ(Host->GetMapChangeProgress()->Phase, EMapChangePhase::Canceled);
	FMapChangeTestAccess::Tick(*Host);
	FMapChangeTestAccess::Tick(*Host);
	EXPECT_EQ(Completions, 1);
	EXPECT_FALSE(Host->IsPreparingMapChange());
	EXPECT_EQ(Host->GetWorld(), Previous);
	EXPECT_EQ(Previous->GetCurrentLevel(), Level);
	EXPECT_FALSE(Previous->IsShutdownRequested());
	const auto Completed = Host->GetMapChangeProgress();
	ASSERT_TRUE(Completed);
	EXPECT_EQ(Completed->Phase, EMapChangePhase::Canceled);
	EXPECT_EQ(Host->GetMapChangeProgress()->ElapsedSeconds, Completed->ElapsedSeconds);
	Host->PreExit();
	EXPECT_FALSE(Host->OpenLevel({.Path = Path}));
	EXPECT_FALSE(Host->OpenLevel(Path));
	auto* Retired = Host.Get();
	Host.Reset();
	MarkObjectHierarchyAsGarbage(Retired);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FWorldTests, MapChangeCompletionCanStartAnotherChangeWithoutOverwritingItsProgress)
{
	using namespace Durin;
	const std::array MountDefinitions{FMountPoint{.VirtualRoot = "/SceneCompletion/", .Owner = EMountOwner::Test,
		.Root = std::filesystem::current_path()}};
	Testing::FScopedMountRegistryFixture Mounts(MountDefinitions);
	ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
	InitializeDObjectSystem();
	TStrongObjectPtr<DEngine> Host(NewObject<DEngine>(nullptr, "MapCompletionHost"));
	FSoftObjectPath Path;
	ASSERT_TRUE(FSoftObjectPath::TryCreate("/SceneCompletion/Candidate.Candidate", Path));
	uint64 FirstId = 0;
	int Completions = 0;
	ASSERT_TRUE(Host->OpenLevel({.Path = Path, .OnComplete = [&](const FMapChangeProgress& Progress) {
		++Completions;
		EXPECT_EQ(Progress.Id, FirstId);
		EXPECT_TRUE(Host->OpenLevel({.Path = Path}));
		EXPECT_NE(Host->GetMapChangeProgress()->Id, Progress.Id);
		throw std::runtime_error("Completion failure after starting another map change");
	}}));
	FirstId = Host->GetMapChangeProgress()->Id;
	Host->CancelPendingMapChange();
	FMapChangeTestAccess::Tick(*Host);
	EXPECT_EQ(Completions, 1);
	EXPECT_TRUE(Host->IsPreparingMapChange());
	EXPECT_EQ(Host->GetMapChangeProgress()->Phase, EMapChangePhase::LoadingAssets);
	Host->CancelPendingMapChange();
	FMapChangeTestAccess::Tick(*Host);
	EXPECT_EQ(Completions, 1);
	Host->PreExit();
	auto* Retired = Host.Get(); Host.Reset(); MarkObjectHierarchyAsGarbage(Retired); CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FWorldTests, MapChangeCompletionExceptionsDoNotEscapeOrRedeliver)
{
	using namespace Durin;
	const std::array MountDefinitions{FMountPoint{.VirtualRoot = "/CompletionFailure/", .Owner = EMountOwner::Test,
		.Root = std::filesystem::current_path()}};
	Testing::FScopedMountRegistryFixture Mounts(MountDefinitions);
	ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
	InitializeDObjectSystem();
	TStrongObjectPtr<DEngine> Host(NewObject<DEngine>(nullptr, "MapCompletionFailureHost"));
	FSoftObjectPath Path;
	ASSERT_TRUE(FSoftObjectPath::TryCreate("/CompletionFailure/Candidate.Candidate", Path));
	for (bool bStandardException : {true, false})
	{
		int Completions = 0;
		ASSERT_TRUE(Host->OpenLevel({.Path = Path, .OnComplete = [&](const FMapChangeProgress&) {
			++Completions;
			if (bStandardException) throw std::runtime_error("Completion failure");
			throw 42;
		}}));
		Host->CancelPendingMapChange();
		EXPECT_NO_THROW(FMapChangeTestAccess::Tick(*Host));
		EXPECT_EQ(Completions, 1);
		EXPECT_FALSE(Host->IsPreparingMapChange());
		EXPECT_EQ(Host->GetMapChangeProgress()->Phase, EMapChangePhase::Canceled);
		EXPECT_FALSE(Host->GetMapChangeProgress()->Diagnostic.empty());
	}
	Host->PreExit();
	auto* Retired = Host.Get(); Host.Reset(); MarkObjectHierarchyAsGarbage(Retired); CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FWorldTests, MapChangeCommitsWithoutAViewportOrWaitingForComponentPSOs)
{
	using namespace Durin;
	InitializeDObjectSystem();
	const auto Root = Testing::GetTestWorkDirectory() / "MapChangeWithoutView";
	std::filesystem::create_directories(Root);
	const std::array MountDefinitions{FMountPoint{.VirtualRoot = "/MapChangeWithoutView/", .Owner = EMountOwner::Test,
		.Root = Root}};
	Testing::FScopedMountRegistryFixture Mounts(MountDefinitions);
	ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
	FPackagePath PackagePath;
	ASSERT_TRUE(FPackagePath::TryCreate("/MapChangeWithoutView/Candidate", PackagePath));
	DLevel* Level = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(PackagePath, Level));
	FPreviewLifecycleRenderer Renderer;
	TStrongObjectPtr<DEngine> Host(NewObject<DEngine>(nullptr, "MapWithoutViewHost"));
	FMapChangeTestAccess::SetRenderer(*Host, Renderer);
	auto* PreviousEngine = std::exchange(GEngine, Host.Get());
	struct FRestoreEngine { DEngine* Previous; ~FRestoreEngine() { GEngine = Previous; } } Restore{PreviousEngine};
	auto* Previous = CreateWorld(Host.Get(), EWorldType::Editor);
	Host->SetWorld(Previous);
	int Completions = 0;
	auto* Actor = Level->SpawnActor<AStaticMeshActor>("DeferredPSOActor");
	ASSERT_NE(Actor, nullptr);
	auto* Component = Actor->GetStaticMeshComponent();
	auto* Mesh = NewObject<DStaticMesh>(Level, "UnpreparedMesh");
	Component->SetStaticMesh(Mesh);
	const auto Request = FMaterialPSOPrecacheRequestID::Rejected("Required pipeline failed");
	Component->SetMaterialPSOPrecacheData({Request});
	ASSERT_TRUE(Host->OpenLevel({.Path = Testing::MakePackageLeafAssetObjectPathForTests(PackagePath),
		.BeforeCommit = [&](DLevel&) {
			EXPECT_TRUE(Component->IsRegistered());
			EXPECT_FALSE(Component->IsPSOPrecacheReadyForProxyCreation());
			EXPECT_EQ(Host->GetWorld(), Previous);
			EXPECT_EQ(Completions, 0);
			EXPECT_EQ(Renderer.RenderCalls, 0);
			return true;
		},
		.OnComplete = [&](const FMapChangeProgress& Progress) {
			++Completions;
			EXPECT_EQ(Progress.Phase, EMapChangePhase::Committed);
		}}));
	for (int Frame = 0; Frame < 12 && Host->GetMapChangeProgress()->Phase != EMapChangePhase::RegisteringWorld; ++Frame)
	{
		FMapChangeTestAccess::Tick(*Host);
		ProcessAsyncLoading();
	}
	ASSERT_EQ(Host->GetMapChangeProgress()->Phase, EMapChangePhase::RegisteringWorld);
	for (int Frame = 0; Frame < 12 && Completions == 0; ++Frame)
	{
		FMapChangeTestAccess::Tick(*Host);
		ProcessAsyncLoading();
	}
	EXPECT_EQ(Completions, 1);
	EXPECT_FALSE(Host->IsPreparingMapChange());
	EXPECT_NE(Host->GetWorld(), Previous);
	EXPECT_EQ(Host->GetWorld()->GetCurrentLevel(), Level);
	EXPECT_EQ(Mesh->GetRenderData(), nullptr); // Resource readiness does not gate commit.
	EXPECT_FALSE(Component->IsPSOPrecacheReadyForProxyCreation());
	EXPECT_EQ(Request.GetDiagnostic(), "Required pipeline failed");
	EXPECT_EQ(Renderer.RenderCalls, 0);
	Host->PreExit();
	auto* Retired = Host.Get(); Host.Reset(); MarkObjectHierarchyAsGarbage(Retired); CollectGarbage(Durin::EObjectFlags::Standalone);
	const auto Unloaded = Durin::Testing::EnsurePackageNotResident(PackagePath, EAssetPackageUnloadPolicy::DiscardUnsaved);
	EXPECT_TRUE(Unloaded) << Unloaded.Message;
}

TEST(FWorldTests, PrimaryMapPreloadTransfersRetentionToTheWorld)
{
	using namespace Durin;
	InitializeDObjectSystem();
	const auto Root = Testing::CreateTestFixtureDirectory("PrimaryMapPreload");
	const std::array Definitions{FMountPoint{.VirtualRoot = "/PrimaryMapPreload/", .Owner = EMountOwner::Test, .Root = Root, .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Mounts(Definitions);
	ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
	FPackagePath PackagePath;
	ASSERT_TRUE(FPackagePath::TryCreate("/PrimaryMapPreload/Candidate", PackagePath));
	DLevel* Level = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(PackagePath, Level));
	const auto Saved = SavePackage(Level->GetPackage());
	ASSERT_TRUE(Saved) << Saved.Message;
	ASSERT_TRUE(RefreshAssetRegistry());
	TStrongObjectPtr<DAssetManager> Manager(NewObject<DAssetManager>(nullptr, "PrimaryMapManager"));
	ASSERT_TRUE(Manager->StartInitialLoading());
	const auto Primary = Level->GetPrimaryAssetId();
	auto Managed = Manager->LoadPrimaryAsset(Primary);
	const auto LoadDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!Managed->IsComplete() && std::chrono::steady_clock::now() < LoadDeadline)
	{
		ProcessAsyncLoading(10, 100);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_TRUE(Managed->HasLoadCompleted()); ASSERT_FALSE(Managed->HasError()); Managed.reset();
	auto Preload = Manager->PreloadPrimaryAssets(std::span(&Primary, 1));
	ASSERT_NE(Preload, nullptr);
	FPreviewLifecycleRenderer Renderer;
	TStrongObjectPtr<DEngine> Host(NewObject<DEngine>(nullptr, "PrimaryMapHost"));
	FMapChangeTestAccess::SetRenderer(*Host, Renderer);
	auto* PreviousEngine = std::exchange(GEngine, Host.Get());
	struct FRestore { DEngine* Previous; ~FRestore() { GEngine = Previous; } } Restore{PreviousEngine};
	Host->SetWorld(CreateWorld(Host.Get(), EWorldType::Editor));
	int Completions = 0;
	ASSERT_TRUE(Host->OpenLevel({.Path = Testing::MakePackageLeafAssetObjectPathForTests(PackagePath), .Preload = Preload,
		.OnComplete = [&](const FMapChangeProgress& Progress) {
			++Completions;
			EXPECT_EQ(Progress.Phase, EMapChangePhase::Committed);
			EXPECT_EQ(Manager->GetPrimaryAssetObject(Primary), Level);
		}}));
	Preload.reset();
	FMapChangeTestAccess::Tick(*Host);
	EXPECT_EQ(Host->GetMapChangeProgress()->Phase, EMapChangePhase::LoadingAssets);
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (Completions == 0 && std::chrono::steady_clock::now() < Deadline)
	{
		ProcessAsyncLoading(10, 100); FMapChangeTestAccess::Tick(*Host);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_EQ(Completions, 1);
	EXPECT_EQ(Manager->GetPrimaryAssetObject(Primary), Level);
	ASSERT_TRUE(Manager->UnloadPrimaryAsset(Primary));
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_NE(Host->GetWorld(), nullptr);
	EXPECT_EQ(Host->GetWorld()->GetCurrentLevel(), Level);
	Host->PreExit(); Manager->Shutdown();
	auto* Retired = Host.Get(); Host.Reset(); MarkObjectHierarchyAsGarbage(Retired); CollectGarbage(Durin::EObjectFlags::Standalone);
	if (FindResidentPackage(PackagePath)) EXPECT_TRUE(UnloadPackage(PackagePath));
}

TEST(FWorldTests, MapChangeInitializationAndCommitFailuresPreserveTheCurrentWorld)
{
	using namespace Durin;
	InitializeDObjectSystem();
	const auto Root = Testing::GetTestWorkDirectory() / "MapChangeFailures";
	std::filesystem::create_directories(Root);
	const std::array MountDefinitions{FMountPoint{.VirtualRoot = "/MapChangeFailures/", .Owner = EMountOwner::Test,
		.Root = Root}};
	Testing::FScopedMountRegistryFixture Mounts(MountDefinitions);
	ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
	FPreviewLifecycleRenderer Renderer;
	for (int FailureMode : {0, 1, 2, 3, 4})
	{
		SCOPED_TRACE(FailureMode);
		FPackagePath PackagePath;
		ASSERT_TRUE(FPackagePath::TryCreate(std::format("/MapChangeFailures/Candidate{}", FailureMode), PackagePath));
		DLevel* Level = nullptr;
		ASSERT_TRUE(CreatePackageLeafAssetForTesting(PackagePath, Level));
		TStrongObjectPtr<DEngine> Host(NewObject<DEngine>(nullptr, "MapBuilderFailureHost"));
		FMapChangeTestAccess::SetRenderer(*Host, Renderer);
		auto* Previous = CreateWorld(Host.Get(), EWorldType::Editor);
		Host->SetWorld(Previous);
		std::optional<FWorldSubsystemRegistration> ThrowingService;
		if (FailureMode == 4)
		{
			const std::array Classes{FThrowingWorldSubsystem::StaticClass()};
			DObjectForceRegistration(Classes.front());
			ASSERT_TRUE(Private::CreateClassDefaultObjectsForBatch(Classes));
			ThrowingService.emplace(FSubsystemDescriptor{.Type = Classes.front()});
		}
		int Completions = 0;
		ASSERT_TRUE(Host->OpenLevel({.Path = Testing::MakePackageLeafAssetObjectPathForTests(PackagePath),
			.BeforeCommit = [FailureMode, &Host](DLevel&) {
				if (FailureMode == 0) throw std::runtime_error("Commit veto failure");
				if (FailureMode == 1) throw 42;
				if (FailureMode == 3) Host->CancelPendingMapChange();
				return false;
			},
			.OnComplete = [&](const FMapChangeProgress& Progress) {
				++Completions;
				EXPECT_EQ(Progress.Phase, FailureMode == 3 ? EMapChangePhase::Canceled : EMapChangePhase::Failed);
			}}));
		for (int Frame = 0; Frame < 12 && Host->IsPreparingMapChange(); ++Frame)
		{
			EXPECT_NO_THROW(FMapChangeTestAccess::Tick(*Host));
			ProcessAsyncLoading();
		}
		EXPECT_EQ(Completions, 1);
		EXPECT_FALSE(Host->IsPreparingMapChange());
		const auto Progress = Host->GetMapChangeProgress();
		ASSERT_TRUE(Progress);
		EXPECT_EQ(Progress->Phase, FailureMode == 3 ? EMapChangePhase::Canceled : EMapChangePhase::Failed);
		if (FailureMode < 3) EXPECT_NE(Progress->Diagnostic.find("Ready to commit"), std::string::npos) << Progress->Diagnostic;
		const auto Expected = FailureMode == 0 ? "Commit veto failure" : FailureMode == 1 ? "Unknown native exception"
			: FailureMode == 2 ? "Map activation was rejected" : FailureMode == 3 ? "Map change canceled" : "World callback exception";
		EXPECT_NE(Progress->Diagnostic.find(Expected), std::string::npos) << Progress->Diagnostic;
		EXPECT_EQ(Host->GetWorld(), Previous);
		EXPECT_FALSE(Previous->IsShutdownRequested());
		EXPECT_EQ(Level->GetWorld(), nullptr);
		Host->PreExit();
		auto* Retired = Host.Get(); Host.Reset(); MarkObjectHierarchyAsGarbage(Retired); CollectGarbage(Durin::EObjectFlags::Standalone);
		const auto Unloaded = Durin::Testing::EnsurePackageNotResident(PackagePath, EAssetPackageUnloadPolicy::DiscardUnsaved);
		EXPECT_TRUE(Unloaded) << Unloaded.Message;
	}
}

TEST(FWorldTests, ResourceQueriesDoNotStartPreparationAndTextureFailureStaysOnOwner)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DStaticMesh> Mesh(NewObject<DStaticMesh>(nullptr, "UnpreparedMesh"));
	EXPECT_EQ(Mesh->GetRenderDataLoadStatus().CpuPhase, ECookedMeshCpuPhase::Unloaded);
	TStrongObjectPtr<DTexture2D> Texture(NewObject<DTexture2D>(nullptr, "UnpreparedTexture"));
	EXPECT_EQ(Texture->GetResource(), nullptr);
	EXPECT_FALSE(Texture->HasPendingRenderResourceInitialization());
	Texture->UpdateResource();
	EXPECT_EQ(Texture->GetResource(), nullptr);
	auto* RetiredMesh = Mesh.Get(); auto* RetiredTexture = Texture.Get();
	Mesh.Reset(); Texture.Reset(); MarkObjectHierarchyAsGarbage(RetiredMesh); MarkObjectHierarchyAsGarbage(RetiredTexture); CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FWorldTests, ComponentProxyGatePreservesRequiredFailureAndHonorsProxyStrategy)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FPreviewLifecycleRenderer Renderer;
	FPreviewLifecycleEngine Host(Renderer);
	auto* PreviousEngine = std::exchange(GEngine, &Host);
	struct FRestoreEngine { DEngine* Previous; ~FRestoreEngine() { GEngine = Previous; } } Restore{PreviousEngine};
	auto* World = CreateWorld(&Host, EWorldType::Editor);
	Host.SetWorld(World);
	auto* Actor = World->SpawnActor<AStaticMeshActor>("PSOReadinessActor");
	auto* Component = Actor->GetStaticMeshComponent();
	Component->SetMaterialPSOPrecacheData({FMaterialPSOPrecacheRequestID::Rejected("Required pipeline failed")});
	EXPECT_FALSE(Component->IsPSOPrecacheReadyForProxyCreation());
	EXPECT_EQ(Component->GetPSOPrecacheComponentData().GetMaterialPSOPrecacheRequestIDs()[0].GetDiagnostic(), "Required pipeline failed");
	Component->SetPSOPrecacheProxyCreationStrategy(EPSOPrecacheProxyCreationStrategy::AlwaysCreate);
	EXPECT_TRUE(Component->IsPSOPrecacheReadyForProxyCreation());
	Host.PreExit(); Host.SetWorld(nullptr); MarkObjectHierarchyAsGarbage(World); CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FWorldTests, AutomaticComponentPSOPolicyUsesHostInsteadOfWorldOrBuildCapabilities)
{
	using namespace Durin;
	Tests::FScopedComponentPSOPrecacheMode Mode(EComponentPSOPrecacheMode::Automatic);
	InitializeDObjectSystem();
	FPreviewLifecycleRenderer Renderer;
	FPreviewLifecycleEngine GameHost(Renderer);
	class FEditorHost final : public FPreviewLifecycleEngine
	{
	public:
		using FPreviewLifecycleEngine::FPreviewLifecycleEngine;
		auto GetInitialWorldType() const -> EWorldType override { return EWorldType::Editor; }
	} EditorHost(Renderer);
	auto* PreviousEngine = std::exchange(GEngine, nullptr);
	struct FRestoreEngine { DEngine* Previous; ~FRestoreEngine() { GEngine = Previous; } } Restore{PreviousEngine};
	EXPECT_EQ(IsComponentPSOPrecachingEnabled(), !DURIN_WITH_EDITOR);
	GEngine = &GameHost;
	EXPECT_TRUE(IsComponentPSOPrecachingEnabled());
	GEngine = &EditorHost;
	for (const auto Type : {EWorldType::Editor, EWorldType::EditorPreview, EWorldType::PIE, EWorldType::Game})
	{
		auto* World = CreateWorld(&EditorHost, Type);
		EditorHost.SetWorld(World);
		EXPECT_FALSE(IsComponentPSOPrecachingEnabled());
		auto* Actor = World->SpawnActor<AStaticMeshActor>();
		auto* Component = Actor->GetStaticMeshComponent();
		Component->PostLoad();
		Component->PrecachePSOs();
		EXPECT_TRUE(Component->IsPSOPrecacheReadyForProxyCreation());
		EXPECT_FALSE(Component->IsPSOPrecaching());
		EXPECT_FALSE(Component->GetPSOPrecacheComponentData().IsPSOPrecacheCalled());
		SetComponentPSOPrecacheMode(EComponentPSOPrecacheMode::Enabled);
		EXPECT_TRUE(IsComponentPSOPrecachingEnabled());
		SetComponentPSOPrecacheMode(EComponentPSOPrecacheMode::Automatic);
		EditorHost.SetWorld(nullptr);
		MarkObjectHierarchyAsGarbage(World); CollectGarbage(Durin::EObjectFlags::Standalone);
	}
	GEngine = &GameHost;
	SetComponentPSOPrecacheMode(EComponentPSOPrecacheMode::Disabled);
	EXPECT_FALSE(IsComponentPSOPrecachingEnabled());
}

TEST(FWorldTests, DisabledComponentPSOPolicyReleasesFailedObserversWithoutCancelingSharedWork)
{
	using namespace Durin;
	Tests::FScopedComponentPSOPrecacheMode Mode(EComponentPSOPrecacheMode::Disabled);
	InitializeDObjectSystem();
	TStrongObjectPtr<DStaticMeshComponent> Component(NewObject<DStaticMeshComponent>(nullptr, NAME_None));
	auto Request = FMaterialPSOPrecacheRequestID::Rejected("Required pipeline failed");
	Component->SetMaterialPSOPrecacheData({Request});
	EXPECT_TRUE(Component->IsPSOPrecacheReadyForProxyCreation());
	EXPECT_TRUE(Component->GetPSOPrecacheComponentData().GetMaterialPSOPrecacheRequestIDs().empty());
	EXPECT_FALSE(Component->CheckPSOPrecachingAndBoostPriority());
	EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Failed);
	EXPECT_EQ(Request.GetDiagnostic(), "Required pipeline failed");
	auto* Retired = Component.Get(); Component.Reset(); MarkObjectHierarchyAsGarbage(Retired); CollectGarbage(Durin::EObjectFlags::Standalone);
}
