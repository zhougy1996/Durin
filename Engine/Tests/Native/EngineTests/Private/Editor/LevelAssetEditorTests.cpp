#include "CoreMinimal.h"
#include "AssetEditorTestQueries.h"
#include "LevelEditorModule.h"
#include "EditorModeManager.h"
#include "FileHelpers.h"
#include "Misc/FeedbackContext.h"
#include "ContentBrowserAssetDataSource.h"
#include "ContentBrowserAssetDataPayload.h"

#include "LevelEditorSubsystem.h"
#include "EditorEngineTestSupport.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditor.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/ToolkitManager.h"
#include "FLevelEditorToolkit.h"
#include "AssetDefinition/AssetDefinitionRegistry.h"
#include "AssetDefinition/AssetDefinition.h"
#include "NativeDObjectTestSupport.h"
#include "DObject/Class.h"
#include "Engine/World.h"
#include "Engine/WorldSubsystem.h"
#include "Engine/Actor.h"
#include "Components/SceneComponent.h"
#include "IRendererModule.h"
#include "Rendering/SceneInterface.h"
#include "Rendering/SplineMeshSceneProxy.h"
#include "Asset/StreamableHandle.h"
#include "Threading/TaskRuntime.h"
#include "Engine/Level.h"
#include "Engine/MapChange.h"
#include "Asset/PackageSerialization.h"
#include "DObject/Package.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include <gtest/gtest.h>

namespace Durin
{
	struct FMapChangeTestAccess
	{
		static auto SetRenderer(DEngine& Engine, IRendererModule& Renderer) -> void { Engine.RendererModule = &Renderer; }
	};

	class FMapLifecycleSubsystem : public DWorldSubsystem
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FMapLifecycleSubsystem, DWorldSubsystem, FMapLifecycleSubsystem::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FMapLifecycleSubsystem)
	public:
		explicit FMapLifecycleSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}
		inline static int Deinitializations = 0;
		auto Deinitialize() noexcept -> void override { ++Deinitializations; Super::Deinitialize(); }
	};
	auto FMapLifecycleSubsystem::GetPrivateStaticClass() -> DClass*
	{
		static DClass* Class = nullptr;
		if (!Class)
			GetPrivateStaticClassBody("", "FMapLifecycleSubsystem", Class, nullptr, sizeof(FMapLifecycleSubsystem), alignof(FMapLifecycleSubsystem),
				EClassFlags::None, InternalConstructor<FMapLifecycleSubsystem>, &DWorldSubsystem::StaticClass);
		return Class;
	}

	// Headless presentation adapter records ownership without allocating an RHI viewport.
	class FLevelEditorTestEngine : public DEditorEngine
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FLevelEditorTestEngine, DEditorEngine, FLevelEditorTestEngine::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FLevelEditorTestEngine)
	public:
		explicit FLevelEditorTestEngine(const FObjectInitializer& Initializer) : Super(Initializer) {}
		auto SetMainSceneViewport(std::shared_ptr<FSceneViewport> Viewport) -> void override
		{ FEditorEngineTestAccess::SetStartupViewport(*this, std::move(Viewport)); }
	};
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FLevelEditorTestEngine)
}

namespace
{
	using namespace Durin;
	class FReloadTestScene final : public FSceneInterface
	{
	public:
		auto UpdateSkyLighting() -> void override {}
		auto AddPrimitive(DPrimitiveComponent* Primitive) -> void override {}
		auto RemovePrimitive(DPrimitiveComponent* Primitive) -> void override {}
		auto AddLight(DLightComponent* Light) -> void override {}
		auto RemoveLight(DLightComponent* Light) -> void override {}
		auto AddProceduralSky(DProceduralSkyComponent* Sky) -> void override {}
		auto RemoveProceduralSky(DProceduralSkyComponent* Sky) -> void override {}
		auto AddSkyLight(DSkyLightComponent* SkyLight) -> void override {}
		auto RemoveSkyLight(DSkyLightComponent* SkyLight) -> void override {}
		auto AddVolumetricCloud( DVolumetricCloudComponent* Cloud ) -> void override {}
		auto RemoveVolumetricCloud( DVolumetricCloudComponent* Cloud ) -> void override {}
		auto Release() -> void override {}
		auto UpdatePrimitiveTransform( FPrimitiveComponentId PrimitiveId, const FMatrix& Transform ) -> void override {}
		auto UpdatePrimitiveVisibility( FPrimitiveComponentId PrimitiveId, bool bVisible ) -> void override {}
		auto UpdatePrimitiveMaterialBinding( FPrimitiveComponentId PrimitiveId, const FMaterialRenderProxyBindingUpdate& Update ) -> void override {}
		auto UpdateMaterialParameterCollection( std::shared_ptr<const FMaterialParameterCollectionSnapshot> Snapshot) -> void override {}
		auto UpdatePrimitiveCustomData(FPrimitiveComponentId Id, FPrimitiveCustomData Data) -> void override {}
		auto UpdateStaticMeshRenderInstances(FPrimitiveComponentId Id, FStaticMeshRenderInstances Instances) -> void override {}
		auto RemoveMaterialParameterCollection(FGuid CollectionId) -> void override {}
		auto UpdateSplineMeshDynamicData( FPrimitiveComponentId PrimitiveId, FSplineMeshRenderDynamicData DynamicData ) -> void override {}
	};
	class FReloadTestRenderer final : public IRendererModule
	{
	public:
		bool FailNextScene = false;
		auto CreateScene() -> FScenePtr override { if (std::exchange(FailNextScene, false)) return {}; return {new FReloadTestScene, FSceneDeleter([](FSceneInterface* Scene) { delete Scene; })}; }
		auto CreateViewState() -> FSceneViewStateOwner override { return {}; }
		auto InvalidateViewState(FSceneViewStateId) -> void override {}
		auto InvalidateAllViewStates() -> void override {}
		auto RenderView(FRHICommandListImmediate&, const FSceneViewRenderRequest&) -> ERenderViewResult override { return ERenderViewResult::Success; }
	};

