#include "WorldTestSupport.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "PropertyEditor/PropertyEditing.h"
#include "Preview/PreviewScene.h"
#include "IRendererModule.h"
#include "Rendering/LightSceneProxy.h"
#include "Rendering/SplineMeshSceneProxy.h"
#include "Rendering/SkyBoxSceneProxy.h"
#include "Rendering/VolumetricCloudSceneProxy.h"

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
	Durin::CollectGarbage();
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
			Durin::Editor::FScopedTransaction Transaction(Transactions.Get(), {"test", "Reparent component"});
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
		ASSERT_TRUE(Transactions->Undo()) << Durin::Editor::FormatTransactorResult(Transactions->GetLastResult());
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
		ASSERT_TRUE(Transactions->Undo()) << Durin::Editor::FormatTransactorResult(Transactions->GetLastResult());
		ExpectAttachment(First, Second, 42.0);
		ASSERT_TRUE(Transactions->Reset());
		World->SetRenderScene(nullptr);
		Durin::MarkObjectHierarchyAsGarbage(World);
		Durin::CollectGarbage();
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
			Durin::Editor::FScopedTransaction Transaction(Transactions.Get(), {"test", "Detach component"});
			ASSERT_TRUE(Child->HasAnyObjectFlags(Durin::EObjectFlags::Transactional)) << "child=" << int(Child->GetObjectFlags()) << " outer=" << int(Child->GetOuter()->GetObjectFlags()) << " world=" << int(World->GetWorldType());
			ASSERT_TRUE(Child->Modify());
			ASSERT_TRUE(Child->DetachFromComponent(Rule));
		}
		const auto DetachedLocation = Child->GetWorldLocation();
		for (int32 Repeat = 0; Repeat < 2; ++Repeat)
		{
			ASSERT_TRUE(Transactions->Undo()) << Durin::Editor::FormatTransactorResult(Transactions->GetLastResult());
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
		ASSERT_TRUE(Transactions->Reset());
		Durin::MarkObjectHierarchyAsGarbage(World);
		Durin::CollectGarbage();
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
			Durin::Editor::FScopedTransaction Transaction(Transactions.Get(), {"test", "Reverse hierarchy"});
			(void)(bParentFirst ? Second : First)->Modify();
			(void)(bParentFirst ? First : Second)->Modify();
			ASSERT_TRUE(First->DetachFromComponent(Durin::EDetachmentTransformRule::KeepWorld));
			ASSERT_TRUE(Second->AttachToComponent(First, Durin::EAttachmentTransformRule::KeepWorld));
		}
		ASSERT_TRUE(Transactions->Undo()) << Durin::Editor::FormatTransactorResult(Transactions->GetLastResult());
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
		ASSERT_TRUE(Transactions->Reset());
		Durin::MarkObjectHierarchyAsGarbage(World);
		Durin::CollectGarbage();
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
		Durin::Editor::FScopedTransaction Transaction(Transactions.Get(), {"test", "Reparent unregistered component"});
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
	ASSERT_TRUE(Transactions->Undo()) << Durin::Editor::FormatTransactorResult(Transactions->GetLastResult());
	ExpectWorld(BeforeWorld);
	ASSERT_TRUE(Transactions->Redo());
	ExpectWorld(AfterWorld);
	ASSERT_TRUE(Transactions->Reset());
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
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
	Durin::CollectGarbage();
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
			Durin::Editor::FScopedTransaction Transaction(Transactions.Get(), {"test", "Toggle actor visibility"});
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
		ASSERT_TRUE(Transactions->Undo()) << Durin::Editor::FormatTransactorResult(Transactions->GetLastResult());
		ExpectVisibility(bInitiallyHidden);
		ASSERT_TRUE(Transactions->Redo());
		ExpectVisibility(!bInitiallyHidden);
		ASSERT_TRUE(Transactions->Undo()) << Durin::Editor::FormatTransactorResult(Transactions->GetLastResult());
		ExpectVisibility(bInitiallyHidden);
		ASSERT_TRUE(Transactions->Reset());
		World->SetRenderScene(nullptr);
		Durin::MarkObjectHierarchyAsGarbage(World);
		Durin::CollectGarbage();
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
	Durin::Editor::FPropertyEditSession Session;
	const auto Target = Durin::Editor::FPropertyEditTarget::ForMember(Light, Hidden);
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
	ASSERT_TRUE(Transactions->Undo()) << Durin::Editor::FormatTransactorResult(Transactions->GetLastResult());
	EXPECT_TRUE(Scene.Lights.contains(Light->GetLightComponent()));
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_TRUE(Scene.Lights.empty());
	ASSERT_TRUE(Transactions->Reset());
	World->SetRenderScene(nullptr);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
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

TEST(FWorldTests, ActorModifyRecordsTheNativeRootTransform)
{
	using namespace Durin; using namespace Durin::Editor;
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
	ASSERT_TRUE(Transactions->Reset()); MarkObjectHierarchyAsGarbage(World); CollectGarbage();
}
