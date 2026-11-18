#include "Toolkits/AssetEditor.h"
#include "Editor/AssetEditorTestQueries.h"
#include "Editor/AssetEditorTestHost.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/ToolkitHost.h"
#include "Toolkits/ToolkitManager.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Documents/EditorDocumentManager.h"
#include "Documents/EditorDocumentUI.h"
#include "Editor/EditorEngineTestSupport.h"
#include "Editor/IMainFrameModule.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "Asset/PackageSerialization.h"
#include "NativePackageUnloadTestSupport.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "Framework/TabManager.h"
#include "Framework/TabLayout.h"
#include "ThirdParty/ImGui/imgui_internal.h"
#include "AssetDefinition/AssetDefinition.h"
#include "AssetDefinition/AssetDefinitionRegistry.h"
#include "Documents/AssetEditorDocumentModels.h"
#include "Transactions/Transactor.h"
#include "Yaml/Yaml.h"
#include <gtest/gtest.h>

namespace Durin
{
	class FLifecycleTestToolkit : public FBaseAssetToolkit
	{
	public:
		auto GetToolkitId() const -> const FAssetEditorToolkitId& override { return Type; }
		auto OpenDocument(const FEditorDocument&) -> EDocumentOpenResult override { return bDeferOpen ? EDocumentOpenResult::Deferred : EDocumentOpenResult::Opened; }
		auto ActivateDocument(const FEditorDocument&) -> void override {}
		auto OnDrawToolkit(bool) -> bool override { return Drawing ? Drawing() : false; }
		auto ResetLayout() -> void override {}
		auto RequestDeactivate() -> bool override { return Deactivate ? Deactivate() : bCanDeactivate; }
		auto OnHostChanged() -> void override { if (HostChanged) HostChanged(); }
		auto OnRequestClose(EAssetEditorCloseReason Reason) -> bool override
		{
			++CloseRequests;
			LastCloseReason = Reason;
			return bAllowClose && FBaseAssetToolkit::OnRequestClose(Reason);
		}
		auto OnClose() -> void override
		{
			++Closes;
			if (Closing) Closing();
			FBaseAssetToolkit::OnClose();
		}
		auto RequestCloseDocument(const FEditorDocument& Document) -> EDocumentCloseResult override
		{ return CloseRequest ? CloseRequest(Document) : IsDocumentDirty(Document) ? EDocumentCloseResult::PendingConfirmation : EDocumentCloseResult::Closed; }
		auto OnIsDocumentDirty(const FEditorDocument& Document) const -> bool override
		{ if (DirtyQuery) DirtyQuery(); return bDirty || DirtyDocuments.contains(Document.Id.Value); }
		auto OnSaveDocument(const FEditorDocument& Document) -> bool override
		{ if (!bSaveSucceeds) return false; bDirty = false; DirtyDocuments.erase(Document.Id.Value); return true; }
		auto OnDiscardDocument(const FEditorDocument& Document) -> bool override { bDirty = false; DirtyDocuments.erase(Document.Id.Value); return true; }
		auto OnCanSaveActiveDocument() const -> bool override { return true; }
		auto OnSaveActiveDocument() -> bool override { ++Saves; return true; }
		auto OnCanUndo() const -> bool override { return true; }
		auto OnUndo() -> bool override { ++Undos; return true; }
		bool bDirty = false;
		bool bDeferOpen = false;
		std::unordered_set<uint64> DirtyDocuments;
		bool bSaveSucceeds = true;
		bool bCanDeactivate = true;
		std::function<bool()> Deactivate;
		std::function<void()> HostChanged;
		std::function<bool()> Drawing;
		std::function<void()> DirtyQuery;
		std::function<EDocumentCloseResult(const FEditorDocument&)> CloseRequest;
		int Saves = 0;
		int Undos = 0;
		bool bAllowClose = true;
		int CloseRequests = 0;
		int Closes = 0;
		EAssetEditorCloseReason LastCloseReason = EAssetEditorCloseReason::AssetEditorHostClosed;
		std::function<void()> Closing;
		FAssetEditorToolkitId Type{"LifecycleTest"};
	};

	class FReadOnlyDirectTestToolkit final : public FLifecycleTestToolkit
	{
	protected:
		auto CanEditAssets() const -> bool override { return false; }
	};

	class FLifecycleTestEditor : public DAssetEditor
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FLifecycleTestEditor, DAssetEditor, FLifecycleTestEditor::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FLifecycleTestEditor)
	public:
		explicit FLifecycleTestEditor(const FObjectInitializer& Initializer) : Super(Initializer) {}
		auto CreateToolkit() -> std::shared_ptr<FAssetEditorToolkit> override
		{ return bFailCreation ? nullptr : Factory ? Factory() : std::make_shared<FLifecycleTestToolkit>(); }
		inline static bool bFailCreation = false;
		inline static std::function<std::shared_ptr<FAssetEditorToolkit>()> Factory;
	};
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FLifecycleTestEditor)

	class FGenericEditorTestDefinition : public DAssetDefinitionDefault
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FGenericEditorTestDefinition, DAssetDefinitionDefault, FGenericEditorTestDefinition::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FGenericEditorTestDefinition)
	public:
		explicit FGenericEditorTestDefinition(const FObjectInitializer& Initializer) : Super(Initializer) {}
		auto GetAssetClassPath() const -> std::string override { return AssetClassPath; }
		auto GetAssetEditorClass() const -> DClass* override { return EditorClassOverride ? EditorClassOverride : Super::GetAssetEditorClass(); }
		std::string AssetClassPath = DObject::StaticClass()->GetQualifiedName().ToString();
		DClass* EditorClassOverride = nullptr;
		auto CanRegisterStatically() const -> bool override { return false; }
		auto OpenAssets(const FAssetOpenArgs& Args) const -> EAssetCommandResult override
		{ return OpenOverride ? OpenOverride(Args) : Super::OpenAssets(Args); }
		auto GetAssetOpenSupport(const FAssetOpenSupportArgs& Args) const -> FAssetOpenSupport override
		{ auto Support = Super::GetAssetOpenSupport(Args); Support.RequiredToolkitMode = RequiredMode; return Support; }
		std::optional<EToolkitMode> RequiredMode;
		std::function<EAssetCommandResult(const FAssetOpenArgs&)> OpenOverride;
	};
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FGenericEditorTestDefinition)
}