	class FLevelMapLifecycleTests : public testing::Test
	{
	protected:
		bool StartedScheduler = false;
		std::unique_ptr<Testing::FScopedMountRegistryFixture> Mounts;
		std::filesystem::path Root;
		std::optional<FWorldSubsystemRegistration> LifecycleRegistration;
		FReloadTestRenderer Renderer;
		FLevelEditorModule* Module = nullptr;
		TStrongObjectPtr<DEditorEngine> Host;
		DLevel* Level = nullptr;
		AActor* Actor = nullptr;
		DSceneComponent* Component = nullptr;
		DWorld* World = nullptr;
		DAssetEditorSubsystem* Editors = nullptr;
		std::shared_ptr<FAssetEditorToolkit> Toolkit;
		auto CurrentResource() const -> std::string
		{
			const auto Assets = Toolkit->GetEditingObjects();
			return Assets.empty() ? std::string{} : Assets.front()->GetObjectPath();
		}
		TWeakObjectPtr<DWorld> OriginalWorld;
		TWeakObjectPtr<DLevel> PreviousLevel;
		TWeakObjectPtr<AActor> PreviousActor;
		TWeakObjectPtr<DSceneComponent> PreviousComponent;
		FAssetOpenTarget Target;
		std::vector<FAssetOpenTarget> Targets;
		const DAssetDefinition* Definition = nullptr;
		int Prompts = 0;
		std::string Diagnostic;
		FFeedbackContext Feedback{{}, [this](ELogLevel Level, std::string_view Message) { if (Level >= ELogLevel::Error) Diagnostic += Message; }};
		FScopedFeedbackContext FeedbackScope{Feedback};
		auto SetUp() -> void override
		{
			StartedScheduler = !IsTaskSchedulerRunning();
			if (StartedScheduler) ASSERT_TRUE(InitializeTaskScheduler(2));
			Testing::InitializeAssetSystemForTests();
			Mounts = std::make_unique<Testing::FScopedMountRegistryFixture>();
			Root = Testing::CreateTestFixtureDirectory("LevelDiskReload");
			Testing::RegisterMountPointForTests("/LevelDiskReload/", Root.generic_string() + "/");
			const std::array Classes{FMapLifecycleSubsystem::StaticClass()};
			DObjectForceRegistration(Classes.front());
			ASSERT_TRUE(Private::CreateClassDefaultObjectsForBatch(Classes));
			LifecycleRegistration.emplace(FSubsystemDescriptor{.Type = Classes.front()});
			Module = &FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
			Host = NewObject<FLevelEditorTestEngine>(nullptr, {});
			FEditorEngineTestAccess::StartEditor(*Host);
			FMapChangeTestAccess::SetRenderer(*Host, Renderer);
			FPackagePath PackagePath;
			ASSERT_TRUE(FPackagePath::TryCreate("/LevelDiskReload/Map", PackagePath));
			Level = nullptr;
			ASSERT_TRUE(CreatePackageLeafAssetForTesting(PackagePath, Level));
			Actor = Level->SpawnActor<AActor>("SavedActor");
			ASSERT_NE(Actor, nullptr);
			Component = Cast<DSceneComponent>(Actor->AddInstanceComponent(DSceneComponent::StaticClass(), "Root"));
			ASSERT_NE(Component, nullptr);
			ASSERT_TRUE(Actor->SetRootComponent(Component));
			ASSERT_TRUE(Actor->SetActorLabel("On disk"));
			ASSERT_TRUE(SavePackage(Level->GetPackage()));
			World = NewObject<DWorld>(Host.Get(), "OriginalWorld");
			World->SetWorldType(EWorldType::Editor);
			World->InitializeSubsystems();
			ASSERT_TRUE(World->SetCurrentLevel(Level));
			ASSERT_TRUE(Component->IsRegistered());
			FEditorEngineTestAccess::SetEditorWorld(*Host, World);
			OriginalWorld = World;
			Editors = Host->GetEditorSubsystem<DAssetEditorSubsystem>();
			ASSERT_TRUE(Module->InitializeLevelEditor(*Editors, {}, [] {}));
			Toolkit = Module->GetOrCreateLevelEditor(*Editors)->GetToolkit();
			ASSERT_TRUE(Toolkit);
			EXPECT_EQ(Toolkit->GetEditingObjects(), std::vector<DObject*>{Level});
			ASSERT_TRUE(Actor->SetActorLabel("Unsaved edit"));
			Level->GetPackage()->MarkDirty();
			PreviousLevel = Level;
			PreviousActor = Actor;
			PreviousComponent = Component;
			FSoftObjectPath Path;
			ASSERT_TRUE(FSoftObjectPath::TryCreate(Level->GetObjectPath(), Path));
			Target = {Path, DLevel::StaticClass()->GetQualifiedName().ToString()};
			Prompts = 0;
			FEditorFileUtils::SetSavePrompt([&](auto, const auto&) { ++Prompts; return FEditorFileUtils::PR_Declined; });
			Definition = FAssetDefinitionRegistry::Get().GetAssetDefinitionForClass(Target.AssetClassName);
			ASSERT_NE(Definition, nullptr);
			Targets = Definition->PrepareToActivateAssets({.AssetEditors = Editors, .Assets = {Target}});
			ASSERT_EQ(Targets.size(), 1u);
		}
		auto TearDown() -> void override
		{
			FEditorFileUtils::SetSavePrompt({});
			Toolkit.reset();
			if (Host)
			{
				if (Module && Editors) Module->ReleaseLevelEditor(*Editors);
				Host->PreExit();
				MarkObjectHierarchyAsGarbage(Host.Get());
				Host.Reset();
			}
			LifecycleRegistration.reset();
			CollectGarbage(EObjectFlags::Standalone);
			Mounts.reset();
			if (StartedScheduler) ShutdownTaskScheduler();
		}
	};

