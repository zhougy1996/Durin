#include "CoreMinimal.h"
#include "LevelEditorModule.h"
#include "EditorEngineTestSupport.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditor.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/ToolkitHost.h"
#include "Documents/EditorDocumentManager.h"
#include "AssetDefinition/AssetDefinitionRegistry.h"
#include "AssetDefinition/AssetDefinition.h"
#include "NativeDObjectTestSupport.h"
#include "DObject/Class.h"
#include "Engine/World.h"
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
		static auto Tick(DEngine& Engine) -> void { Engine.TickMapChange(); }
	};

	// Headless presentation adapter records ownership without allocating an RHI viewport.
	class FLevelSessionTestEngine : public DEditorEngine
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FLevelSessionTestEngine, DEditorEngine, FLevelSessionTestEngine::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FLevelSessionTestEngine)
	public:
		explicit FLevelSessionTestEngine(const FObjectInitializer& Initializer) : Super(Initializer) {}
		auto SetMainSceneViewport(std::shared_ptr<FSceneViewport> Viewport) -> void override
		{ FEditorEngineTestAccess::SetStartupViewport(*this, std::move(Viewport)); }
	};
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FLevelSessionTestEngine)
}

namespace
{
	using namespace Durin;
	TEST(FLevelAssetEditorTests, CancelingConfirmationOrLoadingCompletesOnceAndAllowsAnotherOpen)
	{
		Testing::InitializeAssetSystemForTests();
		Testing::FScopedMountRegistryFixture Mounts;
		const auto Root = Testing::CreateTestFixtureDirectory("LevelOpenCancellation");
		Testing::RegisterMountPointForTests("/LevelOpenCancellation/", Root.generic_string() + "/");
		auto& Module = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
		for (const bool bUnsavedConfirmation : {false, true})
		{
			SCOPED_TRACE(bUnsavedConfirmation);
			TStrongObjectPtr<DEditorEngine> Host(NewObject<FLevelSessionTestEngine>(nullptr, {}));
			ASSERT_NO_THROW(FEditorEngineTestAccess::StartEditor(*Host));
			FPackagePath Path;
			ASSERT_TRUE(FPackagePath::TryCreate(bUnsavedConfirmation ? "/LevelOpenCancellation/Unsaved" : "/LevelOpenCancellation/Loading", Path));
			DLevel* Level = nullptr;
			ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Level));
			TStrongObjectPtr<DPackage> Package(Level->GetPackage());
			struct FRelease
			{
				FLevelEditorModule& Module; DEditorEngine& Host; DPackage& Package;
				~FRelease()
				{
					Package.ClearDirty();
					if (auto* Service = Host.GetEditorSubsystem<DAssetEditorSubsystem>()) Module.ReleaseLevelEditor(*Service);
					Host.PreExit();
					MarkObjectHierarchyAsGarbage(&Host);
					MarkObjectHierarchyAsGarbage(&Package);
				}
			} Release{Module, *Host, *Package};
			ASSERT_TRUE(SavePackage(Package.Get()));
			auto* World = NewObject<DWorld>(Host.Get(), "CancellationWorld");
			World->SetWorldType(EWorldType::Editor);
			World->InitializeSubsystems();
			if (bUnsavedConfirmation)
			{
				ASSERT_TRUE(World->SetCurrentLevel(Level));
				Package->MarkDirty();
			}
			FEditorEngineTestAccess::SetEditorWorld(*Host, World);
			auto* Service = Host->GetEditorSubsystem<DAssetEditorSubsystem>();
			ASSERT_TRUE(Module.InitializeLevelEditor(*Service, {}, [] {}));
			FSoftObjectPath AssetPath;
			ASSERT_TRUE(FSoftObjectPath::TryCreate(Level->GetObjectPath(), AssetPath));
			int Completions = 0;
			auto Subscription = Service->Subscribe([&](const FAssetEditorEvent& Event) {
				if (Event.Type == EAssetEditorEvent::RequestFinished) ++Completions;
			});
			for (int OpenIndex = 0; OpenIndex != 2; ++OpenIndex)
			{
				const auto Opened = Service->OpenAssets({.AssetEditors = Service,
					.Assets = {{AssetPath, DLevel::StaticClass()->GetQualifiedName().ToString()}}});
				ASSERT_EQ(1u, Opened.Items.size());
				const auto Request = Opened.Items.front();
				ASSERT_EQ(EAssetOpenStatus::Pending, Request.Status) << Request.Message;
				EXPECT_EQ(!bUnsavedConfirmation, Host->IsPreparingMapChange());
				(void)Service->CancelOpenRequest(Request.RequestId);
				FMapChangeTestAccess::Tick(*Host);
				EXPECT_EQ(EAssetOpenStatus::Cancelled, Service->ResolveOpenResult(Request).Status);
				EXPECT_EQ(OpenIndex + 1, Completions);
				FMapChangeTestAccess::Tick(*Host);
				EXPECT_EQ(OpenIndex + 1, Completions);
				EXPECT_FALSE(Host->IsPreparingMapChange());
			}
		}
	}

	TEST(FLevelAssetEditorTests, SuccessiveEditorLifetimesOwnLevelSessionsAndRejectViewAndRestore)
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
					if (auto* Service = Host->GetEditorSubsystem<DAssetEditorSubsystem>()) Module.ReleaseLevelEditor(*Service);
					Host->PreExit(); MarkObjectHierarchyAsGarbage(Host.Get());
				}
				Hosts.clear(); CollectGarbage(Durin::EObjectFlags::Standalone);
			}
		} Release{Module, Hosts};
		const auto* Definition = FAssetDefinitionRegistry::Get().FindDefinition(DLevel::StaticClass()->GetQualifiedName().ToString());
		ASSERT_NE(nullptr, Definition);
		EXPECT_FALSE(Definition->GetAssetOpenSupport({EAssetOpenMethod::View}).IsSupported);
		EXPECT_EQ(EToolkitMode::WorldCentric, Definition->GetAssetOpenSupport({EAssetOpenMethod::Edit}).RequiredToolkitMode);
		std::shared_ptr<FAssetEditorToolkit> RetiredToolkit;
		for (int Index = 0; Index != 2; ++Index)
		{
			auto* Host = NewObject<FLevelSessionTestEngine>(nullptr, {});
			Hosts.emplace_back(Host);
			ASSERT_NO_THROW(FEditorEngineTestAccess::StartEditor(*Host));
			auto* World = NewObject<DWorld>(Host, "LevelSessionWorld");
			World->SetWorldType(EWorldType::Editor);
			ASSERT_NO_THROW(World->InitializeSubsystems());
			ASSERT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World, "Scene")));
			FEditorEngineTestAccess::SetEditorWorld(*Host, World);
			auto* Service = Host->GetEditorSubsystem<DAssetEditorSubsystem>();
			ASSERT_TRUE(Module.InitializeLevelEditor(*Service, {}, [] {}));
			const auto Sessions = Service->GetOwnedAssetEditors();
			ASSERT_EQ(1u, Sessions.size());
			EXPECT_EQ("LevelEditor", Sessions.front()->GetEditorName().ToString());
			EXPECT_EQ(EToolkitMode::WorldCentric, Sessions.front()->GetToolkitMode());
			EXPECT_FALSE(Sessions.front()->SupportsRestore());
			EXPECT_EQ(nullptr, Service->OpenAssetEditor(Sessions.front()->GetClass(), {
				.OpenArgs = {.AssetEditors = Service, .bIsRestore = true}}));
			EXPECT_FALSE(Sessions.front()->GetAssetOpenSupport({EAssetOpenMethod::View}).IsSupported);
			EXPECT_TRUE(Service->CaptureSessions().empty());
			EXPECT_EQ(1u, Sessions.front()->GetDocumentManager()->GetDocuments().size());
			EXPECT_EQ(0u, Service->GetUsage().Opened);
			EXPECT_FALSE(Sessions.front()->TransferToHost(std::make_shared<FNonWindowToolkitHost>()));
			EXPECT_EQ(EToolkitMode::WorldCentric, Sessions.front()->GetToolkitMode());
			if (Index == 0)
			{
				RetiredToolkit = Sessions.front()->GetToolkit();
				EXPECT_TRUE(Sessions.front()->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
				auto* Reopened = Module.GetOrCreateLevelEditor(*Service);
				ASSERT_NE(nullptr, Reopened);
				EXPECT_NE(Sessions.front().Get(), Reopened);
				EXPECT_EQ(1u, Service->GetOwnedAssetEditors().size());
				Module.ReleaseLevelEditor(*Service);
				Host->PreExit();
			}
		}
		EXPECT_EQ(nullptr, Hosts.front()->GetMainSceneViewport());
		EXPECT_NE(nullptr, Hosts.back()->GetMainSceneViewport());
		EXPECT_FALSE(RetiredToolkit->IsSessionActive());
		EXPECT_FALSE(RetiredToolkit->DrawToolkit(true));
		EXPECT_FALSE(RetiredToolkit->SaveActiveDocument());
		EXPECT_EQ(1u, Hosts.back()->GetEditorSubsystem<DAssetEditorSubsystem>()->GetOwnedAssetEditors().size());
	}
}