namespace
{
	using namespace Durin;
	template<typename T>
	concept CHasPublicAssetEditorTicker = requires(T& AssetEditorSubsystem) { AssetEditorSubsystem.HandleTicker(0.0f); };
	template<typename T>
	concept CHasPublicAssetEditorShutdown = requires(T& AssetEditorSubsystem) { AssetEditorSubsystem.CloseAssetEditorsForShutdown(); };
	template<typename T>
	concept CHasPublicAssetCallbackDispatch = requires(T& AssetEditorSubsystem) { AssetEditorSubsystem.DispatchEditorCallback([] {}); };
	static_assert(!CHasPublicAssetEditorTicker<DAssetEditorSubsystem>);
	static_assert(!CHasPublicAssetEditorShutdown<DAssetEditorSubsystem>);
	static_assert(!CHasPublicAssetCallbackDispatch<DAssetEditorSubsystem>);
	class FAssetEditorLifecycleTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			Testing::InitializeDObjectSystemForTests();
			Mounts = std::make_unique<Testing::FScopedMountRegistryFixture>();
			Testing::RegisterMountPointForTests("/AssetEditorLifecycle/", Testing::GetTestWorkDirectory().generic_string() + "/");
			Host = NewObject<DEditorEngine>(nullptr, {});
			ASSERT_NO_THROW(FEditorEngineTestAccess::StartEditor(*Host));
			AssetEditorSubsystem = Host->GetEditorSubsystem<DAssetEditorSubsystem>();
			AssetEditorSubsystemLifetime = AssetEditorSubsystem;
			FPackagePath Path;
			ASSERT_TRUE(FPackagePath::TryCreate("/AssetEditorLifecycle/Package", Path));
			Package = CreatePackage(Path);
		}
		auto TearDown() -> void override
		{
			FLifecycleTestEditor::bFailCreation = false;
			FLifecycleTestEditor::Factory = {};
			const auto Remaining = AssetEditorSubsystemLifetime.IsValid() ? Durin::Testing::GetReflectedEditors(AssetEditorSubsystemLifetime.Get())
				: std::vector<TStrongObjectPtr<DAssetEditor>>{};
			for (const auto& EditorInstance : Remaining)
			{
				if (auto Toolkit = std::dynamic_pointer_cast<FLifecycleTestToolkit>(EditorInstance->GetToolkit())) { Toolkit->bDirty = false; Toolkit->DirtyDocuments.clear(); }
				(void)EditorInstance->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed);
			}
			Host->PreExit();
			MarkObjectHierarchyAsGarbage(Host.Get());
			Host.Reset();
			if (Package) MarkObjectHierarchyAsGarbage(Package.Get());
			Package.Reset();
			CollectGarbage(Durin::EObjectFlags::Standalone);
			Mounts.reset();
		}
		auto Asset(const char* Name) -> DObject* { return NewObject<DObject>(Package.Get(), Name); }
		auto Open(std::vector<DObject*> Assets, EAssetOpenMethod Method = EAssetOpenMethod::Edit,
			bool bPrimary = true, std::shared_ptr<IToolkitHost> ToolkitHost = {}, std::string Role = {},
			EToolkitMode Mode = EToolkitMode::Standalone) -> DAssetEditor*
		{
			return AssetEditorSubsystem->OpenAssetEditor(FLifecycleTestEditor::StaticClass(), {
				.OpenArgs = {.AssetEditors = AssetEditorSubsystem, .OpenMethod = Method, .ToolkitMode = Mode, .Host = std::move(ToolkitHost), .InstanceRole = std::move(Role), .bPrimary = bPrimary},
				.Assets = std::move(Assets)});
		}
		auto TickHost() -> void
		{
			FEditorEngineTestAccess::SetStartupState(*Host, EHostStartupState::Ready);
			Host->Tick(0.0f, false);
		}
		std::unique_ptr<Testing::FScopedMountRegistryFixture> Mounts;
		TStrongObjectPtr<DEditorEngine> Host;
		TStrongObjectPtr<DPackage> Package;
		DAssetEditorSubsystem* AssetEditorSubsystem = nullptr;
		TWeakObjectPtr<DAssetEditorSubsystem> AssetEditorSubsystemLifetime;
	};

	TEST_F(FAssetEditorLifecycleTests, ManagerOwnsToolkitAndToolkitKeepsReflectedEditorAliveUntilClose)
	{
		auto* Editor = Open({Asset("ManagerOwnership")});
		ASSERT_NE(Editor, nullptr);
		TWeakObjectPtr<DAssetEditor> WeakEditor(Editor);
		std::weak_ptr<FAssetEditorToolkit> WeakToolkit = Editor->GetToolkit();
		CollectGarbage(EObjectFlags::Standalone);
		ASSERT_TRUE(WeakEditor.IsValid());
		auto Lease = WeakToolkit.lock();
		ASSERT_TRUE(Lease);
		EXPECT_TRUE(FToolkitManager::Get().Contains(*Lease));
		FToolkitManager::Get().CloseToolkit(*Lease);
		EXPECT_FALSE(FToolkitManager::Get().Contains(*Lease));
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
		CollectGarbage(EObjectFlags::Standalone);
		EXPECT_FALSE(WeakEditor.IsValid());
		EXPECT_TRUE(Lease->IsClosed());
		EXPECT_EQ(nullptr, Lease->GetOwningAssetEditor());
		EXPECT_EQ(nullptr, Lease->GetEditorInstance());
		EXPECT_FALSE(Lease->SupportsRestore());
		EXPECT_FALSE(Lease->CanUndo());
		EXPECT_FALSE(Lease->Undo());
		Lease.reset();
		EXPECT_TRUE(WeakToolkit.expired());
	}

	TEST_F(FAssetEditorLifecycleTests, DestroyingHostClosesBothToolkitShapesWithoutAVetoOrOwnershipCycle)
	{
		// Direct interface implementations get the same destruction contract as shell hosts.
		class FPresentationHost final : public IToolkitHost
		{
		public:
			auto GetToolkitMode() const -> EToolkitMode override { return EToolkitMode::Standalone; }
			auto AttachToolkit(std::shared_ptr<FAssetEditorToolkit>) -> bool override { return true; }
			auto DetachToolkit(FAssetEditorToolkit&) -> void override {}
			auto FocusToolkit(FAssetEditorToolkit&) -> void override {}
			auto RequestClose() -> bool override { return false; }
			auto IsAvailable() const -> bool override { return true; }
		};
		auto Presentation = std::make_shared<FPresentationHost>();
		std::weak_ptr<IToolkitHost> WeakHost = Presentation;
		auto* Editor = Open({Asset("HostOwnedReflected")}, EAssetOpenMethod::Edit, true, Presentation);
		ASSERT_NE(Editor, nullptr);
		TWeakObjectPtr<DAssetEditor> WeakEditor(Editor);
		const auto Reflected = std::dynamic_pointer_cast<FLifecycleTestToolkit>(Editor->GetToolkit());
		const auto Direct = std::make_shared<FReadOnlyDirectTestToolkit>();
		const std::vector<DObject*> Assets{Asset("HostOwnedDirect")};
		ASSERT_TRUE(AssetEditorSubsystem->InitializeDirectToolkit(Direct, std::make_shared<FEditorDocumentManager>(),
			{.AssetEditors = AssetEditorSubsystem, .Host = Presentation}, Assets));
		Reflected->bAllowClose = false;
		Direct->bAllowClose = false;
		Presentation.reset();
		EXPECT_TRUE(WeakHost.expired());
		EXPECT_TRUE(Reflected->IsClosed());
		EXPECT_TRUE(Direct->IsClosed());
		EXPECT_EQ(1, Reflected->Closes);
		EXPECT_EQ(1, Direct->Closes);
		EXPECT_EQ(0, Reflected->CloseRequests);
		EXPECT_EQ(0, Direct->CloseRequests);
		EXPECT_TRUE(FToolkitManager::Get().GetToolkits(AssetEditorSubsystem).empty());
		EXPECT_TRUE(AssetEditorSubsystem->GetAllOpenEditors().empty());
		CollectGarbage(EObjectFlags::Standalone);
		EXPECT_FALSE(WeakEditor.IsValid());
	}

	TEST_F(FAssetEditorLifecycleTests, DestroyingPreviousHostAfterTransferDoesNotCloseTheToolkit)
	{
		auto Previous = std::make_shared<FNonWindowToolkitHost>();
		auto Destination = std::make_shared<FNonWindowToolkitHost>(EToolkitMode::WorldCentric);
		auto* Editor = Open({Asset("TransferredOwnership")}, EAssetOpenMethod::Edit, true, Previous);
		ASSERT_NE(Editor, nullptr);
		const auto Toolkit = Editor->GetToolkit();
		ASSERT_TRUE(Editor->GetToolkit()->TransferToHost(Destination));
		Previous.reset();
		EXPECT_TRUE(Toolkit->IsEditorOpen());
		EXPECT_EQ(Toolkit->GetToolkitHost(), Destination);
		Destination.reset();
		EXPECT_TRUE(Toolkit->IsClosed());
		EXPECT_TRUE(FToolkitManager::Get().GetToolkits(AssetEditorSubsystem).empty());
	}

	TEST_F(FAssetEditorLifecycleTests, ClosePermissionPrecedesOneCloseNotificationWithRetainedToolkit)
	{
		auto* Editor = Open({Asset("CloseHooks")});
		ASSERT_NE(nullptr, Editor);
		const auto Toolkit = std::dynamic_pointer_cast<FLifecycleTestToolkit>(Editor->GetToolkit());
		ASSERT_TRUE(Toolkit);
		Toolkit->bAllowClose = false;
		EXPECT_FALSE(Editor->CloseWindow(EAssetEditorCloseReason::CloseAllAssetEditors));
		EXPECT_EQ(1, Toolkit->CloseRequests);
		EXPECT_EQ(0, Toolkit->Closes);
		EXPECT_TRUE(Toolkit->IsEditorOpen());
		EXPECT_FALSE(Toolkit->GetDocumentManager()->GetDocuments().empty());
		Toolkit->bAllowClose = true;
		bool ClosedDuringNotification = false;
		Toolkit->Closing = [&] {
			ClosedDuringNotification = Toolkit->IsClosed();
			// Reentrant close observes the committed state without notifying twice.
			EXPECT_TRUE(Toolkit->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
		};
		EXPECT_TRUE(Editor->CloseWindow(EAssetEditorCloseReason::CloseAllAssetEditors));
		EXPECT_EQ(EAssetEditorCloseReason::CloseAllAssetEditors, Toolkit->LastCloseReason);
		EXPECT_EQ(2, Toolkit->CloseRequests);
		EXPECT_EQ(1, Toolkit->Closes);
		EXPECT_TRUE(ClosedDuringNotification);
		EXPECT_FALSE(Toolkit->IsEditorOpen());
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
		EXPECT_TRUE(Toolkit->CloseWindow(EAssetEditorCloseReason::CloseAllAssetEditors));
		EXPECT_EQ(1, Toolkit->Closes);
		Toolkit->Closing = {};
	}

	TEST_F(FAssetEditorLifecycleTests, ClosedRetainedToolkitReleasesHostAfterCloseAndDetachCallbacks)
	{
		class FObservedHost : public FNonWindowToolkitHost
		{
		public:
			explicit FObservedHost(std::vector<std::string>& InEvents)
				: FNonWindowToolkitHost(EToolkitMode::WorldCentric), Events(InEvents) {}
			~FObservedHost() override { Events.push_back("destroy"); }
			auto DetachToolkit(FAssetEditorToolkit& Toolkit) -> void override
			{
				EXPECT_TRUE(Toolkit.IsClosed());
				Events.push_back("detach");
				FNonWindowToolkitHost::DetachToolkit(Toolkit);
			}
			std::vector<std::string>& Events;
		};
		std::vector<std::string> Events;
		auto ToolkitHost = std::make_shared<FObservedHost>(Events);
		std::weak_ptr<IToolkitHost> WeakHost = ToolkitHost;
		auto* Editor = Open({Asset("HostLifetime")}, EAssetOpenMethod::Edit, true,
			ToolkitHost, {}, EToolkitMode::WorldCentric);
		ASSERT_NE(Editor, nullptr);
		const auto Toolkit = std::dynamic_pointer_cast<FLifecycleTestToolkit>(Editor->GetToolkit());
		Toolkit->Closing = [&] {
			EXPECT_FALSE(WeakHost.expired());
			Events.push_back("close");
		};
		EXPECT_FALSE(WeakHost.expired());
		EXPECT_TRUE(Editor->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
		EXPECT_TRUE(Toolkit->IsClosed());
		ToolkitHost.reset();
		EXPECT_TRUE(WeakHost.expired());
		EXPECT_EQ(Toolkit->GetToolkitHost(), nullptr);
		EXPECT_EQ(Toolkit->GetToolkitMode(), EToolkitMode::WorldCentric);
		EXPECT_EQ(Events, (std::vector<std::string>{"close", "detach", "destroy"}));
		Toolkit->Closing = {};
	}

	TEST_F(FAssetEditorLifecycleTests, PreExitClosesRetainedToolkitBeforeReleasingEditorOwnership)
	{
		auto* Editor = Open({Asset("ExitHooks")});
		ASSERT_NE(nullptr, Editor);
		const auto Toolkit = std::dynamic_pointer_cast<FLifecycleTestToolkit>(Editor->GetToolkit());
		ASSERT_TRUE(Toolkit);
		Toolkit->bAllowClose = false;
		Host->PreExit();
		EXPECT_TRUE(Toolkit->IsClosed());
		EXPECT_EQ(1, Toolkit->Closes);
		EXPECT_EQ(0, Toolkit->CloseRequests);
		EXPECT_FALSE(Toolkit->DrawToolkit(true));
	}

	TEST_F(FAssetEditorLifecycleTests, DefinitionWithdrawalKeepsDirtyEditorAliveUntilItsOwnClose)
	{
		auto* Definition = NewObject<FGenericEditorTestDefinition>(nullptr, {});
		Definition->EditorClassOverride = FLifecycleTestEditor::StaticClass();
		auto Registration = FAssetDefinitionRegistry::Get().RegisterAssetDefinition(*Definition);
		ASSERT_TRUE(Registration);
		const FObjectKey Provider(Definition);
		auto* Object = Asset("WithdrawnProvider");
		FSoftObjectPath Path;
		ASSERT_TRUE(FSoftObjectPath::TryCreate(Object->GetObjectPath(), Path));
		const FAssetOpenTarget Target{Path, Object->GetClass()->GetQualifiedName().ToString()};
		ASSERT_TRUE(AssetEditorSubsystem->OpenAssets({.AssetEditors = AssetEditorSubsystem, .Assets = {Target}}));
		auto* EditorInstance = dynamic_cast<DAssetEditor*>(AssetEditorSubsystem->FindEditorForAsset(Object, false));
		ASSERT_NE(nullptr, EditorInstance);
		auto Toolkit = std::dynamic_pointer_cast<FLifecycleTestToolkit>(EditorInstance->GetToolkit());
		ASSERT_TRUE(Toolkit);
		Toolkit->bDirty = true;
		Registration.Reset();
		CollectGarbage(Durin::EObjectFlags::Standalone);
		EXPECT_EQ(Definition, Provider.ResolveObjectPtr());
		EXPECT_FALSE(AssetEditorSubsystem->CanOpenAsset(Target));
		EXPECT_EQ(EditorInstance, AssetEditorSubsystem->FindEditorForAsset(Object, false));
		EXPECT_TRUE(Toolkit->IsEditorOpen());
		EXPECT_FALSE(EditorInstance->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
		EXPECT_EQ(EDocumentCloseResult::Closed,
			EditorInstance->GetDocumentManager()->ResolvePendingDocumentClose(EDocumentCloseResponse::Discard));
		TickHost();
		EXPECT_EQ(nullptr, AssetEditorSubsystem->FindEditorForAsset(Object, false));
		EXPECT_TRUE(Toolkit->IsClosed());
		Toolkit.reset();
		CollectGarbage(Durin::EObjectFlags::Standalone);
		EXPECT_EQ(nullptr, Provider.ResolveObjectPtr());
	}

	TEST_F(FAssetEditorLifecycleTests, DirectAdmissionRejectsWritableEditBeforeCreatingAHostButAllowsReadOnlyView)
	{
		auto* Definition = NewObject<FGenericEditorTestDefinition>(nullptr, {});
		auto Registration = FAssetDefinitionRegistry::Get().RegisterAssetDefinition(*Definition);
		ASSERT_TRUE(Registration);
		int Hosts = 0;
		std::vector<std::shared_ptr<IToolkitHost>> PresentationHosts;
		AssetEditorSubsystem->SetToolkitHostFactory([&](const FAssetOpenArgs& Args) {
			++Hosts;
			auto Presentation = std::make_shared<FNonWindowToolkitHost>(Args.ToolkitMode);
			PresentationHosts.push_back(Presentation);
			return Presentation;
		});
		class FCountingToolkit final : public FLifecycleTestToolkit
		{
		public:
			auto OpenDocument(const FEditorDocument& Document) -> EDocumentOpenResult override
			{ ++DocumentsOpened; return FLifecycleTestToolkit::OpenDocument(Document); }
			int DocumentsOpened = 0;
		};
		auto Toolkit = std::make_shared<FCountingToolkit>();
		const std::vector<DObject*> Objects{Asset("ReadOnlyDirectContract")};
		EXPECT_FALSE(AssetEditorSubsystem->InitializeDirectToolkit(Toolkit, std::make_shared<FEditorDocumentManager>(),
			{.AssetEditors = AssetEditorSubsystem}, Objects));
		EXPECT_EQ(0, Hosts);
		EXPECT_EQ(0, Toolkit->DocumentsOpened);
		EXPECT_EQ(nullptr, Toolkit->GetEditorInstance());
		EXPECT_TRUE(AssetEditorSubsystem->GetAllOpenEditors().empty());
		ASSERT_TRUE(AssetEditorSubsystem->InitializeDirectToolkit(Toolkit, std::make_shared<FEditorDocumentManager>(),
			{.AssetEditors = AssetEditorSubsystem, .OpenMethod = EAssetOpenMethod::View}, Objects));
		EXPECT_EQ(1, Hosts);
		EXPECT_EQ(1, Toolkit->DocumentsOpened);
		EXPECT_TRUE(Toolkit->IsReadOnly());
		EXPECT_FALSE(Toolkit->SaveActiveDocument());
		EXPECT_FALSE(Toolkit->Undo());
		EXPECT_EQ(0, Toolkit->Saves); EXPECT_EQ(0, Toolkit->Undos);
		EXPECT_TRUE(Toolkit->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
	}

	TEST_F(FAssetEditorLifecycleTests, DefaultDefinitionFocusesNewAndReusedEditorsExactlyOnce)
	{
		auto* Definition = NewObject<FGenericEditorTestDefinition>(nullptr, {});
		Definition->EditorClassOverride = FLifecycleTestEditor::StaticClass();
		auto Registration = FAssetDefinitionRegistry::Get().RegisterAssetDefinition(*Definition);
		ASSERT_TRUE(Registration);
		auto* Object = Asset("FocusExactlyOnce"); FSoftObjectPath Path;
		ASSERT_TRUE(FSoftObjectPath::TryCreate(Object->GetObjectPath(), Path));
		const FAssetOpenArgs Args{.AssetEditors = AssetEditorSubsystem, .Assets = {{Path, Object->GetClass()->GetQualifiedName().ToString()}}};
		int Opened = 0, Focused = 0;
		auto Subscription = AssetEditorSubsystem->Subscribe([&](const auto& Event) {
			if (Event.Type == EAssetEditorEvent::Opened) ++Opened;
			if (Event.Type == EAssetEditorEvent::Focused) ++Focused;
		});
		ASSERT_TRUE(AssetEditorSubsystem->OpenAssets(Args));
		EXPECT_EQ(1, Opened); EXPECT_EQ(1, Focused);
		ASSERT_TRUE(AssetEditorSubsystem->OpenAssets(Args));
		EXPECT_EQ(1, Opened); EXPECT_EQ(2, Focused);
		EXPECT_EQ(1u, Durin::FToolkitManager::Get().GetToolkits(AssetEditorSubsystem).size());
	}

	TEST_F(FAssetEditorLifecycleTests, DirectToolkitAdmissionNormalizesRequiredModeAndRejectsExplicitConflict)
	{
		auto* Definition = NewObject<FGenericEditorTestDefinition>(nullptr, {});
		Definition->RequiredMode = EToolkitMode::WorldCentric;
		auto Registration = FAssetDefinitionRegistry::Get().RegisterAssetDefinition(*Definition);
		ASSERT_TRUE(Registration);
		const std::vector<DObject*> Objects{Asset("DirectRequiredMode")};
		auto Rejected = std::make_shared<FReadOnlyDirectTestToolkit>();
		EXPECT_FALSE(AssetEditorSubsystem->InitializeDirectToolkit(Rejected, std::make_shared<FEditorDocumentManager>(),
			{.AssetEditors = AssetEditorSubsystem, .Host = std::make_shared<FNonWindowToolkitHost>()}, Objects));
		EXPECT_TRUE(Durin::FToolkitManager::Get().GetToolkits(AssetEditorSubsystem).empty());
		auto Accepted = std::make_shared<FReadOnlyDirectTestToolkit>();
		ASSERT_TRUE(AssetEditorSubsystem->InitializeDirectToolkit(Accepted, std::make_shared<FEditorDocumentManager>(), {.AssetEditors = AssetEditorSubsystem}, Objects));
		EXPECT_EQ(EToolkitMode::WorldCentric, Accepted->GetToolkitMode());
		EXPECT_TRUE(Accepted->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
	}

	TEST_F(FAssetEditorLifecycleTests, FailedDirectAttachmentClosesTheCandidateAndPublishesNothing)
	{
		class FRejectingHost final : public FNonWindowToolkitHost
		{
		public:
			auto AttachToolkit(std::shared_ptr<FAssetEditorToolkit>) -> bool override { return false; }
		};
		auto* Definition = NewObject<FGenericEditorTestDefinition>(nullptr, {});
		auto Registration = FAssetDefinitionRegistry::Get().RegisterAssetDefinition(*Definition);
		ASSERT_TRUE(Registration);
		auto Toolkit = std::make_shared<FReadOnlyDirectTestToolkit>();
		int Draws = 0;
		Toolkit->Drawing = [&] { ++Draws; return true; };
		EXPECT_FALSE(Toolkit->IsEditorOpen());
		EXPECT_FALSE(Toolkit->DrawToolkit(true));
		EXPECT_FALSE(Toolkit->SaveActiveDocument());
		auto* Object = Asset("FailedDirect");
		const std::vector<DObject*> Objects{Object};
		EXPECT_FALSE(AssetEditorSubsystem->InitializeDirectToolkit(Toolkit, std::make_shared<FEditorDocumentManager>(),
			{.AssetEditors = AssetEditorSubsystem, .Host = std::make_shared<FRejectingHost>()}, Objects));
		EXPECT_TRUE(Toolkit->IsClosed());
		EXPECT_FALSE(Toolkit->DrawToolkit(true));
		EXPECT_FALSE(Toolkit->SaveActiveDocument());
		EXPECT_EQ(0, Draws);
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
		EXPECT_TRUE(Durin::FToolkitManager::Get().GetToolkits(AssetEditorSubsystem).empty());
		EXPECT_TRUE(AssetEditorSubsystem->GetAllOpenEditors().empty());
	}

	TEST_F(FAssetEditorLifecycleTests, DirectToolkitRetainsWithdrawnProviderWithoutCreatingAReflectedEditor)
	{
		auto* Definition = NewObject<FGenericEditorTestDefinition>(nullptr, {});
		auto Registration = FAssetDefinitionRegistry::Get().RegisterAssetDefinition(*Definition);
		ASSERT_TRUE(Registration);
		const FObjectKey Provider(Definition);
		std::shared_ptr<FReadOnlyDirectTestToolkit> Toolkit;
		std::vector<FAssetEditorEvent> Events;
		auto Subscription = AssetEditorSubsystem->Subscribe([&](const auto& Event) { Events.push_back(Event); });
		Definition->OpenOverride = [&](const FAssetOpenArgs& Args) {
			Toolkit = std::make_shared<FReadOnlyDirectTestToolkit>();
			auto Documents = std::make_shared<FEditorDocumentManager>();
			std::vector<DObject*> Objects;
			for (const auto& Target : Args.Assets) Objects.push_back(Args.LoadAsset(Target));
			const bool Opened = AssetEditorSubsystem->InitializeDirectToolkit(Toolkit, Documents, Args, Objects);
			return Opened ? EAssetCommandResult::Handled : EAssetCommandResult::Unhandled;
		};
		auto* Object = Asset("DirectProvider");
		FSoftObjectPath Path; ASSERT_TRUE(FSoftObjectPath::TryCreate(Object->GetObjectPath(), Path));
		const auto SemanticHost = std::make_shared<FNonWindowToolkitHost>();
		ASSERT_TRUE(AssetEditorSubsystem->OpenAssets({.AssetEditors = AssetEditorSubsystem,
			.Assets = {{Path, Object->GetClass()->GetQualifiedName().ToString()}}, .Host = SemanticHost}));
		ASSERT_TRUE(Toolkit);
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
		EXPECT_EQ(nullptr, Toolkit->GetOwningAssetEditor());
		EXPECT_EQ(Toolkit.get(), AssetEditorSubsystem->FindEditorForAsset(Object, false));
		EXPECT_EQ(Toolkit, AssetEditorSubsystem->GetActiveAssetToolkit());
		Registration.Reset();
		CollectGarbage(Durin::EObjectFlags::Standalone);
		EXPECT_EQ(Definition, Provider.ResolveObjectPtr());
		EXPECT_TRUE(Toolkit->IsEditorOpen());
		int Draws = 0;
		Toolkit->Drawing = [&] { ++Draws; return true; };
		EXPECT_TRUE(Toolkit->DrawToolkit(true));
		EXPECT_EQ(1, Draws);
		EXPECT_TRUE(SemanticHost->RequestClose());
		EXPECT_TRUE(Durin::FToolkitManager::Get().GetToolkits(AssetEditorSubsystem).empty());
		EXPECT_TRUE(AssetEditorSubsystem->GetAllOpenEditors().empty());
		for (const auto& Event : Events)
			if (Event.Type == EAssetEditorEvent::Opened || Event.Type == EAssetEditorEvent::Focused || Event.Type == EAssetEditorEvent::Closed)
				EXPECT_EQ(Toolkit->GetInstanceId(), Event.InstanceId);
		EXPECT_FALSE(Toolkit->DrawToolkit(true));
		EXPECT_FALSE(Toolkit->SaveActiveDocument());
		EXPECT_EQ(1, Draws);
		Toolkit.reset();
		CollectGarbage(Durin::EObjectFlags::Standalone);
		EXPECT_EQ(nullptr, Provider.ResolveObjectPtr());
	}

	TEST_F(FAssetEditorLifecycleTests, DirectToolkitRestoreReopensAssetsAsPrimaryTabsInMainWindow)
	{
		auto* Definition = NewObject<FGenericEditorTestDefinition>(nullptr, {});
		auto Registration = FAssetDefinitionRegistry::Get().RegisterAssetDefinition(*Definition);
		ASSERT_TRUE(Registration);
		Definition->OpenOverride = [&](const FAssetOpenArgs& Args) {
			auto Toolkit = std::make_shared<FReadOnlyDirectTestToolkit>();
			std::vector<DObject*> Objects;
			for (const auto& Target : Args.Assets) Objects.push_back(Args.LoadAsset(Target));
			const bool Opened = AssetEditorSubsystem->InitializeDirectToolkit(Toolkit, std::make_shared<FEditorDocumentManager>(), Args, Objects);
			return Opened ? EAssetCommandResult::Handled : EAssetCommandResult::Unhandled;
		};
		std::vector<FAssetOpenTarget> Targets;
		for (const auto* Name : {"DirectRestoreA", "DirectRestoreB"})
		{
			auto* Object = Asset(Name); FSoftObjectPath Path;
			ASSERT_TRUE(FSoftObjectPath::TryCreate(Object->GetObjectPath(), Path));
			Targets.push_back({Path, Object->GetClass()->GetQualifiedName().ToString()});
		}
		ASSERT_TRUE(SavePackage(Package.Get()));
		ASSERT_TRUE(AssetEditorSubsystem->OpenAssets({.AssetEditors = AssetEditorSubsystem, .Assets = Targets,
			.OpenMethod = EAssetOpenMethod::View, .ToolkitMode = EToolkitMode::WorldCentric,
			.OpenLocation = EAssetEditorOpenLocation::NewWindow, .InstanceRole = "Inspect", .bPrimary = false,
			.bCreateSingleEditor = true}));
		ASSERT_TRUE(AssetEditorSubsystem->OpenAssets({.AssetEditors = AssetEditorSubsystem, .Assets = {Targets.front()}}));
		ASSERT_EQ(2u, Durin::FToolkitManager::Get().GetToolkits(AssetEditorSubsystem).size());
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
		const auto Captured = AssetEditorSubsystem->CaptureEditorState();
		ASSERT_EQ(3u, Captured.size());
		EXPECT_EQ(EAssetOpenMethod::View, Captured.front().Method);
		EXPECT_EQ(Targets.front().AssetPath, Captured.front().AssetPath);
		const auto Earlier = Durin::FToolkitManager::Get().GetToolkits(AssetEditorSubsystem).front();
		const auto PreviousActivation = Earlier->GetLastActivationTime();
		ASSERT_TRUE(AssetEditorSubsystem->ActivateAssetToolkit(*Earlier));
		EXPECT_GT(Earlier->GetLastActivationTime(), PreviousActivation);
		ASSERT_TRUE(AssetEditorSubsystem->CloseAllAssetEditors());
		ASSERT_TRUE(AssetEditorSubsystem->RestoreEditorState(Captured));
		const auto Restored = Durin::FToolkitManager::Get().GetToolkits(AssetEditorSubsystem);
		ASSERT_EQ(3u, Restored.size());
		for (const auto& Toolkit : Restored)
		{
			EXPECT_EQ(1u, Toolkit->GetObjectsToEdit().size());
			EXPECT_EQ(EAssetEditorOpenLocation::MainWindow, Toolkit->GetOpenLocation());
			EXPECT_EQ(EToolkitMode::Standalone, Toolkit->GetToolkitMode());
			EXPECT_TRUE(Toolkit->IsPrimaryEditor());
			EXPECT_TRUE(Toolkit->GetInstanceKey().empty());
		}
		EXPECT_TRUE(AssetEditorSubsystem->CloseAllAssetEditors());
	}

	TEST_F(FAssetEditorLifecycleTests, LifecyclePublishesCommittedAssociationsAndAllowsReentrantClose)
	{
		std::vector<EAssetEditorEvent> Events;
		DAssetEditor* Other = Open({Asset("OtherLifecycle")});
		ASSERT_NE(nullptr, Other);
		auto Subscription = AssetEditorSubsystem->Subscribe([&](const FAssetEditorEvent& Event) {
			Events.push_back(Event.Type);
			if (Event.Type == EAssetEditorEvent::Opened && Other) { auto* Closing = std::exchange(Other, nullptr); Closing->CloseWindow(EAssetEditorCloseReason::CloseAllAssetEditors); }
			if (Event.Type == EAssetEditorEvent::AssetRemoved)
				EXPECT_TRUE(AssetEditorSubsystem->FindEditorsForAsset(Event.Target.AssetPath.IsValid() ? FAssetOpenArgs{}.LoadAsset(Event.Target) : nullptr).empty());
		});
		auto* EditorInstance = Open({Asset("Lifecycle")});
		ASSERT_NE(nullptr, EditorInstance);
		EXPECT_EQ(1u, Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).size());
		EXPECT_EQ(EAssetEditorEvent::Opened, Events.front());
		Subscription.Reset();
		const auto Count = Events.size();
		EditorInstance->CloseWindow(EAssetEditorCloseReason::CloseAllAssetEditors);
		EXPECT_EQ(Count, Events.size());
	}

	TEST_F(FAssetEditorLifecycleTests, GroupDispatchCreatesOnceAndOrdinaryBatchKeepsIndividualEditors)
	{
		auto* Definition = NewObject<FGenericEditorTestDefinition>(nullptr, {});
		auto Registration = FAssetDefinitionRegistry::Get().RegisterAssetDefinition(*Definition);
		auto* A = Asset("BatchA"); auto* B = Asset("BatchB");
		FSoftObjectPath PathA, PathB;
		ASSERT_TRUE(FSoftObjectPath::TryCreate(A->GetObjectPath(), PathA));
		ASSERT_TRUE(FSoftObjectPath::TryCreate(B->GetObjectPath(), PathB));
		const std::vector<FAssetOpenTarget> Targets{{PathA, "Durin::DObject"}, {PathB, "Durin::DObject"}, {PathA, "Durin::DObject"}};
		int Calls = 0;
		Definition->OpenOverride = [&](const FAssetOpenArgs& Args) {
			++Calls; EXPECT_EQ(2u, Args.Assets.size());
			return Definition->DAssetDefinitionDefault::OpenAssets(Args);
		};
		ASSERT_TRUE(AssetEditorSubsystem->OpenAssets({.AssetEditors = AssetEditorSubsystem, .Assets = Targets, .bCreateSingleEditor = true}));
		EXPECT_EQ(1, Calls);
		ASSERT_EQ(1u, Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).size());
		EXPECT_EQ(2u, Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).front()->GetObjectsToEdit().size());
		ASSERT_TRUE(AssetEditorSubsystem->CloseAllAssetEditors());
		ASSERT_TRUE(AssetEditorSubsystem->OpenAssets({.AssetEditors = AssetEditorSubsystem, .Assets = Targets}));
		EXPECT_EQ(2, Calls);
		EXPECT_EQ(2u, Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).size());
	}

	TEST_F(FAssetEditorLifecycleTests, MixedDefinitionGroupExecutesOnceAndInvalidGroupsPublishNothing)
	{
		auto* First = NewObject<FGenericEditorTestDefinition>(nullptr, {});
		auto* Second = NewObject<FGenericEditorTestDefinition>(nullptr, {});
		Second->AssetClassPath = FLifecycleTestEditor::StaticClass()->GetQualifiedName().ToString();
		auto RegistrationA = FAssetDefinitionRegistry::Get().RegisterAssetDefinition(*First);
		auto RegistrationB = FAssetDefinitionRegistry::Get().RegisterAssetDefinition(*Second);
		ASSERT_TRUE(RegistrationA); ASSERT_TRUE(RegistrationB);
		auto* A = Asset("MixedA");
		auto* B = NewObject<FLifecycleTestEditor>(Package.Get(), "MixedB");
		FSoftObjectPath PathA, PathB, Missing;
		ASSERT_TRUE(FSoftObjectPath::TryCreate(A->GetObjectPath(), PathA));
		ASSERT_TRUE(FSoftObjectPath::TryCreate(B->GetObjectPath(), PathB));
		ASSERT_TRUE(FSoftObjectPath::TryCreate("/AssetEditorLifecycle/Missing.Asset", Missing));
		int Calls = 0;
		First->OpenOverride = [&](const FAssetOpenArgs& Args) { ++Calls; return First->DAssetDefinitionDefault::OpenAssets(Args); };
		Second->OpenOverride = [&](const FAssetOpenArgs&) { ADD_FAILURE() << "Grouped selection must execute once"; return EAssetCommandResult::Unhandled; };
		FAssetOpenArgs Args{.AssetEditors = AssetEditorSubsystem, .Assets = {{PathA, First->AssetClassPath}, {PathB, Second->AssetClassPath}}, .bCreateSingleEditor = true};
		ASSERT_TRUE(AssetEditorSubsystem->OpenAssets(Args));
		EXPECT_EQ(1, Calls); EXPECT_EQ(1u, Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).size());
		ASSERT_TRUE(AssetEditorSubsystem->CloseAllAssetEditors());
		Second->EditorClassOverride = FLifecycleTestEditor::StaticClass();
		EXPECT_FALSE(AssetEditorSubsystem->OpenAssets(Args));
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
		Args.Assets[1] = {Missing, First->AssetClassPath};
		const auto MissingResult = AssetEditorSubsystem->OpenAssets(Args);
		EXPECT_FALSE(MissingResult);
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
	}

	TEST_F(FAssetEditorLifecycleTests, StartupLevelObservationUsesActualWorldRegardlessOfDeferredDocuments)
	{
		auto* EditorInstance = Open({Asset("ExistingDocument")});
		ASSERT_NE(nullptr, EditorInstance);
		const auto Toolkit = std::dynamic_pointer_cast<FLifecycleTestToolkit>(EditorInstance->GetToolkit());
		const auto Documents = Toolkit->GetDocumentManager();
		const auto Original = Documents->GetDocuments().front();
		auto* Level = NewObject<DLevel>(Package.Get(), "InitialLevel");
		FSoftObjectPath Path;
		ASSERT_TRUE(FSoftObjectPath::TryCreate(Level->GetObjectPath(), Path));
		FEditorEngineTestAccess::SetInitialLevelPath(*Host, Path);
		FEditorEngineTestAccess::SetStartupState(*Host, EHostStartupState::Ready);
		Toolkit->bDeferOpen = true;
		const auto Deferred = Documents->OpenDocument({.ToolkitId = Toolkit->GetToolkitId(),
			.DocumentKey = Original.DocumentKey, .ResourceId = Path.ToString(), .Label = "Initial Level"});
		ASSERT_TRUE(Deferred.IsValid());
		std::string Error;
		// Generic deferred documents cannot impersonate a synchronous map load.
		EXPECT_EQ(EHostStartupState::Failed, Host->GetStartupCompletion(Error));
		EXPECT_FALSE(Error.empty());
		ASSERT_TRUE(Documents->CompleteDeferredDocumentOpen(Deferred, false));
		EXPECT_EQ(EHostStartupState::Failed, Host->GetStartupCompletion(Error));
		EXPECT_FALSE(Error.empty());
		Toolkit->bDeferOpen = false;
		ASSERT_TRUE(Documents->OpenDocument({.ToolkitId = Toolkit->GetToolkitId(),
			.DocumentKey = Original.DocumentKey, .ResourceId = Path.ToString(), .Label = "Initial Level"}).IsValid());
		auto* World = NewObject<DWorld>(Host.Get(), "InitialWorld");
		World->SetWorldType(EWorldType::Editor);
		World->InitializeSubsystems();
		ASSERT_TRUE(World->SetCurrentLevel(Level));
		FEditorEngineTestAccess::SetEditorWorld(*Host, World);
		// Loading succeeded; first-frame presentation remains a separate startup gate.
		EXPECT_EQ(EHostStartupState::Pending, Host->GetStartupCompletion(Error));
		EXPECT_TRUE(Error.empty());
		ASSERT_TRUE(World->SetCurrentLevel(nullptr));
		EXPECT_EQ(EHostStartupState::Failed, Host->GetStartupCompletion(Error));
	}

	TEST_F(FAssetEditorLifecycleTests, ConfiguredEditorInitializationAndSharedHostHaveOneOwner)
	{
		const auto SharedHost = std::make_shared<FNonWindowToolkitHost>(EToolkitMode::WorldCentric);
		AssetEditorSubsystem->SetToolkitHostFactory([&](const FAssetOpenArgs&) { return SharedHost; });
		auto* A = NewObject<FLifecycleTestEditor>(Host.Get(), {});
		FAssetEditorInitArgs Args{.OpenArgs = {.AssetEditors = AssetEditorSubsystem, .ToolkitMode = EToolkitMode::WorldCentric}, .Assets = {Asset("OwnerA")}};
		ASSERT_EQ(A, AssetEditorSubsystem->InitializeAssetEditor(*A, Args));
		EXPECT_EQ(nullptr, AssetEditorSubsystem->InitializeAssetEditor(*A, Args));
		auto* B = Open({Asset("OwnerB")}, EAssetOpenMethod::Edit, true, {}, {}, EToolkitMode::WorldCentric);
		ASSERT_NE(nullptr, B);
		EXPECT_EQ(A->GetToolkitHost(), B->GetToolkitHost());
		ASSERT_TRUE(A->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
		EXPECT_EQ(SharedHost, B->GetToolkitHost());
		EXPECT_TRUE(SharedHost->IsAvailable()); EXPECT_FALSE(B->IsClosed());
		auto* Foreign = NewObject<FLifecycleTestEditor>(nullptr, {});
		EXPECT_EQ(nullptr, AssetEditorSubsystem->InitializeAssetEditor(*Foreign, Args));
	}

	TEST_F(FAssetEditorLifecycleTests, DirectAdmissionResolvesDefinitionRequirementsBeforeCreatingToolkit)
	{
		auto* Definition = NewObject<FGenericEditorTestDefinition>(nullptr, {});
		Definition->RequiredMode = EToolkitMode::WorldCentric;
		auto Registration = FAssetDefinitionRegistry::Get().RegisterAssetDefinition(*Definition);
		int Created = 0;
		FLifecycleTestEditor::Factory = [&] { ++Created; return std::make_shared<FLifecycleTestToolkit>(); };
		auto* A = Asset("RequiredHost");
		EXPECT_EQ(nullptr, Open({A}, EAssetOpenMethod::Edit, true, std::make_shared<FNonWindowToolkitHost>()));
		EXPECT_EQ(0, Created);
		auto* EditorInstance = Open({A}); ASSERT_NE(nullptr, EditorInstance);
		EXPECT_EQ(EToolkitMode::WorldCentric, EditorInstance->GetToolkitMode()); EXPECT_EQ(1, Created);
		EXPECT_FALSE(EditorInstance->GetToolkit()->TransferToHost(std::make_shared<FNonWindowToolkitHost>()));
		EXPECT_EQ(EToolkitMode::WorldCentric, EditorInstance->GetToolkitMode());
		EXPECT_EQ(nullptr, Open({Asset("InvalidMethod")}, static_cast<EAssetOpenMethod>(255)));
		EXPECT_EQ(1, Created);
	}

	TEST_F(FAssetEditorLifecycleTests, OpeningLocationIsIndependentOfHostModeAndExistingEditorIdentity)
	{
		auto* Definition = NewObject<FGenericEditorTestDefinition>(nullptr, {});
		auto Registration = FAssetDefinitionRegistry::Get().RegisterAssetDefinition(*Definition);
		ASSERT_TRUE(Registration);
		auto* Object = Asset("OpeningLocation");
		FSoftObjectPath Path;
		ASSERT_TRUE(FSoftObjectPath::TryCreate(Object->GetObjectPath(), Path));
		for (const auto Mode : {EToolkitMode::Standalone, EToolkitMode::WorldCentric})
			for (const auto Location : {EAssetEditorOpenLocation::MainWindow, EAssetEditorOpenLocation::NewWindow})
			{
				FAssetOpenArgs Args{.AssetEditors = AssetEditorSubsystem,
					.Assets = {{Path, Object->GetClass()->GetQualifiedName().ToString()}},
					.ToolkitMode = Mode, .OpenLocation = Location};
				ASSERT_TRUE(AssetEditorSubsystem->OpenAssets(Args));
				const auto Toolkits = FToolkitManager::Get().GetToolkits(AssetEditorSubsystem);
				ASSERT_EQ(1u, Toolkits.size());
				EXPECT_EQ(Mode, Toolkits.front()->GetToolkitMode());
				EXPECT_EQ(Location, Toolkits.front()->GetOpenLocation());
				Args.OpenLocation = Location == EAssetEditorOpenLocation::MainWindow
					? EAssetEditorOpenLocation::NewWindow : EAssetEditorOpenLocation::MainWindow;
				ASSERT_TRUE(AssetEditorSubsystem->OpenAssets(Args));
				EXPECT_EQ(Toolkits, FToolkitManager::Get().GetToolkits(AssetEditorSubsystem));
				EXPECT_EQ(Location, Toolkits.front()->GetOpenLocation());
				ASSERT_TRUE(AssetEditorSubsystem->CloseAllAssetEditors());
			}
	}

	TEST_F(FAssetEditorLifecycleTests, CaptureOnlyIncludesAssetsPublishedOnDisk)
	{
		auto* Saved = Asset("SavedAsset");
		ASSERT_TRUE(SavePackage(Package.Get()));
		auto* Unsaved = Asset("UnsavedSibling");
		ASSERT_NE(nullptr, Open({Saved, Unsaved}));
		const auto Captured = AssetEditorSubsystem->CaptureEditorState();
		ASSERT_EQ(1u, Captured.size());
		EXPECT_EQ(Saved->GetObjectPath(), Captured.front().AssetPath.ToString());
		ASSERT_TRUE(SavePackage(Package.Get()));
		EXPECT_EQ(2u, AssetEditorSubsystem->CaptureEditorState().size());
	}

	TEST_F(FAssetEditorLifecycleTests, RestorationResolvesCurrentRegistryType)
	{
		auto* Definition = NewObject<FGenericEditorTestDefinition>(nullptr, {});
		auto Registration = FAssetDefinitionRegistry::Get().RegisterAssetDefinition(*Definition);
		ASSERT_TRUE(Registration);
		auto* Original = Asset("RegistryTypedAsset");
		FAssetWriteResult Saved;
		ASSERT_TRUE(SavePackage(Package.Get(), {}, &Saved)) << (Saved.Error ? Saved.Error->ToString() : "");
		FSoftObjectPath Path;
		ASSERT_TRUE(FSoftObjectPath::TryCreate(Original->GetObjectPath(), Path));
		const auto ClassName = Original->GetClass()->GetQualifiedName().ToString();
		ASSERT_TRUE(AssetEditorSubsystem->OpenAssets({.AssetEditors = AssetEditorSubsystem,
			.Assets = {{Path, ClassName}}, .OpenMethod = EAssetOpenMethod::View}));
		const auto Captured = AssetEditorSubsystem->CaptureEditorState();
		ASSERT_EQ(1u, Captured.size());
		ASSERT_TRUE(AssetEditorSubsystem->CloseAllAssetEditors());
		const auto PackagePath = Package->GetPackagePathIdentity();
		Package.Reset();
		const auto Unloaded = UnloadPackage(PackagePath);
		ASSERT_TRUE(Unloaded) << Unloaded.error();
		std::string OpenedClass;
		Definition->OpenOverride = [&](const FAssetOpenArgs& Args) {
			OpenedClass = Args.Assets.front().AssetClassName;
			return Definition->DAssetDefinitionDefault::OpenAssets(Args);
		};
		ASSERT_TRUE(AssetEditorSubsystem->RestoreEditorState(Captured));
		EXPECT_EQ(ClassName, OpenedClass);
		Package = FindPackage(PackagePath.ToString());
		ASSERT_TRUE(Package);
		EXPECT_EQ(ClassName, AssetEditorSubsystem->GetActiveAssetToolkit()->GetObjectsToEdit().front()->GetClass()->GetQualifiedName().ToString());
	}

	TEST_F(FAssetEditorLifecycleTests, RestoreRoundTripsAssetListAndViewAndReportsMissingItems)
	{
		auto* Definition = NewObject<FGenericEditorTestDefinition>(nullptr, {});
		auto Registration = FAssetDefinitionRegistry::Get().RegisterAssetDefinition(*Definition);
		ASSERT_TRUE(Registration);
		auto* A = Asset("RestoreA"); auto* B = Asset("RestoreB");
		FSoftObjectPath PathA, PathB, MissingPath;
		ASSERT_TRUE(FSoftObjectPath::TryCreate(A->GetObjectPath(), PathA));
		ASSERT_TRUE(FSoftObjectPath::TryCreate(B->GetObjectPath(), PathB));
		ASSERT_TRUE(FSoftObjectPath::TryCreate("/AssetEditorLifecycle/Missing.Missing", MissingPath));
		const FAssetOpenTarget TargetA{PathA, A->GetClass()->GetQualifiedName().ToString()};
		const FAssetOpenTarget TargetB{PathB, B->GetClass()->GetQualifiedName().ToString()};
		ASSERT_TRUE(SavePackage(Package.Get()));
		ASSERT_TRUE(AssetEditorSubsystem->OpenAssets({.AssetEditors = AssetEditorSubsystem,
			.Assets = {TargetA, TargetB}, .OpenMethod = EAssetOpenMethod::View,
			.ToolkitMode = EToolkitMode::WorldCentric, .OpenLocation = EAssetEditorOpenLocation::NewWindow,
			.InstanceRole = "RestoreInspection", .bPrimary = false, .bCreateSingleEditor = true}));
		ASSERT_EQ(1u, Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).size());
		EXPECT_EQ(2u, Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).front()->GetObjectsToEdit().size());
		const auto Path = (Testing::GetTestWorkDirectory() / "AssetEditorRestore.yaml").generic_string();
		ASSERT_TRUE(AssetEditorSubsystem->SaveEditorState(Path));
		FYamlDocument Saved;
		ASSERT_TRUE(Saved.LoadFromFile(Path));
		EXPECT_EQ(2, Saved.GetRootView().GetView("Version").GetInt());
		const auto Nodes = Saved.GetRootView().GetView("OpenAssets");
		ASSERT_EQ(2u, Nodes.Num());
		EXPECT_FALSE(Nodes.GetView(size_t{0}).GetView("Class").IsValid());
		const auto RecentNodes = Saved.GetRootView().GetView("Recent");
		ASSERT_EQ(2u, RecentNodes.Num());
		EXPECT_FALSE(RecentNodes.GetView(size_t{0}).GetView("Class").IsValid());
		ASSERT_TRUE(AssetEditorSubsystem->CloseAllAssetEditors());
		const auto PackagePath = Package->GetPackagePathIdentity();
		Package.Reset();
		ASSERT_TRUE(UnloadPackage(PackagePath));
		ASSERT_TRUE(AssetEditorSubsystem->LoadEditorState(Path));
		Package = FindPackage(PackagePath.ToString());
		ASSERT_TRUE(Package);
		const auto Restored = AssetEditorSubsystem->CaptureEditorState();
		ASSERT_EQ(2u, Restored.size());
		for (const auto& Toolkit : FToolkitManager::Get().GetToolkits(AssetEditorSubsystem))
		{
			EXPECT_EQ(EAssetOpenMethod::View, Toolkit->GetOpenMethod());
			EXPECT_EQ(EAssetEditorOpenLocation::MainWindow, Toolkit->GetOpenLocation());
			EXPECT_EQ(EToolkitMode::Standalone, Toolkit->GetToolkitMode());
			EXPECT_TRUE(Toolkit->GetInstanceKey().empty());
			EXPECT_TRUE(Toolkit->IsPrimaryEditor());
		}
		const std::vector<FAssetEditorRestoreEntry> Mixed{{.AssetPath = PathA}, {.AssetPath = MissingPath}};
		const auto Result = AssetEditorSubsystem->RestoreEditorState(Mixed);
		EXPECT_FALSE(Result);
		EXPECT_FALSE(Durin::FToolkitManager::Get().GetToolkits(AssetEditorSubsystem).empty());
		EXPECT_TRUE(AssetEditorSubsystem->CloseAllAssetEditors());
		Saved.GetMutableRoot().SetChildValue("Version", 0);
		ASSERT_TRUE(Saved.SaveToFile(Path));
		EXPECT_TRUE(AssetEditorSubsystem->LoadEditorState(Path));
		EXPECT_TRUE(FToolkitManager::Get().GetToolkits(AssetEditorSubsystem).empty());
		Saved.GetMutableRoot().SetChildValue("Version", 2);
		ASSERT_TRUE(Saved.SaveToFile(Path));
		Registration.Reset();
		const auto MissingProvider = AssetEditorSubsystem->LoadEditorState(Path);
		EXPECT_FALSE(MissingProvider);
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
		FYamlDocument Snapshot;
		ASSERT_TRUE(Snapshot.LoadFromFile(Path));
		Snapshot.GetMutableRoot().SetChildValue("Project", "AnotherProject.dproject");
		ASSERT_TRUE(Snapshot.SaveToFile(Path));
		const auto WrongProject = AssetEditorSubsystem->LoadEditorState(Path);
		EXPECT_FALSE(WrongProject);
	}

	TEST_F(FAssetEditorLifecycleTests, RestorationUsesOrdinaryFocusWithoutRefocusingOnLaterTicks)
	{
		auto* Definition = NewObject<FGenericEditorTestDefinition>(nullptr, {});
		auto Registration = FAssetDefinitionRegistry::Get().RegisterAssetDefinition(*Definition);
		ASSERT_TRUE(Registration);
		auto* Object = Asset("OrdinaryRestoreFocus");
		FSoftObjectPath Path; ASSERT_TRUE(FSoftObjectPath::TryCreate(Object->GetObjectPath(), Path));
		ASSERT_TRUE(SavePackage(Package.Get()));
		const std::vector<FAssetEditorRestoreEntry> Entries{{.AssetPath = Path},
			{.AssetPath = Path, .Method = EAssetOpenMethod::View}};
		ASSERT_TRUE(AssetEditorSubsystem->RestoreEditorState(Entries));
		auto* Edit = Durin::Testing::FindReflectedEditor(AssetEditorSubsystem, Object, EAssetOpenMethod::Edit);
		auto* View = Durin::Testing::FindReflectedEditor(AssetEditorSubsystem, Object, EAssetOpenMethod::View);
		ASSERT_NE(nullptr, Edit); ASSERT_NE(nullptr, View);
		EXPECT_EQ(View->GetToolkit(), AssetEditorSubsystem->GetActiveAssetToolkit());
		Edit->FocusWindow();
		TickHost();
		EXPECT_EQ(Edit->GetToolkit(), AssetEditorSubsystem->GetActiveAssetToolkit());
		TickHost();
		EXPECT_EQ(Edit->GetToolkit(), AssetEditorSubsystem->GetActiveAssetToolkit());
	}



	TEST_F(FAssetEditorLifecycleTests, DefaultPanelFactoriesCreateVisiblePanels)
	{
		class FFactoryToolkit final : public FLifecycleTestToolkit
		{
		public:
			FFactoryToolkit(int& Viewports, int& Details)
				: Viewports(Viewports), Details(Details) {}
		protected:
			auto CreateDefaultViewportPanel() -> std::optional<FToolkitPanelSpawner> override
			{ ++Viewports; return FToolkitPanelSpawner{"Viewport", "Viewport", [] {}}; }
			auto CreateDefaultDetailsPanel() -> std::optional<FToolkitPanelSpawner> override
			{ ++Details; return FToolkitPanelSpawner{"Details", "Details", [] {}}; }
		private:
			int& Viewports;
			int& Details;
		};
		int Viewports = 0, Details = 0;
		FLifecycleTestEditor::Factory = [&] { return std::make_shared<FFactoryToolkit>(Viewports, Details); };
		auto* EditorInstance = Open({Asset("DefaultPanels")});
		ASSERT_NE(nullptr, EditorInstance);
		EXPECT_EQ(1, Viewports); EXPECT_EQ(1, Details);
		const auto Toolkit = std::dynamic_pointer_cast<FBaseAssetToolkit>(EditorInstance->GetToolkit());
		EXPECT_TRUE(Toolkit->IsPanelOpen("Viewport"));
		EXPECT_TRUE(Toolkit->IsPanelOpen("Details"));
	}

	TEST_F(FAssetEditorLifecycleTests, ScopedToolsUseTheActiveCommandContextAndReleaseCallbacks)
	{
		auto* First = Open({Asset("ToolFirst")});
		ASSERT_NE(nullptr, First);
		const auto Toolkit = std::dynamic_pointer_cast<FBaseAssetToolkit>(First->GetToolkit());
		auto Lifetime = std::make_shared<int>(0);
		const std::weak_ptr<int> Weak = Lifetime;
		int Calls = 0;
		auto Tool = Toolkit->RegisterUIExtension({.Commands = {{.Id = "Tool.Action", .Label = "Action",
			.Execute = [Lifetime, &Calls] { ++Calls; }}}});
		Lifetime.reset();
		ASSERT_TRUE(Tool);
		EXPECT_TRUE(Toolkit->InvokeCommand("Tool.Action"));
		auto* Second = Open({Asset("ToolSecond")});
		ASSERT_NE(nullptr, Second);
		EXPECT_FALSE(Toolkit->InvokeCommand("Tool.Action"));
		First->FocusWindow();
		EXPECT_TRUE(Toolkit->InvokeCommand("Tool.Action"));
		Tool.Reset();
		EXPECT_TRUE(Weak.expired());
		EXPECT_FALSE(Toolkit->InvokeCommand("Tool.Action"));
		EXPECT_EQ(2, Calls);
	}

	TEST_F(FAssetEditorLifecycleTests, ClosingEditorReleasesToolCallbacksEvenWithAFrameLease)
	{
		auto* EditorInstance = Open({Asset("ReleaseTool")});
		ASSERT_NE(nullptr, EditorInstance);
		const auto Toolkit = std::dynamic_pointer_cast<FBaseAssetToolkit>(EditorInstance->GetToolkit());
		auto Lifetime = std::make_shared<int>(0);
		const std::weak_ptr<int> Weak = Lifetime;
		auto Tool = Toolkit->RegisterUIExtension({.Panels = {{"Tool.Panel", "Panel", [Lifetime] {}}}});
		Lifetime.reset();
		ASSERT_TRUE(Tool);
		EXPECT_TRUE(EditorInstance->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
		EXPECT_TRUE(Weak.expired());
		EXPECT_FALSE(Toolkit->RegisterUIExtension({.Commands = {{"Later", "Later", [] {}}}}));
		Tool.Reset();
	}

	TEST_F(FAssetEditorLifecycleTests, PanelVisibilityRestoresAndDuplicateExtensionsAreAtomic)
	{
		auto* EditorInstance = Open({Asset("PanelLayout")});
		ASSERT_NE(nullptr, EditorInstance);
		const auto Toolkit = std::dynamic_pointer_cast<FBaseAssetToolkit>(EditorInstance->GetToolkit());
		auto Panels = Toolkit->RegisterUIExtension({.Panels = {{"Preview", "Preview", [] {}}, {"Details", "Details", [] {}, false}}});
		ASSERT_TRUE(Panels);
		const auto Path = (Testing::GetTestWorkDirectory() / "editor-panel-layout.yaml").generic_string();
		std::filesystem::remove(Path);
		EXPECT_TRUE(Toolkit->RestorePanelLayout(Path));
		EXPECT_TRUE(Toolkit->SetPanelOpen("Preview", false));
		EXPECT_TRUE(Toolkit->SetPanelOpen("Details", true));
		FLifecycleTestToolkit Restored;
		auto RestoredPanels = Restored.RegisterUIExtension({.Panels = {{"Preview", "Preview", [] {}}, {"Details", "Details", [] {}, false}}});
		ASSERT_TRUE(RestoredPanels);
		EXPECT_TRUE(Restored.RestorePanelLayout(Path));
		EXPECT_FALSE(Restored.IsPanelOpen("Preview"));
		EXPECT_TRUE(Restored.IsPanelOpen("Details"));
		Restored.ResetPanels();
		EXPECT_TRUE(Restored.IsPanelOpen("Preview"));
		EXPECT_FALSE(Restored.IsPanelOpen("Details"));
		EXPECT_FALSE(Toolkit->RegisterUIExtension({.Panels = {{"New", "New", [] {}}, {"Preview", "Duplicate", [] {}}}}));
		EXPECT_FALSE(Toolkit->SetPanelOpen("New", true));
		std::filesystem::remove(Path);
	}

	TEST_F(FAssetEditorLifecycleTests, ViewAllowsInspectionToolsButRejectsMutatingCommands)
	{
		auto* EditorInstance = Open({Asset("ViewTool")}, EAssetOpenMethod::View);
		ASSERT_NE(nullptr, EditorInstance);
		const auto Toolkit = std::dynamic_pointer_cast<FBaseAssetToolkit>(EditorInstance->GetToolkit());
		int Calls = 0;
		auto Tool = Toolkit->RegisterUIExtension({.Commands = {
			{.Id = "Mutate", .Label = "Mutate", .Execute = [&] { ++Calls; }},
			{.Id = "Inspect", .Label = "Inspect", .Execute = [&] { ++Calls; }, .bMutating = false}}});
		ASSERT_TRUE(Tool);
		EXPECT_FALSE(Toolkit->InvokeCommand("Mutate"));
		EXPECT_TRUE(Toolkit->InvokeCommand("Inspect"));
		EXPECT_EQ(1, Calls);
	}

	TEST_F(FAssetEditorLifecycleTests, AToolCanWithdrawItselfDuringCommandEligibility)
	{
		auto* EditorInstance = Open({Asset("ReentrantTool")});
		ASSERT_NE(nullptr, EditorInstance);
		const auto Toolkit = std::dynamic_pointer_cast<FBaseAssetToolkit>(EditorInstance->GetToolkit());
		FToolkitUIRegistration Tool;
		bool Executed = false;
		Tool = Toolkit->RegisterUIExtension({.Commands = {{.Id = "Withdraw", .Label = "Withdraw",
			.Execute = [&] { Executed = true; }, .CanExecute = [&] { Tool.Reset(); return true; }}}});
		EXPECT_FALSE(Toolkit->InvokeCommand("Withdraw"));
		EXPECT_FALSE(Executed);
	}

	TEST_F(FAssetEditorLifecycleTests, HostFactoryIsEditorLocalAndFailedPresentationDoesNotPublish)
	{
		int Created = 0;
		std::vector<std::shared_ptr<IToolkitHost>> PresentationHosts;
		AssetEditorSubsystem->SetToolkitHostFactory([&](const FAssetOpenArgs& Args) -> std::shared_ptr<IToolkitHost> {
			++Created;
			if (Args.ToolkitMode == EToolkitMode::WorldCentric) return nullptr;
			auto Presentation = std::make_shared<FNonWindowToolkitHost>();
			PresentationHosts.push_back(Presentation);
			return Presentation;
		});
		EXPECT_EQ(nullptr, Open({Asset("NoWorldHost")}, EAssetOpenMethod::Edit, true, {}, {}, EToolkitMode::WorldCentric));
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
		ASSERT_NE(nullptr, Open({Asset("HasStandaloneHost")}));
		EXPECT_EQ(2, Created);
		EXPECT_EQ(1u, AssetEditorSubsystem->GetAllOpenEditors().size());
		AssetEditorSubsystem->SetToolkitHostFactory({});
	}

	TEST_F(FAssetEditorLifecycleTests, ToolkitSessionReportsDocumentVisibilityBeforeDrawing)
	{
		auto* EditorInstance = Open({Asset("VisibleDocument")});
		ASSERT_NE(nullptr, EditorInstance);
		const auto Toolkit = EditorInstance->GetToolkit();
		auto* Context = ImGui::CreateContext();
		std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> Guard(Context, &ImGui::DestroyContext);
		auto& IO = ImGui::GetIO();
		IO.IniFilename = nullptr;
		IO.DisplaySize = ImVec2(1280, 720);
		IO.DeltaTime = 1.0f / 60;
		IO.Fonts->Build();
		FTabManager Tabs;
		Tabs.RequestFocus(EditorInstance->GetDocumentManager()->GetDocuments().front().Id);
		std::vector<std::string> Events;
		ImGui::NewFrame();
		Tabs.DrawDocuments(*EditorInstance->GetDocumentManager(), Toolkit->GetToolkitId(),
			Toolkit->GetPresentationRootKey("VisibilityTest"), [](const auto&) { return true; },
			[&](const auto&) { Events.emplace_back("draw"); },
			[&](const auto&, bool bVisible) { Events.emplace_back(bVisible ? "visible" : "hidden"); });
		ImGui::Render();
		EXPECT_EQ(Events, (std::vector<std::string>{"visible", "draw"}));
	}

	TEST_F(FAssetEditorLifecycleTests, FallbackHostsAreReclaimedAfterCloseAndTransfer)
	{
		auto* First = Open({Asset("FallbackFirst")});
		auto* Second = Open({Asset("FallbackSecond")});
		ASSERT_NE(nullptr, First);
		ASSERT_NE(nullptr, Second);
		std::weak_ptr<IToolkitHost> FirstHost = First->GetToolkitHost();
		std::weak_ptr<IToolkitHost> SecondHost = Second->GetToolkitHost();
		TickHost();
		EXPECT_FALSE(FirstHost.expired());
		EXPECT_FALSE(SecondHost.expired());
		ASSERT_TRUE(First->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
		TickHost();
		EXPECT_TRUE(FirstHost.expired());
		EXPECT_FALSE(SecondHost.expired());
		const auto Replacement = std::make_shared<FNonWindowToolkitHost>();
		ASSERT_TRUE(Second->GetToolkit()->TransferToHost(Replacement));
		TickHost();
		EXPECT_TRUE(SecondHost.expired());
		EXPECT_EQ(Replacement, Second->GetToolkitHost());
		EXPECT_FALSE(Second->IsClosed());
	}

	TEST_F(FAssetEditorLifecycleTests, FailedInitializationDoesNotRetainAnUnusedFallbackHost)
	{
		class FRejectingToolkit final : public FLifecycleTestToolkit
		{
		public:
			explicit FRejectingToolkit(std::weak_ptr<IToolkitHost>& InHost) : ObservedHost(InHost) {}
			auto OpenDocument(const FEditorDocument&) -> EDocumentOpenResult override
			{ ObservedHost = GetToolkitHost(); return EDocumentOpenResult::Rejected; }
			std::weak_ptr<IToolkitHost>& ObservedHost;
		};
		std::weak_ptr<IToolkitHost> UnusedHost;
		FLifecycleTestEditor::Factory = [&] { return std::make_shared<FRejectingToolkit>(UnusedHost); };
		const auto* Failed = Open({Asset("RejectedFallbackDocument")});
		FLifecycleTestEditor::Factory = {};
		EXPECT_EQ(nullptr, Failed);
		EXPECT_FALSE(UnusedHost.expired());
		TickHost();
		EXPECT_TRUE(UnusedHost.expired());
		EXPECT_TRUE(FToolkitManager::Get().GetToolkits(AssetEditorSubsystem).empty());
	}

	TEST_F(FAssetEditorLifecycleTests, ReflectedAndDirectEditorFamiliesHaveDistinctIdentities)
	{
		auto* Object = Asset("SharedToolkitId");
		auto* Reflected = Open({Object});
		ASSERT_NE(nullptr, Reflected);
		const auto Direct = std::make_shared<FReadOnlyDirectTestToolkit>();
		ASSERT_EQ(Reflected->GetToolkit()->GetToolkitId(), Direct->GetToolkitId());
		const std::vector<DObject*> Assets{Object};
		ASSERT_TRUE(AssetEditorSubsystem->InitializeDirectToolkit(Direct, std::make_shared<FEditorDocumentManager>(),
			{.AssetEditors = AssetEditorSubsystem}, Assets));
		EXPECT_EQ(nullptr, Open({Object}));
		const auto Duplicate = std::make_shared<FReadOnlyDirectTestToolkit>();
		EXPECT_FALSE(AssetEditorSubsystem->InitializeDirectToolkit(Duplicate, std::make_shared<FEditorDocumentManager>(),
			{.AssetEditors = AssetEditorSubsystem}, Assets));
		EXPECT_EQ(2u, FToolkitManager::Get().GetToolkits(AssetEditorSubsystem).size());
	}

	TEST_F(FAssetEditorLifecycleTests, GroupedEditorsUseOneRootEachAndIndependentPanelLayouts)
	{
		auto* Group = Open({Asset("GroupFirst"), Asset("GroupSecond")});
		auto* Single = Open({Asset("OtherWindow")});
		ASSERT_NE(nullptr, Group);
		ASSERT_NE(nullptr, Single);
		const auto First = std::dynamic_pointer_cast<FLifecycleTestToolkit>(Group->GetToolkit());
		const auto Second = std::dynamic_pointer_cast<FLifecycleTestToolkit>(Single->GetToolkit());
		FTabManager FirstTabs, SecondTabs;
		auto Configure = [](DAssetEditor& EditorInstance, FLifecycleTestToolkit& Toolkit, FTabManager& Tabs) {
			Toolkit.Drawing = [&EditorInstance, &Toolkit, &Tabs] {
				return Tabs.DrawDocuments(*EditorInstance.GetDocumentManager(), Toolkit.GetToolkitId(),
					Toolkit.GetPresentationRootKey("LifecycleTest"), [](const auto&) { return true; },
					[](const auto&) { ImGui::TextUnformatted("Document content"); });
			};
		};
		Configure(*Group, *First, FirstTabs);
		Configure(*Single, *Second, SecondTabs);
		int FirstPanels = 0, SecondPanels = 0;
		auto FirstPanel = First->RegisterUIExtension({.Panels = {{"Details", "Details", [&] { ++FirstPanels; }}}});
		auto SecondPanel = Second->RegisterUIExtension({.Panels = {{"Details", "Details", [&] { ++SecondPanels; }}}});
		const auto LayoutFile = Testing::GetTestWorkDirectory() / "draw-panels.yaml";
		std::filesystem::remove(LayoutFile);
		ASSERT_TRUE(First->RestorePanelLayout(LayoutFile.generic_string()));
		ASSERT_TRUE(Second->RestorePanelLayout(LayoutFile.generic_string()));
		ImGuiContext* Context = ImGui::CreateContext();
		std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> ContextGuard(Context, &ImGui::DestroyContext);
		auto& IO = ImGui::GetIO();
		IO.IniFilename = nullptr;
		IO.DisplaySize = ImVec2(1200, 800);
		IO.DeltaTime = 1.0f / 60;
		IO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		IO.Fonts->Build();
		for (int Frame = 0; Frame < 3; ++Frame)
		{
			ImGui::NewFrame();
			ImGui::SetNextWindowSize(ImVec2(500, 500), ImGuiCond_Always);
			Group->GetToolkit()->DrawToolkit(false);
			ImGui::SetNextWindowSize(ImVec2(500, 500), ImGuiCond_Always);
			Single->GetToolkit()->DrawToolkit(true);
			ImGui::Render();
			EXPECT_EQ(0, Context->ErrorCountCurrentFrame);
		}
		const auto FirstKey = First->GetPresentationRootKey("LifecycleTest");
		const auto SecondKey = Second->GetPresentationRootKey("LifecycleTest");
		const auto* FirstRoot = ImGui::FindWindowByName(TabLayout::MakeRootWindowName("", FirstKey).c_str());
		const auto* SecondRoot = ImGui::FindWindowByName(TabLayout::MakeRootWindowName("", SecondKey).c_str());
		ASSERT_NE(nullptr, FirstRoot);
		ASSERT_NE(nullptr, SecondRoot);
		EXPECT_NE(FirstRoot->ID, SecondRoot->ID);
		EXPECT_GT(FirstPanels, 0);
		EXPECT_GT(SecondPanels, 0);
		const std::string Ini = ImGui::SaveIniSettingsToMemory();
		EXPECT_EQ(std::string::npos, Ini.find(FirstKey));
		EXPECT_EQ(std::string::npos, Ini.find(SecondKey));
		ContextGuard.reset();
		First->Drawing = {}; Second->Drawing = {};
	}

	TEST_F(FAssetEditorLifecycleTests, DefaultDefinitionCreatesAndReusesGenericEditAndViewEditors)
	{
		auto* Definition = NewObject<FGenericEditorTestDefinition>(nullptr, {});
		auto Registration = FAssetDefinitionRegistry::Get().RegisterAssetDefinition(*Definition);
		ASSERT_TRUE(Registration);
		auto* Object = Asset("GenericAsset");
		FSoftObjectPath Path;
		ASSERT_TRUE(FSoftObjectPath::TryCreate(Object->GetObjectPath(), Path));
		FAssetOpenArgs Args{.AssetEditors = AssetEditorSubsystem, .Assets = {{Path, Object->GetClass()->GetQualifiedName().ToString()}}, .OpenMethod = EAssetOpenMethod::View};
		const auto Flags = Object->GetObjectFlags();
		ASSERT_TRUE(AssetEditorSubsystem->OpenAssets(Args));
		auto* View = Durin::Testing::FindReflectedEditor(AssetEditorSubsystem, Object, EAssetOpenMethod::View);
		ASSERT_NE(nullptr, View);
		EXPECT_EQ("GenericAssetEditor", View->GetEditorName().ToString());
		EXPECT_EQ(Flags, Object->GetObjectFlags());
		EXPECT_FALSE(View->GetToolkit()->CanSaveActiveDocument());
		EXPECT_FALSE(View->GetToolkit()->SaveActiveDocument());
		EXPECT_TRUE(AssetEditorSubsystem->OpenAssets(Args));
		EXPECT_EQ(1u, Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).size());
		Args.OpenMethod = EAssetOpenMethod::Edit;
		ASSERT_TRUE(AssetEditorSubsystem->OpenAssets(Args));
		auto* Edit = Durin::Testing::FindReflectedEditor(AssetEditorSubsystem, Object, EAssetOpenMethod::Edit);
		ASSERT_NE(nullptr, Edit);
		EXPECT_NE(View, Edit);
		EXPECT_TRUE(Edit->GetToolkit()->CanSaveActiveDocument());
		EXPECT_TRUE(Edit->GetToolkit()->SaveActiveDocument());
		EXPECT_FALSE(Package->IsDirty());
		EXPECT_TRUE(Edit->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
		EXPECT_TRUE(View->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
	}

	TEST_F(FAssetEditorLifecycleTests, TransactionScopesRestoreTheirOwnerWithoutChangingTheProcessEditor)
	{
		const auto ProcessEditor = GEditor;
		const TStrongObjectPtr<DEditorEngine> Other(NewObject<DEditorEngine>(nullptr, {}));
		EXPECT_EQ(ProcessEditor, GetTransactionEditor());
		{
			FScopedEditorTransactionContext Scope(Other.Get());
			EXPECT_EQ(Other.Get(), GetTransactionEditor());
			EXPECT_EQ(ProcessEditor, GEditor);
			{
				FScopedEditorTransactionContext Nested(nullptr);
				EXPECT_EQ(nullptr, GetTransactionEditor());
			}
			EXPECT_EQ(Other.Get(), GetTransactionEditor());
		}
		EXPECT_EQ(ProcessEditor, GetTransactionEditor());
		FEditableAssetDocumentModel Documents;
		Documents.BindEditor(nullptr);
		EXPECT_EQ(nullptr, Documents.GetEditor());
		EXPECT_FALSE(Documents.CanUndo());
		MarkObjectHierarchyAsGarbage(Other.Get());
	}

	TEST_F(FAssetEditorLifecycleTests, SameTypeCreatesIndependentEditorsAndIndexesEachOnlyOnce)
	{
		auto* FirstAsset = Asset("First");
		auto* SecondAsset = Asset("Second");
		auto* First = Open({FirstAsset});
		auto* Second = Open({SecondAsset});
		ASSERT_NE(nullptr, First);
		ASSERT_NE(nullptr, Second);
		EXPECT_NE(First, Second);
		EXPECT_NE(First->GetDocumentManager(), Second->GetDocumentManager());
		EXPECT_NE(First->GetDocumentManager()->GetDocuments().front().Id, Second->GetDocumentManager()->GetDocuments().front().Id);
		EXPECT_EQ(2u, AssetEditorSubsystem->GetAllOpenEditors().size());
		EXPECT_EQ(First, AssetEditorSubsystem->FindEditorForAsset(FirstAsset, false));
		EXPECT_EQ(Second, AssetEditorSubsystem->FindEditorForAsset(SecondAsset, false));
		EXPECT_EQ(1u, AssetEditorSubsystem->FindEditorsForAsset(FirstAsset).size());
	}

	TEST_F(FAssetEditorLifecycleTests, GroupedAndSecondaryEditorsDoNotAssumeOneDocumentPerInstance)
	{
		auto* A = Asset("A");
		auto* B = Asset("B");
		auto* Primary = Open({A, B});
		ASSERT_NE(nullptr, Primary);
		EXPECT_EQ(1u, AssetEditorSubsystem->GetAllOpenEditors().size());
		EXPECT_EQ(2u, Primary->GetObjectsToEdit().size());
		EXPECT_EQ(Primary, AssetEditorSubsystem->FindEditorForAsset(A, false));
		EXPECT_EQ(Primary, AssetEditorSubsystem->FindEditorForAsset(B, false));
		auto* Secondary = Open({A}, EAssetOpenMethod::Edit, false);
		ASSERT_NE(nullptr, Secondary);
		EXPECT_EQ(2u, AssetEditorSubsystem->FindEditorsForAsset(A).size());
		EXPECT_EQ(Primary, AssetEditorSubsystem->FindEditorForAsset(A, true));
		EXPECT_EQ(Primary->GetToolkit(), AssetEditorSubsystem->GetActiveAssetToolkit());
	}

	TEST_F(FAssetEditorLifecycleTests, ViewGuardsCommandsAndDoesNotPromptForAnotherEditorsDirtyAsset)
	{
		auto* EditorInstance = Open({Asset("A")}, EAssetOpenMethod::View);
		ASSERT_NE(nullptr, EditorInstance);
		auto Toolkit = std::dynamic_pointer_cast<FLifecycleTestToolkit>(EditorInstance->GetToolkit());
		ASSERT_TRUE(Toolkit);
		Toolkit->bDirty = true;
		EXPECT_TRUE(Toolkit->IsReadOnly());
		EXPECT_FALSE(Toolkit->CanSaveActiveDocument());
		EXPECT_FALSE(Toolkit->SaveActiveDocument());
		EXPECT_FALSE(Toolkit->CanUndo());
		EXPECT_FALSE(Toolkit->Undo());
		EXPECT_FALSE(Toolkit->SaveDocument(EditorInstance->GetDocumentManager()->GetDocuments().front()));
		EXPECT_FALSE(Toolkit->DiscardDocument(EditorInstance->GetDocumentManager()->GetDocuments().front()));
		EXPECT_EQ(0, Toolkit->Saves);
		EXPECT_EQ(0, Toolkit->Undos);
		EXPECT_TRUE(EditorInstance->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
		EXPECT_TRUE(AssetEditorSubsystem->GetAllOpenEditors().empty());
	}

	TEST_F(FAssetEditorLifecycleTests, HeadlessEditorTickRefreshesDocumentsOnceAndClosesEmptyEditors)
	{
		auto* EditorInstance = Open({Asset("HeadlessRefresh")});
		ASSERT_NE(nullptr, EditorInstance);
		const auto Toolkit = std::dynamic_pointer_cast<FLifecycleTestToolkit>(EditorInstance->GetToolkit());
		const auto Documents = EditorInstance->GetDocumentManager();
		int Queries = 0;
		Toolkit->DirtyQuery = [&] { ++Queries; };
		Toolkit->bDirty = true;
		TickHost();
		EXPECT_EQ(1, Queries);
		ASSERT_EQ(1u, Documents->GetDocuments().size());
		EXPECT_TRUE(Documents->GetDocuments().front().bDirty);
		Toolkit->bDirty = false;
		TickHost();
		EXPECT_EQ(2, Queries);
		EXPECT_FALSE(Documents->GetDocuments().front().bDirty);
		ASSERT_EQ(EDocumentCloseResult::Closed, Documents->RequestCloseDocument(Documents->GetDocuments().front().Id));
		EXPECT_FALSE(EditorInstance->IsClosed());
		TickHost();
		EXPECT_TRUE(EditorInstance->IsClosed());
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
	}

	TEST_F(FAssetEditorLifecycleTests, ShellObservesRefreshedDocumentsWithoutAdvancingEditorsAgain)
	{
		class FEditorFrame final : public IMainFrameModule
		{
		public:
			std::function<void()> Observe;
			auto CreateDefaultMainFrame(DEditorEngine&, std::shared_ptr<MWindow>) -> void override {}
			auto DestroyMainFrame() -> void override {}
			auto LoadWorkspaceModules() -> bool override { return true; }
			auto InitializeWorkspaces(std::string&) -> bool override { return true; }
			auto TickMainFrame(float) -> void override { Observe(); }
		};
		auto* EditorInstance = Open({Asset("ShellRefresh")});
		ASSERT_NE(nullptr, EditorInstance);
		const auto Toolkit = std::dynamic_pointer_cast<FLifecycleTestToolkit>(EditorInstance->GetToolkit());
		const auto Documents = EditorInstance->GetDocumentManager();
		int Queries = 0;
		Toolkit->DirtyQuery = [&] { ++Queries; };
		Toolkit->bDirty = true;
		FEditorFrame Frame;
		int ShellTicks = 0;
		Frame.Observe = [&] {
			++ShellTicks;
			EXPECT_EQ(1, Queries);
			EXPECT_TRUE(Documents->GetDocuments().front().bDirty);
		};
		auto* Previous = FEditorEngineTestAccess::BindMainFrameModule(*Host, &Frame);
		TickHost();
		FEditorEngineTestAccess::BindMainFrameModule(*Host, Previous);
		EXPECT_EQ(1, Queries);
		EXPECT_EQ(1, ShellTicks);
		Toolkit->DirtyQuery = {};
	}

	TEST_F(FAssetEditorLifecycleTests, HeadlessRefreshShutdownStopsUpdatesUntilOwnerTeardown)
	{
		auto* First = Open({Asset("RefreshShutdown")});
		auto* Later = Open({Asset("AfterRefreshShutdown")});
		ASSERT_NE(nullptr, First); ASSERT_NE(nullptr, Later);
		const auto Toolkit = std::dynamic_pointer_cast<FLifecycleTestToolkit>(First->GetToolkit());
		const auto LaterToolkit = std::dynamic_pointer_cast<FLifecycleTestToolkit>(Later->GetToolkit());
		int LaterQueries = 0;
		Toolkit->bDirty = true;
		LaterToolkit->DirtyQuery = [&] { ++LaterQueries; };
		Toolkit->DirtyQuery = [&] {
			Host->RequestShutdown();
			Host->ForceGarbageCollection();
			EXPECT_EQ(AssetEditorSubsystem, Host->GetEditorSubsystem<DAssetEditorSubsystem>());
			EXPECT_FALSE(First->IsClosed());
		};
		TickHost();
		EXPECT_TRUE(Host->IsShutdownRequested());
		EXPECT_TRUE(IsGarbageCollectionRequested());
		EXPECT_EQ(0, LaterQueries);
		EXPECT_EQ(AssetEditorSubsystem, Host->GetEditorSubsystem<DAssetEditorSubsystem>());
		EXPECT_FALSE(First->IsClosed());
		EXPECT_TRUE(First->GetDocumentManager()->GetDocuments().front().bDirty); // The admitted refresh completed.
		Toolkit->DirtyQuery = {};
		LaterToolkit->DirtyQuery = {};
		Host->PreExit();
		EXPECT_TRUE(First->IsClosed());
		EXPECT_TRUE(Later->IsClosed());
	}

	TEST_F(FAssetEditorLifecycleTests, CloseCallbackCanCloseAnotherEditor)
	{
		auto* First = Open({Asset("ReentrantClose")});
		auto* Other = Open({Asset("OtherClose")});
		ASSERT_NE(nullptr, First); ASSERT_NE(nullptr, Other);
		const auto Toolkit = std::dynamic_pointer_cast<FLifecycleTestToolkit>(First->GetToolkit());
		Toolkit->CloseRequest = [&](const FEditorDocument&) {
			EXPECT_TRUE(Other->CloseWindow(EAssetEditorCloseReason::CloseOtherEditors));
			return EDocumentCloseResult::Closed;
		};
		EXPECT_TRUE(First->CloseWindow(EAssetEditorCloseReason::CloseAllAssetEditors));
		EXPECT_TRUE(First->IsClosed());
		EXPECT_TRUE(Other->IsClosed());
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
	}

	TEST_F(FAssetEditorLifecycleTests, HeadlessCloseShutdownCompletesCurrentCloseAndSkipsLaterEditors)
	{
		auto* First = Open({Asset("CloseShutdownA"), Asset("CloseShutdownB"), Asset("CloseShutdownC")});
		auto* Later = Open({Asset("AfterCloseShutdown")});
		ASSERT_NE(nullptr, First); ASSERT_NE(nullptr, Later);
		const auto Toolkit = std::dynamic_pointer_cast<FLifecycleTestToolkit>(First->GetToolkit());
		const auto LaterToolkit = std::dynamic_pointer_cast<FLifecycleTestToolkit>(Later->GetToolkit());
		Toolkit->bDirty = true;
		ASSERT_FALSE(First->CloseWindow(EAssetEditorCloseReason::CloseAllAssetEditors));
		const auto Documents = First->GetDocumentManager();
		ASSERT_EQ(EDocumentCloseResult::Closed, Documents->ResolvePendingDocumentClose(EDocumentCloseResponse::Discard));
		ASSERT_EQ(2u, Documents->GetDocuments().size());
		int CloseCalls = 0;
		int LaterQueries = 0;
		LaterToolkit->DirtyQuery = [&] { ++LaterQueries; };
		Toolkit->CloseRequest = [&](const FEditorDocument&) {
			++CloseCalls;
			Host->RequestShutdown();
			EXPECT_EQ(AssetEditorSubsystem, Host->GetEditorSubsystem<DAssetEditorSubsystem>());
			return EDocumentCloseResult::Closed;
		};
		TickHost();
		EXPECT_TRUE(Host->IsShutdownRequested());
		EXPECT_EQ(2, CloseCalls);
		EXPECT_TRUE(First->IsClosed());
		EXPECT_FALSE(Later->IsClosed());
		EXPECT_EQ(0, LaterQueries);
		EXPECT_EQ(AssetEditorSubsystem, Host->GetEditorSubsystem<DAssetEditorSubsystem>());
		LaterToolkit->DirtyQuery = {};
		Host->PreExit();
		EXPECT_TRUE(Later->IsClosed());
	}

	TEST_F(FAssetEditorLifecycleTests, ToolkitClosePublishesCapturedViewIdentityAndReleasesReflectedOwner)
	{
		auto* EditorInstance = Open({Asset("ViewClose")}, EAssetOpenMethod::View);
		ASSERT_NE(nullptr, EditorInstance);
		const TStrongObjectPtr<DAssetEditor> KeepAlive(EditorInstance);
		const auto Toolkit = EditorInstance->GetToolkit();
		const auto EditorKey = FObjectKey(EditorInstance);
		const std::string InstanceId(EditorInstance->GetInstanceId());
		int Closed = 0;
		auto Subscription = AssetEditorSubsystem->Subscribe([&](const FAssetEditorEvent& Event) {
			if (Event.Type != EAssetEditorEvent::Closed) return;
			++Closed;
			EXPECT_EQ(EditorKey, Event.Editor);
			EXPECT_EQ(InstanceId, Event.InstanceId);
			EXPECT_EQ(EAssetOpenMethod::View, Event.Method);
			EXPECT_TRUE(EditorInstance->IsClosed());
			EXPECT_EQ(nullptr, EditorInstance->GetToolkit());
			EXPECT_FALSE(FToolkitManager::Get().Contains(*Toolkit));
		});
		EXPECT_TRUE(Toolkit->CloseWindow(EAssetEditorCloseReason::CloseAllAssetEditors));
		EXPECT_TRUE(EditorInstance->CloseWindow(EAssetEditorCloseReason::CloseAllAssetEditors));
		EXPECT_EQ(1, Closed);
		EXPECT_TRUE(Toolkit->IsClosed());
		EXPECT_TRUE(AssetEditorSubsystem->GetAllOpenEditors().empty());
	}

	TEST_F(FAssetEditorLifecycleTests, DirectPendingCloseCancellationStopsWholeWindowContinuation)
	{
		auto Toolkit = std::make_shared<FReadOnlyDirectTestToolkit>();
		auto Documents = std::make_shared<FEditorDocumentManager>();
		std::vector<DObject*> Assets{Asset("DirectA"), Asset("DirectB")};
		ASSERT_TRUE(AssetEditorSubsystem->InitializeDirectToolkit(Toolkit, Documents, {.AssetEditors = AssetEditorSubsystem}, Assets));
		int Requests = 0;
		Toolkit->CloseRequest = [&](const FEditorDocument&) {
			++Requests; return EDocumentCloseResult::PendingConfirmation;
		};
		EXPECT_FALSE(Toolkit->CloseWindow(EAssetEditorCloseReason::CloseAllAssetEditors));
		EXPECT_EQ(1, Requests);
		(void)Documents->ResolvePendingDocumentClose(EDocumentCloseResponse::Cancel);
		TickHost();
		EXPECT_EQ(1, Requests);
		EXPECT_EQ(2u, Documents->GetDocuments().size());
		EXPECT_TRUE(FToolkitManager::Get().Contains(*Toolkit));
		Toolkit->CloseRequest = {};
		EXPECT_TRUE(Toolkit->CloseWindow(EAssetEditorCloseReason::CloseAllAssetEditors));
		EXPECT_TRUE(Toolkit->IsClosed());
		EXPECT_TRUE(AssetEditorSubsystem->GetAllOpenEditors().empty());
	}

	TEST_F(FAssetEditorLifecycleTests, FailedSaveRetainsPendingCloseAndEditorUntilResolution)
	{
		auto* EditorInstance = Open({Asset("A")});
		ASSERT_NE(nullptr, EditorInstance);
		auto Toolkit = std::dynamic_pointer_cast<FLifecycleTestToolkit>(EditorInstance->GetToolkit());
		Toolkit->bDirty = true;
		Toolkit->bSaveSucceeds = false;
		EXPECT_FALSE(EditorInstance->CloseWindow(EAssetEditorCloseReason::CloseAllAssetEditors));
		EXPECT_NE(nullptr, EditorInstance->GetDocumentManager()->GetPendingCloseDocument());
		const auto Documents = EditorInstance->GetDocumentManager();
		EXPECT_EQ(EDocumentCloseResult::PendingConfirmation,
			Documents->ResolvePendingDocumentClose(EDocumentCloseResponse::Save));
		EXPECT_EQ(1u, Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).size());
		EXPECT_EQ(1u, Documents->GetDocuments().size());
		Toolkit->bSaveSucceeds = true;
		EXPECT_EQ(EDocumentCloseResult::Closed, Documents->ResolvePendingDocumentClose(EDocumentCloseResponse::Save));
		TickHost();
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
		EXPECT_TRUE(AssetEditorSubsystem->GetAllOpenEditors().empty());
	}

	TEST_F(FAssetEditorLifecycleTests, GroupCloseContinuesAfterSaveAndStopsAfterCancel)
	{
		auto* EditorInstance = Open({Asset("GroupA"), Asset("GroupB")});
		ASSERT_NE(nullptr, EditorInstance);
		const auto Toolkit = std::dynamic_pointer_cast<FLifecycleTestToolkit>(EditorInstance->GetToolkit());
		const auto Documents = EditorInstance->GetDocumentManager();
		for (const auto& Document : Documents->GetDocuments()) Toolkit->DirtyDocuments.insert(Document.Id.Value);
		EXPECT_FALSE(EditorInstance->CloseWindow(EAssetEditorCloseReason::CloseAllAssetEditors));
		const auto Sequence = Documents->GetPendingCloseSequence();
		Toolkit->bSaveSucceeds = false;
		EXPECT_EQ(EDocumentCloseResult::PendingConfirmation, Documents->ResolvePendingDocumentClose(EDocumentCloseResponse::Save));
		TickHost();
		EXPECT_EQ(Sequence, Documents->GetPendingCloseSequence());
		Toolkit->bSaveSucceeds = true;
		EXPECT_EQ(EDocumentCloseResult::Closed, Documents->ResolvePendingDocumentClose(EDocumentCloseResponse::Save));
		TickHost();
		ASSERT_NE(nullptr, Documents->GetPendingCloseDocument());
		EXPECT_EQ(1u, Documents->GetDocuments().size());
		EXPECT_EQ(EDocumentCloseResult::Cancelled, Documents->ResolvePendingDocumentClose(EDocumentCloseResponse::Cancel));
		TickHost();
		EXPECT_EQ(nullptr, Documents->GetPendingCloseDocument());
		EXPECT_FALSE(EditorInstance->IsClosed());
		EXPECT_FALSE(EditorInstance->CloseWindow(EAssetEditorCloseReason::CloseAllAssetEditors));
		EXPECT_EQ(EDocumentCloseResult::Closed, Documents->ResolvePendingDocumentClose(EDocumentCloseResponse::Save));
		TickHost();
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
	}

	TEST_F(FAssetEditorLifecycleTests, CanonicalPrimarySelectionFollowsExplicitFocus)
	{
		auto* A = Asset("PrimaryA");
		auto* Group = Open({A, Asset("PrimaryB")});
		auto* Single = Open({A});
		ASSERT_NE(nullptr, Group); ASSERT_NE(nullptr, Single);
		ASSERT_NE(nullptr, Open({A}, EAssetOpenMethod::Edit, true, {}, "Inspection"));
		EXPECT_EQ(Single, Durin::Testing::FindReflectedEditor(AssetEditorSubsystem, A, EAssetOpenMethod::Edit, EToolkitMode::Standalone));
		Group->FocusWindow(A);
		EXPECT_EQ(Group, Durin::Testing::FindReflectedEditor(AssetEditorSubsystem, A, EAssetOpenMethod::Edit, EToolkitMode::Standalone));
		EXPECT_TRUE(AssetEditorSubsystem->OpenEditorForAsset(A));
		EXPECT_EQ(Group->GetToolkit(), AssetEditorSubsystem->GetActiveAssetToolkit());
	}

	TEST_F(FAssetEditorLifecycleTests, FailedInitializationPublishesNoEditorOrAssociation)
	{
		FLifecycleTestEditor::bFailCreation = true;
		EXPECT_EQ(nullptr, Open({Asset("A")}));
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
		EXPECT_TRUE(AssetEditorSubsystem->GetAllOpenEditors().empty());
	}

	TEST_F(FAssetEditorLifecycleTests, DrawCallbackCanCloseItsOwnEditorAndRetainedToolkitRejectsLaterFrames)
	{
		auto* EditorInstance = Open({Asset("SelfClosingFrame")});
		ASSERT_NE(nullptr, EditorInstance);
		auto Toolkit = std::dynamic_pointer_cast<FLifecycleTestToolkit>(EditorInstance->GetToolkit());
		int Draws = 0;
		Toolkit->Drawing = [&] {
			++Draws;
			EXPECT_TRUE(EditorInstance->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
			return true;
		};
		EXPECT_FALSE(EditorInstance->GetToolkit()->DrawToolkit(true));
		EXPECT_EQ(1, Draws);
		EXPECT_TRUE(Toolkit->IsClosed());
		EXPECT_FALSE(Toolkit->DrawToolkit(true));
		EXPECT_EQ(1, Draws);
	}

	TEST_F(FAssetEditorLifecycleTests, NonWindowHostRetainsPresentationWhileDirtyCloseIsPending)
	{
		const auto ToolkitHost = std::make_shared<FNonWindowToolkitHost>();
		auto* EditorInstance = Open({Asset("A")}, EAssetOpenMethod::Edit, true, ToolkitHost);
		ASSERT_NE(nullptr, EditorInstance);
		const auto Toolkit = std::dynamic_pointer_cast<FLifecycleTestToolkit>(EditorInstance->GetToolkit());
		Toolkit->bDirty = true;
		EXPECT_EQ(ToolkitHost, EditorInstance->GetToolkitHost());
		EXPECT_EQ(1u, Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).size());
		EXPECT_FALSE(ToolkitHost->RequestClose());
		EXPECT_TRUE(ToolkitHost->IsAvailable());
		EXPECT_FALSE(EditorInstance->IsClosed());
		EXPECT_EQ(EDocumentCloseResult::Closed,
			EditorInstance->GetDocumentManager()->ResolvePendingDocumentClose(EDocumentCloseResponse::Discard));
		TickHost();
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
		EXPECT_TRUE(ToolkitHost->RequestClose());
		EXPECT_FALSE(ToolkitHost->IsAvailable());
	}

	TEST_F(FAssetEditorLifecycleTests, FailedHostAttachmentPublishesNoInstanceOrToolkit)
	{
		class FRejectingHost : public FNonWindowToolkitHost
		{
		public:
			auto AttachToolkit(std::shared_ptr<FAssetEditorToolkit>) -> bool override { return false; }
		};
		const auto Escaped = std::make_shared<FLifecycleTestToolkit>();
		int Draws = 0;
		Escaped->Drawing = [&] { ++Draws; return true; };
		FLifecycleTestEditor::Factory = [Escaped] { return Escaped; };
		EXPECT_EQ(nullptr, Open({Asset("A")}, EAssetOpenMethod::Edit, true, std::make_shared<FRejectingHost>()));
		EXPECT_TRUE(Escaped->IsClosed());
		EXPECT_FALSE(Escaped->IsEditorOpen());
		EXPECT_FALSE(Escaped->SaveActiveDocument());
		EXPECT_FALSE(Escaped->Undo());
		EXPECT_FALSE(Escaped->DrawToolkit(true));
		EXPECT_EQ(0, Draws);
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
		EXPECT_TRUE(AssetEditorSubsystem->GetAllOpenEditors().empty());
	}

	TEST_F(FAssetEditorLifecycleTests, DeactivationVetoKeepsCurrentEditorAndRejectsNewEditorCreation)
	{
		auto* First = Open({Asset("First")});
		ASSERT_NE(nullptr, First);
		const auto Toolkit = std::dynamic_pointer_cast<FLifecycleTestToolkit>(First->GetToolkit());
		Toolkit->bCanDeactivate = false;
		EXPECT_EQ(nullptr, Open({Asset("Second")}));
		EXPECT_EQ(1u, Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).size());
		EXPECT_EQ(Toolkit, AssetEditorSubsystem->GetActiveAssetToolkit());
		Toolkit->bCanDeactivate = true;
	}

	TEST_F(FAssetEditorLifecycleTests, ShutdownDuringDeactivationCannotPublishAnotherEditor)
	{
		auto* First = Open({Asset("First")});
		ASSERT_NE(nullptr, First);
		const auto Toolkit = std::dynamic_pointer_cast<FLifecycleTestToolkit>(First->GetToolkit());
		bool Requested = false;
		Toolkit->Deactivate = [&] {
			if (!Requested)
			{
				Requested = true;
				Host->RequestShutdown();
				Host->ForceGarbageCollection();
				EXPECT_TRUE(IsGarbageCollectionRequested());
			}
			return true;
		};
		EXPECT_EQ(nullptr, Open({Asset("Second")}));
		EXPECT_TRUE(Requested);
		EXPECT_TRUE(Host->IsShutdownRequested());
		Host->PreExit();
		if (auto* Remaining = AssetEditorSubsystemLifetime.Get()) EXPECT_TRUE(Remaining->IsReadyForFinishDestroy());
	}

	TEST_F(FAssetEditorLifecycleTests, CloseConfirmationKeepsRequestOrderAcrossIndependentControllers)
	{
		auto* A = Open({Asset("A")});
		auto* B = Open({Asset("B")});
		ASSERT_NE(nullptr, A);
		ASSERT_NE(nullptr, B);
		std::dynamic_pointer_cast<FLifecycleTestToolkit>(A->GetToolkit())->bDirty = true;
		std::dynamic_pointer_cast<FLifecycleTestToolkit>(B->GetToolkit())->bDirty = true;
		const auto ADocuments = A->GetDocumentManager();
		const auto BDocuments = B->GetDocumentManager();
		EXPECT_FALSE(B->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
		EXPECT_FALSE(A->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
		const std::array<FEditorDocumentManager*, 2> Controllers{ADocuments.get(), BDocuments.get()};
		EXPECT_EQ(BDocuments.get(), DocumentUI::FindPendingClose(Controllers));
		EXPECT_EQ(EDocumentCloseResult::Cancelled, BDocuments->ResolvePendingDocumentClose(EDocumentCloseResponse::Cancel));
		EXPECT_EQ(ADocuments.get(), DocumentUI::FindPendingClose(Controllers));
		EXPECT_EQ(EDocumentCloseResult::Closed, ADocuments->ResolvePendingDocumentClose(EDocumentCloseResponse::Discard));
		EXPECT_EQ(nullptr, DocumentUI::FindPendingClose(Controllers));
	}

	TEST_F(FAssetEditorLifecycleTests, AdditionalEditorsRequireDistinctStableRoles)
	{
		auto* Object = Asset("A");
		auto* Primary = Open({Object});
		ASSERT_NE(nullptr, Primary);
		EXPECT_EQ(nullptr, Open({Object}));
		auto* Secondary = Open({Object}, EAssetOpenMethod::Edit, false);
		auto* Comparison = Open({Object}, EAssetOpenMethod::Edit, false, {}, "Comparison");
		ASSERT_NE(nullptr, Secondary);
		ASSERT_NE(nullptr, Comparison);
		EXPECT_NE(Secondary->GetToolkit()->GetPresentationRootKey("Editor"), Comparison->GetToolkit()->GetPresentationRootKey("Editor"));
		EXPECT_NE(Primary->GetWindowId(), Secondary->GetWindowId());
		EXPECT_EQ(Primary->GetToolkit()->GetLayoutProfileKey(), Secondary->GetToolkit()->GetLayoutProfileKey());
		auto* World = Open({Object}, EAssetOpenMethod::Edit, true, {}, {}, EToolkitMode::WorldCentric);
		auto* View = Open({Object}, EAssetOpenMethod::View);
		ASSERT_NE(nullptr, World);
		ASSERT_NE(nullptr, View);
		EXPECT_EQ(Primary->GetToolkit()->GetLayoutProfileKey(), World->GetToolkit()->GetLayoutProfileKey());
		EXPECT_NE(Primary->GetToolkit()->GetLayoutProfileKey(), View->GetToolkit()->GetLayoutProfileKey());
	}

	TEST_F(FAssetEditorLifecycleTests, ScopedTestHostClosesNewDirectToolkitsAndKeepsExistingOnes)
	{
		const auto Existing = std::make_shared<FReadOnlyDirectTestToolkit>();
		const std::vector<DObject*> ExistingAssets{Asset("ExistingDirectScope")};
		ASSERT_TRUE(AssetEditorSubsystem->InitializeDirectToolkit(Existing, std::make_shared<FEditorDocumentManager>(),
			{.AssetEditors = AssetEditorSubsystem}, ExistingAssets));
		const auto Created = std::make_shared<FReadOnlyDirectTestToolkit>();
		{
			Testing::FScopedAssetEditorTestHost Scope;
			ASSERT_EQ(Scope.GetAssetEditorSubsystem(), AssetEditorSubsystem);
			const std::vector<DObject*> CreatedAssets{Asset("CreatedDirectScope")};
			ASSERT_TRUE(AssetEditorSubsystem->InitializeDirectToolkit(Created, std::make_shared<FEditorDocumentManager>(),
				{.AssetEditors = AssetEditorSubsystem}, CreatedAssets));
			EXPECT_EQ(FToolkitManager::Get().GetToolkits(AssetEditorSubsystem).size(), 2u);
		}
		EXPECT_TRUE(Created->IsClosed());
		EXPECT_TRUE(Existing->IsEditorOpen());
		EXPECT_EQ(FToolkitManager::Get().GetToolkits(AssetEditorSubsystem).size(), 1u);
	}

	TEST_F(FAssetEditorLifecycleTests, DirectToolkitTransferPreservesIdentityAndRejectsDestinationCollision)
	{
		const std::vector<DObject*> Assets{Asset("DirectTransfer")};
		const auto First = std::make_shared<FReadOnlyDirectTestToolkit>();
		const auto Documents = std::make_shared<FEditorDocumentManager>();
		ASSERT_TRUE(AssetEditorSubsystem->InitializeDirectToolkit(First, Documents, {.AssetEditors = AssetEditorSubsystem}, Assets));
		const auto Source = First->GetToolkitHost();
		const std::string Identity(First->GetInstanceId());
		auto Destination = std::make_shared<FNonWindowToolkitHost>(EToolkitMode::WorldCentric);
		const auto Other = std::make_shared<FReadOnlyDirectTestToolkit>();
		ASSERT_TRUE(AssetEditorSubsystem->InitializeDirectToolkit(Other, std::make_shared<FEditorDocumentManager>(),
			{.AssetEditors = AssetEditorSubsystem, .ToolkitMode = EToolkitMode::WorldCentric, .Host = Destination}, Assets));
		EXPECT_FALSE(First->TransferToHost(Destination));
		EXPECT_EQ(First->GetToolkitHost(), Source);
		FToolkitManager::Get().CloseToolkit(*Other);
		ASSERT_TRUE(First->TransferToHost(Destination));
		EXPECT_EQ(First->GetToolkitMode(), EToolkitMode::WorldCentric);
		EXPECT_EQ(First->GetDocumentManager(), Documents);
		EXPECT_EQ(First->GetInstanceId(), Identity);
		EXPECT_EQ(FToolkitManager::Get().GetToolkits(AssetEditorSubsystem).size(), 1u);
		EXPECT_EQ(AssetEditorSubsystem->GetAllOpenEditors().front(), First.get());
		Destination.reset();
		EXPECT_TRUE(First->IsClosed());
		EXPECT_TRUE(FToolkitManager::Get().GetToolkits(AssetEditorSubsystem).empty());
	}

	TEST_F(FAssetEditorLifecycleTests, DirectToolkitTransferHonorsAssetDefinitionModePolicy)
	{
		auto* Definition = NewObject<FGenericEditorTestDefinition>(nullptr, {});
		Definition->RequiredMode = EToolkitMode::WorldCentric;
		auto Registration = FAssetDefinitionRegistry::Get().RegisterAssetDefinition(*Definition);
		ASSERT_TRUE(Registration);
		const auto Toolkit = std::make_shared<FReadOnlyDirectTestToolkit>();
		const std::vector<DObject*> Assets{Asset("DirectTransferPolicy")};
		ASSERT_TRUE(AssetEditorSubsystem->InitializeDirectToolkit(Toolkit, std::make_shared<FEditorDocumentManager>(),
			{.AssetEditors = AssetEditorSubsystem}, Assets));
		const auto Source = Toolkit->GetToolkitHost();
		const auto Standalone = std::make_shared<FNonWindowToolkitHost>();
		EXPECT_FALSE(Toolkit->TransferToHost(Standalone));
		EXPECT_EQ(Toolkit->GetToolkitMode(), EToolkitMode::WorldCentric);
		EXPECT_EQ(Toolkit->GetToolkitHost(), Source);
		EXPECT_TRUE(Toolkit->IsEditorOpen());
	}

	TEST_F(FAssetEditorLifecycleTests, CloseFromHostChangedCallbackMakesTransferFailWithoutReopening)
	{
		const auto Toolkit = std::make_shared<FReadOnlyDirectTestToolkit>();
		const std::vector<DObject*> Assets{Asset("TransferCallbackClose")};
		ASSERT_TRUE(AssetEditorSubsystem->InitializeDirectToolkit(Toolkit, std::make_shared<FEditorDocumentManager>(),
			{.AssetEditors = AssetEditorSubsystem}, Assets));
		const auto Destination = std::make_shared<FNonWindowToolkitHost>(EToolkitMode::WorldCentric);
		int Changes = 0;
		Toolkit->HostChanged = [&] {
			++Changes;
			EXPECT_TRUE(Toolkit->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
		};
		EXPECT_FALSE(Toolkit->TransferToHost(Destination));
		EXPECT_EQ(Changes, 1);
		EXPECT_TRUE(Toolkit->IsClosed());
		EXPECT_EQ(Toolkit->GetToolkitHost(), nullptr);
		EXPECT_TRUE(FToolkitManager::Get().GetToolkits(AssetEditorSubsystem).empty());
		Toolkit->HostChanged = {};
	}

	TEST_F(FAssetEditorLifecycleTests, CloseFromOldHostDetachCancelsTransferAndReleasesDestination)
	{
		class FClosingHost : public FNonWindowToolkitHost
		{
		public:
			auto DetachToolkit(FAssetEditorToolkit& Toolkit) -> void override
			{
				if (!std::exchange(bClosing, true))
					EXPECT_TRUE(Toolkit.CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
				FNonWindowToolkitHost::DetachToolkit(Toolkit);
			}
			bool bClosing = false;
		};
		class FDestinationHost : public FNonWindowToolkitHost
		{
		public:
			FDestinationHost() : FNonWindowToolkitHost(EToolkitMode::WorldCentric) {}
			auto DetachToolkit(FAssetEditorToolkit& Toolkit) -> void override
			{ ++Detaches; FNonWindowToolkitHost::DetachToolkit(Toolkit); }
			int Detaches = 0;
		};
		const auto SourceHost = std::make_shared<FClosingHost>();
		auto* Editor = Open({Asset("TransferClose")}, EAssetOpenMethod::Edit, true, SourceHost);
		ASSERT_NE(Editor, nullptr);
		const auto Toolkit = Editor->GetToolkit();
		const auto Destination = std::make_shared<FDestinationHost>();
		EXPECT_FALSE(Editor->GetToolkit()->TransferToHost(Destination));
		EXPECT_TRUE(Toolkit->IsClosed());
		EXPECT_EQ(Toolkit->GetToolkitHost(), nullptr);
		EXPECT_EQ(Destination->Detaches, 1);
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
	}

	TEST_F(FAssetEditorLifecycleTests, HostTransferPreservesEditorAndRejectsDestinationIdentityCollision)
	{
		auto* Object = Asset("A");
		auto* EditorInstance = Open({Object});
		ASSERT_NE(nullptr, EditorInstance);
		const auto Toolkit = EditorInstance->GetToolkit();
		const auto Documents = EditorInstance->GetDocumentManager();
		const auto OriginalKey = Toolkit->GetPresentationRootKey("Editor");
		const auto WorldHost = std::make_shared<FNonWindowToolkitHost>(EToolkitMode::WorldCentric);
		ASSERT_TRUE(EditorInstance->GetToolkit()->TransferToHost(WorldHost));
		EXPECT_EQ(EToolkitMode::WorldCentric, EditorInstance->GetToolkitMode());
		EXPECT_EQ(WorldHost, EditorInstance->GetToolkitHost());
		EXPECT_EQ(Toolkit, EditorInstance->GetToolkit());
		EXPECT_EQ(Documents, EditorInstance->GetDocumentManager());
		EXPECT_EQ(OriginalKey, Toolkit->GetPresentationRootKey("Editor"));
		auto* Other = Open({Object});
		ASSERT_NE(nullptr, Other);
		EXPECT_FALSE(EditorInstance->GetToolkit()->TransferToHost(Other->GetToolkitHost()));
		EXPECT_EQ(WorldHost, EditorInstance->GetToolkitHost());
	}

	TEST_F(FAssetEditorLifecycleTests, NamedDockTemplateRestoresIntoIndependentRuntimeWindows)
	{
		auto* A = Open({Asset("DockTemplateA")}); auto* B = Open({Asset("DockTemplateB")});
		ASSERT_NE(nullptr, A); ASSERT_NE(nullptr, B);
		const auto First = std::dynamic_pointer_cast<FLifecycleTestToolkit>(A->GetToolkit());
		const auto Second = std::dynamic_pointer_cast<FLifecycleTestToolkit>(B->GetToolkit());
		ASSERT_EQ(First->GetLayoutProfileKey(), Second->GetLayoutProfileKey());
		const FAssetEditorToolkitId TypeA(First->GetPresentationRootKey("Panels"));
		const FAssetEditorToolkitId TypeB(Second->GetPresentationRootKey("Panels"));
		const std::string_view Panels[]{"Graph", "Details"};
		auto NewContext = [] {
			ImGui::CreateContext(); auto& IO = ImGui::GetIO(); IO.IniFilename = nullptr;
			IO.DisplaySize = ImVec2(1200, 800); IO.DeltaTime = 1.0f / 60;
			IO.ConfigFlags |= ImGuiConfigFlags_DockingEnable; IO.Fonts->Build();
		};
		NewContext();
		struct FContext { ~FContext() { if (ImGui::GetCurrentContext()) ImGui::DestroyContext(); } } Guard;
		A->FocusWindow();
		for (int Frame = 0; Frame < 3; ++Frame)
		{
			ImGui::NewFrame(); ImGui::Begin("Template host", nullptr, ImGuiWindowFlags_NoSavedSettings);
			const auto Root = TabLayout::MakeDockSpaceId(TypeA, 1);
			if (!ImGui::DockBuilderGetNode(Root))
			{
				ImGui::DockBuilderAddNode(Root, ImGuiDockNodeFlags_DockSpace);
				ImGui::DockBuilderSetNodeSize(Root, ImVec2(800, 600));
				ImGuiID Left, Right; ImGui::DockBuilderSplitNode(Root, ImGuiDir_Left, .3f, &Left, &Right);
				ImGui::DockBuilderDockWindow(TabLayout::MakePanelWindowName("", TypeA, "Graph").c_str(), Left);
				ImGui::DockBuilderDockWindow(TabLayout::MakePanelWindowName("", TypeA, "Details").c_str(), Right);
				ImGui::DockBuilderFinish(Root);
			}
			TabLayout::SubmitDockSpace(TypeA, 1, ImVec2(800, 600));
			for (auto Panel : Panels) { TabLayout::BeginDockablePanel(TypeA, Panel, Panel, nullptr, ImGuiWindowFlags_NoSavedSettings); ImGui::End(); }
			First->SaveDockLayout("Document", TypeA, 1, Panels);
			Second->RestoreDockLayout("Document", TypeB, 1, Panels);
			ImGui::End(); ImGui::Render();
			EXPECT_EQ(0, ImGui::GetCurrentContext()->ErrorCountCurrentFrame);
		}
		const auto* RootA = ImGui::DockBuilderGetNode(TabLayout::MakeDockSpaceId(TypeA, 1));
		const auto* RootB = ImGui::DockBuilderGetNode(TabLayout::MakeDockSpaceId(TypeB, 1));
		ASSERT_NE(nullptr, RootA); ASSERT_NE(nullptr, RootB);
		ASSERT_NE(nullptr, RootB->ChildNodes[0]); EXPECT_NE(RootA->ChildNodes[0]->ID, RootB->ChildNodes[0]->ID);
		const std::string Ini = ImGui::SaveIniSettingsToMemory();
		SCOPED_TRACE(Ini);
		EXPECT_EQ(std::string::npos, Ini.find(".Window."));
		EXPECT_NE(std::string::npos, Ini.find(First->GetLayoutProfileKey()));
		ImGui::DestroyContext(); NewContext(); ImGui::LoadIniSettingsFromMemory(Ini.c_str(), Ini.size());
		ImGui::NewFrame(); ImGui::Begin("Template restore", nullptr, ImGuiWindowFlags_NoSavedSettings);
		Second->RestoreDockLayout("Document", TypeB, 1, Panels);
		const auto* Restored = ImGui::DockBuilderGetNode(TabLayout::MakeDockSpaceId(TypeB, 1));
		ASSERT_NE(nullptr, Restored); ASSERT_NE(nullptr, Restored->ChildNodes[0]);
		EXPECT_EQ(ImGuiAxis_X, Restored->SplitAxis);
		ImGui::End(); ImGui::Render();
	}


}