	TEST_F(FLevelMapLifecycleTests, CancelSavePromptPreservesCurrentMap)
	{
		FEditorFileUtils::SetSavePrompt([](auto, const auto&) { return FEditorFileUtils::PR_Cancelled; });
		EXPECT_TRUE(Definition->PrepareToActivateAssets({.AssetEditors = Editors, .Assets = {Target}}).empty());
		EXPECT_EQ(World->GetCurrentLevel(), Level);
		EXPECT_EQ(Actor->GetActorLabel(), "Unsaved edit");
		EXPECT_TRUE(OriginalWorld.IsValid());
	}

	TEST_F(FLevelMapLifecycleTests, SaveThenReloadReadsSavedEdits)
	{
		FEditorFileUtils::SetSavePrompt([](auto, const auto&) { return FEditorFileUtils::PR_Success; });
		const auto SavedTargets = Definition->PrepareToActivateAssets({.AssetEditors = Editors, .Assets = {Target}});
		ASSERT_EQ(SavedTargets.size(), 1u);
		ASSERT_TRUE(Editors->OpenAssets({.AssetEditors = Editors, .Assets = SavedTargets}));
		EXPECT_TRUE(Diagnostic.empty()) << Diagnostic;
		EXPECT_FALSE(PreviousLevel.IsValid());
		auto* Reloaded = Host->GetEditorWorld()->GetCurrentLevel();
		ASSERT_NE(Reloaded->FindActorByName("SavedActor"), nullptr);
		EXPECT_EQ(Reloaded->FindActorByName("SavedActor")->GetActorLabel(), "Unsaved edit");
		EXPECT_FALSE(Reloaded->GetPackage()->IsDirty());
	}

	TEST_F(FLevelMapLifecycleTests, ScriptLoadRejectsUnfinishedTransactionWithoutDestroyingWorld)
	{
		const auto Index = Host->BeginTransaction("Unfinished edit");
		ASSERT_GE(Index, 0);
		EXPECT_FALSE(Host->GetEditorSubsystem<DLevelEditorSubsystem>()->LoadLevel(Target.AssetPath.ToString()));
		EXPECT_FALSE(Diagnostic.empty());
		EXPECT_TRUE(OriginalWorld.IsValid());
		EXPECT_EQ(World->GetCurrentLevel(), Level);
		EXPECT_TRUE(Host->GetTransactor()->IsActive());
		Host->CancelTransaction(Index);
	}

	TEST_F(FLevelMapLifecycleTests, ReferencedMapRejectsUnloadAndRecoversBlankMap)
	{
		{
			TStrongObjectPtr<DLevel> ExternalReference(Level);
			ASSERT_TRUE(Editors->OpenAssets({.AssetEditors = Editors, .Assets = Targets}));
			EXPECT_FALSE(Diagnostic.empty());
			EXPECT_NE(Diagnostic.find("remains referenced"), std::string::npos);
			ASSERT_NE(Host->GetEditorWorld(), nullptr);
			EXPECT_FALSE(OriginalWorld.IsValid());
			ASSERT_NE(Host->GetEditorWorld()->GetCurrentLevel(), nullptr);
			EXPECT_EQ(Host->GetEditorWorld()->GetCurrentLevel()->FindActorByName("SavedActor"), nullptr);
			EXPECT_TRUE(CurrentResource().empty());
		}
	}

	TEST_F(FLevelMapLifecycleTests, RepeatedDiscardReloadsDiskObjects)
	{
		ASSERT_TRUE(Editors->OpenAssets({.AssetEditors = Editors, .Assets = Targets}));
		EXPECT_TRUE(Diagnostic.empty()) << Diagnostic;
		EXPECT_EQ(Prompts, 1);
		EXPECT_FALSE(PreviousLevel.IsValid());
		EXPECT_FALSE(PreviousActor.IsValid());
		EXPECT_FALSE(PreviousComponent.IsValid());
		EXPECT_FALSE(OriginalWorld.IsValid());
		auto* Reloaded = Host->GetEditorWorld()->GetCurrentLevel();
		ASSERT_NE(Reloaded, nullptr);
		ASSERT_NE(Reloaded->FindActorByName("SavedActor"), nullptr);
		EXPECT_EQ(Reloaded->FindActorByName("SavedActor")->GetActorLabel(), "On disk");
		ASSERT_NE(Reloaded->FindActorByName("SavedActor")->GetRootComponent(), nullptr);
		EXPECT_TRUE(Reloaded->FindActorByName("SavedActor")->GetRootComponent()->IsRegistered());
		EXPECT_EQ(Reloaded->FindActorByName("SavedActor")->GetRootComponent()->GetOwner(), Reloaded->FindActorByName("SavedActor"));
		EXPECT_EQ(Reloaded->GetWorld(), Host->GetEditorWorld());
		EXPECT_EQ(Toolkit->GetEditingObjects(), (std::vector<DObject*>{Reloaded}));
		EXPECT_FALSE(Reloaded->GetPackage()->IsDirty());
		const TWeakObjectPtr<DLevel> FirstReload(Reloaded);
		ASSERT_TRUE(Reloaded->FindActorByName("SavedActor")->SetActorLabel("Second unsaved edit"));
		Reloaded->GetPackage()->MarkDirty();
		const auto SecondTargets = Definition->PrepareToActivateAssets({.AssetEditors = Editors, .Assets = {Target}});
		ASSERT_TRUE(Editors->OpenAssets({.AssetEditors = Editors, .Assets = SecondTargets}));
		EXPECT_TRUE(Diagnostic.empty()) << Diagnostic;
		EXPECT_FALSE(FirstReload.IsValid());
		EXPECT_EQ(Prompts, 2);
		ASSERT_NE(Host->GetEditorWorld()->GetCurrentLevel()->FindActorByName("SavedActor"), nullptr);
		EXPECT_EQ(Host->GetEditorWorld()->GetCurrentLevel()->FindActorByName("SavedActor")->GetActorLabel(), "On disk");
	}

	TEST_F(FLevelMapLifecycleTests, SwitchingMapsDiscardsResidentEdits)
	{
		// Switching away also discards the old map's resident graph, so returning reads disk.
		FPackagePath OtherPackagePath;
		ASSERT_TRUE(FPackagePath::TryCreate("/LevelDiskReload/Other", OtherPackagePath));
		DLevel* Other = nullptr;
		ASSERT_TRUE(CreatePackageLeafAssetForTesting(OtherPackagePath, Other));
		ASSERT_NE(Other->SpawnActor<AActor>("OtherActor"), nullptr);
		ASSERT_TRUE(SavePackage(Other->GetPackage()));
		FSoftObjectPath OtherPath;
		ASSERT_TRUE(FSoftObjectPath::TryCreate(Other->GetObjectPath(), OtherPath));
		const TWeakObjectPtr<DLevel> Departing(Host->GetEditorWorld()->GetCurrentLevel());
		ASSERT_TRUE(Departing.Get()->FindActorByName("SavedActor")->SetActorLabel("Discard on switch"));
		Departing.Get()->GetPackage()->MarkDirty();
		const FAssetOpenTarget OtherTarget{OtherPath, Target.AssetClassName};
		const auto OtherTargets = Definition->PrepareToActivateAssets({.AssetEditors = Editors, .Assets = {OtherTarget}});
		ASSERT_TRUE(Editors->OpenAssets({.AssetEditors = Editors, .Assets = OtherTargets}));
		EXPECT_TRUE(Diagnostic.empty()) << Diagnostic;
		EXPECT_FALSE(Departing.IsValid());
		EXPECT_NE(Host->GetEditorWorld()->GetCurrentLevel()->FindActorByName("OtherActor"), nullptr);
		ASSERT_TRUE(Editors->OpenAssets({.AssetEditors = Editors, .Assets = Targets}));
		EXPECT_TRUE(Diagnostic.empty()) << Diagnostic;
		ASSERT_NE(Host->GetEditorWorld()->GetCurrentLevel()->FindActorByName("SavedActor"), nullptr);
		EXPECT_EQ(Host->GetEditorWorld()->GetCurrentLevel()->FindActorByName("SavedActor")->GetActorLabel(), "On disk");
	}

	TEST_F(FLevelMapLifecycleTests, SceneFailureCreatesFreshWorldAndClearsDocument)
	{
		// A failure beyond the destructive boundary must produce a fresh usable blank map.
		const TWeakObjectPtr<DWorld> LastWorld(Host->GetEditorWorld());
		const int Deinitializations = FMapLifecycleSubsystem::Deinitializations;
		Renderer.FailNextScene = true;
		ASSERT_TRUE(Editors->OpenAssets({.AssetEditors = Editors, .Assets = Targets}));
		EXPECT_FALSE(Diagnostic.empty());
		EXPECT_FALSE(LastWorld.IsValid());
		EXPECT_EQ(FMapLifecycleSubsystem::Deinitializations, Deinitializations + 1);
		ASSERT_NE(Host->GetEditorWorld(), nullptr);
		EXPECT_TRUE(Host->GetEditorWorld()->AreSubsystemsInitialized());
		EXPECT_NE(Host->GetEditorWorld()->GetSubsystem<FMapLifecycleSubsystem>(), nullptr);
		auto* Blank = Host->GetEditorWorld()->GetCurrentLevel();
		ASSERT_NE(Blank, nullptr);
		EXPECT_TRUE(Blank->HasAnyObjectFlags(EObjectFlags::Transient));
		EXPECT_NE(Blank->SpawnActor<AActor>("EditableBlankMap"), nullptr);
		EXPECT_TRUE(CurrentResource().empty());
		EXPECT_TRUE(Toolkit->GetEditingObjects().empty());
	}

	TEST_F(FLevelMapLifecycleTests, MissingFileAfterTeardownRecoversAndCanOpenAgain)
	{
		// Disappear the validated file at teardown: exercise the real disk loader failure.
		const auto MapFile = Root / "Map.dasset";
		const auto HeldFile = Root / "Map.dasset.held";
		ASSERT_TRUE(std::filesystem::exists(MapFile));
		const auto FailureHandle = Host->MapChanged.AddLambda([&](DWorld*, EMapChangeType Type) {
			if (Type == EMapChangeType::TearDownWorld) std::filesystem::rename(MapFile, HeldFile);
		});
		ASSERT_TRUE(Editors->OpenAssets({.AssetEditors = Editors, .Assets = Targets}));
		Host->MapChanged.Remove(FailureHandle);
		if (std::filesystem::exists(HeldFile)) std::filesystem::rename(HeldFile, MapFile);
		EXPECT_FALSE(Diagnostic.empty());
		EXPECT_FALSE(Diagnostic.empty());
		ASSERT_NE(Host->GetEditorWorld(), nullptr);
		ASSERT_NE(Host->GetEditorWorld()->GetCurrentLevel(), nullptr);
		EXPECT_TRUE(Host->GetEditorWorld()->GetCurrentLevel()->HasAnyObjectFlags(EObjectFlags::Transient));
		EXPECT_TRUE(CurrentResource().empty());
		// Recovery is a usable state: another open can succeed after the file returns.
		Diagnostic.clear();
		ASSERT_TRUE(Editors->OpenAssets({.AssetEditors = Editors, .Assets = Targets}));
		EXPECT_TRUE(Diagnostic.empty()) << Diagnostic;


	}

	TEST_F(FLevelMapLifecycleTests, PublishedMapSurvivesNotificationExceptions)
	{
		const auto Handle = Host->MapChanged.AddLambda([](DWorld*, EMapChangeType Type) {
			if (Type == EMapChangeType::LoadMap || Type == EMapChangeType::NewMap) throw std::runtime_error("observer failure");
		});
		ASSERT_TRUE(Editors->OpenAssets({.AssetEditors = Editors, .Assets = Targets}));
		EXPECT_TRUE(Diagnostic.empty()) << Diagnostic;
		EXPECT_TRUE(Diagnostic.empty());
		ASSERT_NE(Host->GetEditorWorld(), nullptr);
		EXPECT_EQ(CurrentResource(), Target.AssetPath.ToString());
		EXPECT_NO_THROW(EXPECT_NE(Host->NewMap(), nullptr));
		EXPECT_TRUE(CurrentResource().empty());
		Host->MapChanged.Remove(Handle);

	}

	TEST_F(FLevelMapLifecycleTests, SynchronousMapChangesRejectReentry)
	{
		int Notifications = 0;
		const auto Handle = Host->MapChanged.AddLambda([&](DWorld*, EMapChangeType) {
			++Notifications;
			EXPECT_FALSE(Host->GetCapabilities().bRunTools);
			EXPECT_FALSE(Host->GetCapabilities().bEditWorld);
			EXPECT_EQ(Host->NewMap(), nullptr);
			EXPECT_FALSE(FEditorFileUtils::LoadMap(Target.AssetPath.ToString()));
		});
		EXPECT_TRUE(FEditorFileUtils::LoadMap(Target.AssetPath.ToString()));
		EXPECT_EQ(Notifications, 2);
		EXPECT_NE(Host->NewMap(), nullptr);
		EXPECT_EQ(Notifications, 4);
		Host->MapChanged.Remove(Handle);
		EXPECT_TRUE(Host->GetCapabilities().bRunTools);
	}

	TEST_F(FLevelMapLifecycleTests, ToolkitDeactivationPreservesModesButMapLoadDeactivatesThem)
	{
		auto* Levels = Host->GetEditorSubsystem<DLevelEditorSubsystem>();
		auto* Modes = &GLevelEditorModeTools(*Host);
		TStrongObjectPtr<DEditorEngine> OtherHost(NewObject<DEditorEngine>(nullptr, "OtherModeHost"));
		EXPECT_NE(&GLevelEditorModeTools(*OtherHost), Modes);
		EXPECT_EQ(GLevelEditorModeTools(*OtherHost).GetActiveMode(), nullptr);
		ASSERT_EQ(Modes->GetActiveModeId(), "Select");
		const auto Toolkit = Module->GetOrCreateLevelEditor(*Editors)->GetToolkit();
		ASSERT_TRUE(Toolkit->RequestDeactivate());
		EXPECT_EQ(Modes->GetActiveModeId(), "Select");
		EXPECT_EQ(Host->GetEditorWorld(), World);
		ASSERT_TRUE(Levels->LoadLevel(Target.AssetPath.ToString()));
		EXPECT_TRUE(Modes->GetActiveModeId().empty());
		EXPECT_EQ(Modes->GetActiveMode(), nullptr);
		EXPECT_FALSE(PreviousLevel.IsValid());
	}

	TEST_F(FLevelMapLifecycleTests, ClosingDirtyWindowPreservesMapAndCanReopen)
	{
		EXPECT_TRUE(Toolkit->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
		EXPECT_TRUE(Toolkit->IsClosed());
		EXPECT_TRUE(Toolkit->GetEditingObjects().empty());
		EXPECT_EQ(Host->GetEditorWorld(), World);
		EXPECT_EQ(World->GetCurrentLevel(), Level);
		EXPECT_EQ(Actor->GetActorLabel(), "Unsaved edit");
		EXPECT_TRUE(Level->GetPackage()->IsDirty());
		auto* Reopened = Module->GetOrCreateLevelEditor(*Editors);
		ASSERT_NE(nullptr, Reopened);
		Toolkit = Reopened->GetToolkit();
		EXPECT_EQ(CurrentResource(), Target.AssetPath.ToString());
		EXPECT_EQ(Toolkit->GetEditingObjects(), (std::vector<DObject*>{Level}));
		EXPECT_TRUE(Level->GetPackage()->IsDirty());
	}

	TEST_F(FLevelMapLifecycleTests, CloseLevelCancellationKeepsTheWorldAndDiscardKeepsTheWindow)
	{
		auto& LevelToolkit = static_cast<LevelEditor::FLevelEditorToolkit&>(*Toolkit);
		FEditorFileUtils::SetSavePrompt([](auto, const auto&) { return FEditorFileUtils::PR_Cancelled; });
		EXPECT_FALSE(LevelToolkit.RequestCloseLevel());
		EXPECT_EQ(Level, Host->GetEditorWorld()->GetCurrentLevel());
		EXPECT_EQ(Toolkit->GetEditingObjects(), std::vector<DObject*>{Level});
		FEditorFileUtils::SetSavePrompt([](auto, const auto&) { return FEditorFileUtils::PR_Declined; });
		ASSERT_TRUE(LevelToolkit.RequestCloseLevel());
		EXPECT_EQ(nullptr, Host->GetEditorWorld());
		EXPECT_TRUE(Toolkit->IsEditorOpen());
		EXPECT_TRUE(Toolkit->GetEditingObjects().empty());
		EXPECT_FALSE(Toolkit->CanSaveActiveDocument());
		EXPECT_EQ("Level Editor", Toolkit->GetEditorDisplayName());
		ASSERT_TRUE(Toolkit->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
		auto* Reopened = Module->GetOrCreateLevelEditor(*Editors);
		ASSERT_NE(nullptr, Reopened);
		Toolkit = Reopened->GetToolkit();
		EXPECT_TRUE(Toolkit->IsEditorOpen());
		EXPECT_EQ(Toolkit, Editors->GetActiveAssetToolkit());
		EXPECT_EQ("Level Editor", Toolkit->GetEditorDisplayName());
		ASSERT_NE(nullptr, Host->NewMap());
		EXPECT_EQ("Untitled", Toolkit->GetEditorDisplayName());
		EXPECT_TRUE(Toolkit->GetEditingObjects().empty());
	}

	TEST_F(FLevelMapLifecycleTests, SaveAsPublishesOnlyTheSavedLevelAndKeepsItsWindow)
	{
		auto& LevelToolkit = static_cast<LevelEditor::FLevelEditorToolkit&>(*Toolkit);
		const std::string Window(Toolkit->GetWindowId());
		EXPECT_FALSE(LevelToolkit.SaveLevelAs(Target.AssetPath.GetPackagePath()));
		EXPECT_EQ(Level, Host->GetEditorWorld()->GetCurrentLevel());
		EXPECT_EQ(Toolkit->GetEditingObjects(), std::vector<DObject*>{Level});
		FPackagePath SavedPath;
		ASSERT_TRUE(FPackagePath::TryCreate("/LevelDiskReload/SavedAs", SavedPath));
		ASSERT_TRUE(LevelToolkit.SaveLevelAs(SavedPath)) << LevelToolkit.GetLastOperationError();
		auto* Saved = Host->GetEditorWorld()->GetCurrentLevel();
		ASSERT_NE(Level, Saved);
		ASSERT_NE(nullptr, Saved);
		EXPECT_EQ(SavedPath, Saved->GetPackage()->GetPackagePathIdentity());
		EXPECT_EQ(Toolkit->GetEditingObjects(), std::vector<DObject*>{Saved});
		EXPECT_EQ(Toolkit->GetEditorInstance(), Editors->FindEditorForAsset(Saved, false));
		EXPECT_EQ(nullptr, Editors->FindEditorForAsset(Level, false));
		EXPECT_EQ(Window, Toolkit->GetWindowId());
		EXPECT_FALSE(Saved->GetPackage()->IsDirty());
		const auto* SavedActor = Saved->FindActorByName("SavedActor");
		ASSERT_NE(nullptr, SavedActor);
		EXPECT_EQ("Unsaved edit", SavedActor->GetActorLabel());
	}

	TEST_F(FLevelMapLifecycleTests, SaveAsPromotesAnUntitledLevelToAnAsset)
	{
		auto* NewWorld = Host->NewMap();
		ASSERT_NE(nullptr, NewWorld);
		auto* Draft = NewWorld->GetCurrentLevel();
		ASSERT_NE(nullptr, Draft);
		ASSERT_NE(nullptr, Draft->SpawnActor<AActor>("TransientActor"));
		EXPECT_EQ("Untitled", Toolkit->GetEditorDisplayName());
		EXPECT_FALSE(Toolkit->GetUnsavedChanges().empty());
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate("/LevelDiskReload/FirstSave", Path));
		auto& LevelToolkit = static_cast<LevelEditor::FLevelEditorToolkit&>(*Toolkit);
		ASSERT_TRUE(LevelToolkit.SaveLevelAs(Path)) << LevelToolkit.GetLastOperationError();
		auto* Saved = Host->GetEditorWorld()->GetCurrentLevel();
		ASSERT_NE(nullptr, Saved);
		EXPECT_FALSE(Saved->HasAnyObjectFlags(EObjectFlags::Transient));
		EXPECT_EQ("FirstSave", Toolkit->GetEditorDisplayName());
		EXPECT_EQ(Toolkit->GetEditingObjects(), std::vector<DObject*>{Saved});
		EXPECT_NE(nullptr, Saved->FindActorByName("TransientActor"));
		EXPECT_TRUE(Toolkit->GetUnsavedChanges().empty());
	}

	TEST_F(FLevelMapLifecycleTests, SaveDirtyPackagesIncludesMapAfterWindowCloses)
	{
		ASSERT_TRUE(Toolkit->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
		ASSERT_TRUE(Toolkit->IsClosed());
		EXPECT_TRUE(std::ranges::contains(FEditorFileUtils::GetDirtyPackages(true, false), Level->GetPackage()));
		EXPECT_FALSE(std::ranges::contains(FEditorFileUtils::GetDirtyPackages(false, true), Level->GetPackage()));
		ASSERT_TRUE(DEditorLoadingAndSavingUtils::SaveDirtyPackages(true, true));
		EXPECT_FALSE(Level->GetPackage()->IsDirty());
		EXPECT_FALSE(std::ranges::contains(FEditorFileUtils::GetDirtyPackages(true, true), Level->GetPackage()));
		ASSERT_TRUE(FEditorFileUtils::LoadMap(Target.AssetPath.ToString()));
		auto* Reloaded = Host->GetEditorWorld()->GetCurrentLevel()->FindActorByName("SavedActor");
		ASSERT_NE(Reloaded, nullptr);
		EXPECT_EQ(Reloaded->GetActorLabel(), "Unsaved edit");
	}

	TEST_F(FLevelMapLifecycleTests, ClosingToolkitReleasesViewsWithoutUnloadingWorld)
	{
		const auto WorldGeneration = Host->GetSceneGeneration();
		auto* Modes = &GLevelEditorModeTools(*Host);
		Module->ReleaseLevelEditor(*Editors);
		EXPECT_TRUE(FToolkitManager::Get().GetToolkits(Editors).empty());
		EXPECT_EQ(Modes->GetActiveMode(), nullptr);
		EXPECT_EQ(Host->GetEditorWorld(), World);
		EXPECT_EQ(World->GetCurrentLevel(), Level);
		EXPECT_TRUE(OriginalWorld.IsValid());
		EXPECT_TRUE(PreviousComponent.IsValid());
		EXPECT_TRUE(Component->IsRegistered());
		EXPECT_EQ(Actor->GetActorLabel(), "Unsaved edit");
		EXPECT_TRUE(Level->GetPackage()->IsDirty());
		EXPECT_EQ(Host->GetSceneGeneration(), WorldGeneration);
		EXPECT_TRUE(Toolkit->GetEditingObjects().empty());
		ASSERT_TRUE(Module->InitializeLevelEditor(*Editors, {}, [] {}));
		const auto Reopened = Module->GetOrCreateLevelEditor(*Editors)->GetToolkit();
		EXPECT_EQ(&GLevelEditorModeTools(*Host), Modes);
		EXPECT_EQ(Reopened->GetEditingObjects(), (std::vector<DObject*>{Level}));
		EXPECT_EQ(Host->GetEditorWorld(), World);
		EXPECT_EQ(Actor->GetActorLabel(), "Unsaved edit");
	}

	TEST_F(FLevelMapLifecycleTests, ScriptLoadReturnsActualResultWithoutCreatingToolkit)
	{
		Toolkit.reset();
		Module->ReleaseLevelEditor(*Editors);
		ASSERT_TRUE(FToolkitManager::Get().GetToolkits(Editors).empty());
		auto* Levels = Host->GetEditorSubsystem<DLevelEditorSubsystem>();
		ASSERT_NE(Levels, nullptr);
		ASSERT_TRUE(Levels->LoadLevel(Target.AssetPath.ToString()));
		EXPECT_FALSE(OriginalWorld.IsValid());
		ASSERT_NE(Host->GetEditorWorld()->GetCurrentLevel()->FindActorByName("SavedActor"), nullptr);
		EXPECT_EQ(Host->GetEditorWorld()->GetCurrentLevel()->FindActorByName("SavedActor")->GetActorLabel(), "On disk");
		EXPECT_EQ(Prompts, 1); // Only fixture's interactive preflight prompted.
		EXPECT_TRUE(FToolkitManager::Get().GetToolkits(Editors).empty());
		EXPECT_FALSE(Levels->LoadLevel("/LevelDiskReload/Missing.Missing"));
		EXPECT_EQ(Prompts, 1);
	}

	TEST_F(FLevelMapLifecycleTests, SynchronousLoadFailureReturnsNullDespiteBlankMapRecovery)
	{
		Renderer.FailNextScene = true;
		EXPECT_EQ(DEditorLoadingAndSavingUtils::LoadMap(Target.AssetPath.ToString()), nullptr);
		EXPECT_FALSE(OriginalWorld.IsValid());
		ASSERT_NE(Host->GetEditorWorld(), nullptr);
		ASSERT_NE(Host->GetEditorWorld()->GetCurrentLevel(), nullptr);
		EXPECT_TRUE(Host->GetEditorWorld()->GetCurrentLevel()->HasAnyObjectFlags(EObjectFlags::Transient));
		EXPECT_TRUE(CurrentResource().empty());
		EXPECT_EQ(Prompts, 1);
		ASSERT_TRUE(FEditorFileUtils::LoadMap(Target.AssetPath.GetPackagePath().ToString()));
		EXPECT_EQ(CurrentResource(), Target.AssetPath.ToString());
		auto* LoadedWorld = DEditorLoadingAndSavingUtils::LoadMap(Target.AssetPath.ToString());
		EXPECT_EQ(LoadedWorld, Host->GetEditorWorld());
	}

	TEST_F(FLevelMapLifecycleTests, ContentBrowserReloadReleasesTemporaryReferences)
	{
		struct FItemSink : ContentBrowser::IContentBrowserItemDataSink
		{
			auto QueueItemDataUpdate(ContentBrowser::FContentBrowserItemDataUpdate&&) -> void override {}
			auto NotifyItemDataRefreshed() -> void override {}
		} Sink;
		TStrongObjectPtr<DContentBrowserAssetDataSource> Source(NewObject<DContentBrowserAssetDataSource>(Host.Get(), {}));
		Source->Initialize();
		Source->SetDataSink(&Sink);
		struct FShutdownSource { DContentBrowserAssetDataSource& Source; ~FShutdownSource() { Source.Shutdown(); } } Shutdown{*Source};
		auto Payload = std::make_shared<ContentBrowser::AssetDataSource::Private::FContentBrowserAssetFileItemDataPayload>();
		Payload->AssetClassName = Target.AssetClassName;
		Payload->PackagePath = Target.AssetPath.GetPackagePath();
		auto Operations = std::make_shared<ContentBrowser::FContentBrowserOperationContext>();
		Operations->OpenAsset = [this](const std::string& Path, const std::string& ClassName) {
			FSoftObjectPath ObjectPath;
			return FSoftObjectPath::TryCreate(Path, ObjectPath) && Editors->OpenAssets({
				.AssetEditors = Editors, .Assets = {{ObjectPath, ClassName}}});
		};
		Payload->Host = Operations;
		const ContentBrowser::FContentBrowserItemData Item(*Source,
			ContentBrowser::EContentBrowserItemFlags::Type_File | ContentBrowser::EContentBrowserItemFlags::Category_Asset,
			Target.AssetPath.ToString(), "Target", "", Payload);
		FEditorFileUtils::SetSavePrompt([](auto, const auto&) { return FEditorFileUtils::PR_Cancelled; });
		ASSERT_TRUE(Source->EditItem(Item));
		EXPECT_TRUE(PreviousLevel.IsValid());
		EXPECT_EQ(Actor->GetActorLabel(), "Unsaved edit");
		FEditorFileUtils::SetSavePrompt([](auto, const auto&) { return FEditorFileUtils::PR_Declined; });
		ASSERT_TRUE(Source->EditItem(Item));
		EXPECT_TRUE(Diagnostic.empty()) << Diagnostic;
		EXPECT_FALSE(PreviousLevel.IsValid());
		ASSERT_NE(Host->GetEditorWorld(), nullptr);
		auto* Reloaded = Host->GetEditorWorld()->GetCurrentLevel();
		ASSERT_NE(Reloaded->FindActorByName("SavedActor"), nullptr);
		EXPECT_EQ(Reloaded->FindActorByName("SavedActor")->GetActorLabel(), "On disk");
		EXPECT_EQ(Toolkit->GetEditingObjects(), (std::vector<DObject*>{Reloaded}));
	}

	TEST_F(FLevelMapLifecycleTests, FailedLoadReplacesEvenASurvivingWorld)
	{
		bool Failed = false;
		const auto Handle = Host->MapChanged.AddLambda([&](DWorld*, EMapChangeType Type) {
			if (Type == EMapChangeType::TearDownWorld && !std::exchange(Failed, true))
				throw std::runtime_error("teardown interrupted before world destruction");
		});
		EXPECT_FALSE(FEditorFileUtils::LoadMap(Target.AssetPath.ToString()));
		Host->MapChanged.Remove(Handle);
		EXPECT_NE(Diagnostic.find("teardown interrupted"), std::string::npos);
		EXPECT_FALSE(OriginalWorld.IsValid());
		ASSERT_NE(Host->GetEditorWorld(), nullptr);
		ASSERT_NE(Host->GetEditorWorld()->GetCurrentLevel(), nullptr);
		EXPECT_TRUE(Host->GetEditorWorld()->GetCurrentLevel()->HasAnyObjectFlags(EObjectFlags::Transient));
		EXPECT_TRUE(CurrentResource().empty());
	}

	TEST_F(FLevelMapLifecycleTests, InvalidMapAdmissionPreservesWorldAndReportsError)
	{
		EXPECT_FALSE(FEditorFileUtils::LoadMap("/LevelDiskReload/Missing.Missing"));
		EXPECT_FALSE(Diagnostic.empty());
		EXPECT_TRUE(OriginalWorld.IsValid());
		EXPECT_EQ(Host->GetEditorWorld(), World);
		EXPECT_EQ(Actor->GetActorLabel(), "Unsaved edit");
		// Even through the base Engine API, runtime travel cannot bypass editor loading.
		EXPECT_FALSE(static_cast<DEngine*>(Host.Get())->OpenLevel(Target.AssetPath));
	}

	TEST(FLevelAssetEditorTests, SuccessiveEditorLifetimesOwnLevelEditorsAndRejectViewAndRestore)
	{
		Testing::InitializeDObjectSystemForTests();
		auto& Module = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
		std::vector<TStrongObjectPtr<DEditorEngine>> Hosts;
		struct FRelease
		{
			FLevelEditorModule& Module;
			std::vector<TStrongObjectPtr<DEditorEngine>>& Hosts;
			~FRelease()
			{
				for (const auto& Host : Hosts)
				{
					if (auto* AssetEditorSubsystem = Host->GetEditorSubsystem<DAssetEditorSubsystem>()) Module.ReleaseLevelEditor(*AssetEditorSubsystem);
					Host->PreExit(); MarkObjectHierarchyAsGarbage(Host.Get());
				}
				Hosts.clear(); CollectGarbage(Durin::EObjectFlags::Standalone);
			}
		} Release{Module, Hosts};
		const auto* Definition = FAssetDefinitionRegistry::Get().GetAssetDefinitionForClass(DLevel::StaticClass());
		ASSERT_NE(nullptr, Definition);
		EXPECT_FALSE(Definition->GetAssetOpenSupport({EAssetOpenMethod::View}).IsSupported);
		EXPECT_EQ(EToolkitMode::WorldCentric, Definition->GetAssetOpenSupport({EAssetOpenMethod::Edit}).RequiredToolkitMode);
		std::shared_ptr<FAssetEditorToolkit> ClosedToolkit;
		for (int Index = 0; Index != 2; ++Index)
		{
			auto* Host = NewObject<FLevelEditorTestEngine>(nullptr, {});
			Hosts.emplace_back(Host);
			ASSERT_NO_THROW(FEditorEngineTestAccess::StartEditor(*Host));
			auto* World = NewObject<DWorld>(Host, "LevelEditorWorld");
			World->SetWorldType(EWorldType::Editor);
			ASSERT_NO_THROW(World->InitializeSubsystems());
			ASSERT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World, "Scene")));
			FEditorEngineTestAccess::SetEditorWorld(*Host, World);
			auto* AssetEditorSubsystem = Host->GetEditorSubsystem<DAssetEditorSubsystem>();
			ASSERT_TRUE(Module.InitializeLevelEditor(*AssetEditorSubsystem, {}, [] {}));
			const auto EditorInstances = Durin::Testing::GetReflectedEditors(AssetEditorSubsystem);
			ASSERT_EQ(1u, EditorInstances.size());
			EXPECT_EQ("LevelEditor", EditorInstances.front()->GetEditorName().ToString());
			EXPECT_EQ(EToolkitMode::WorldCentric, EditorInstances.front()->GetToolkitMode());
			EXPECT_FALSE(EditorInstances.front()->SupportsRestore());
			EXPECT_EQ(nullptr, AssetEditorSubsystem->OpenAssetEditor(EditorInstances.front()->GetClass(), {
				.OpenArgs = {.AssetEditors = AssetEditorSubsystem, .bIsRestore = true}}));
			EXPECT_FALSE(EditorInstances.front()->GetAssetOpenSupport({EAssetOpenMethod::View}).IsSupported);
			EXPECT_TRUE(AssetEditorSubsystem->CaptureEditorState().empty());
			EXPECT_TRUE(EditorInstances.front()->GetEditingObjects().empty());
			if (Index == 0)
			{
				ClosedToolkit = EditorInstances.front()->GetToolkit();
				EXPECT_TRUE(EditorInstances.front()->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
				auto* Reopened = Module.GetOrCreateLevelEditor(*AssetEditorSubsystem);
				ASSERT_NE(nullptr, Reopened);
				EXPECT_NE(EditorInstances.front().Get(), Reopened);
				EXPECT_EQ(1u, Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).size());
				Module.ReleaseLevelEditor(*AssetEditorSubsystem);
				Host->PreExit();
			}
		}
		EXPECT_EQ(nullptr, Hosts.front()->GetMainSceneViewport());
		EXPECT_NE(nullptr, Hosts.back()->GetMainSceneViewport());
		EXPECT_FALSE(ClosedToolkit->IsEditorOpen());
		EXPECT_FALSE(ClosedToolkit->DrawToolkit(true));
		EXPECT_FALSE(ClosedToolkit->SaveActiveDocument());
		EXPECT_EQ(1u, Durin::Testing::GetReflectedEditors(Hosts.back()->GetEditorSubsystem<DAssetEditorSubsystem>()).size());
	}
}
