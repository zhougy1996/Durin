#include "Toolkits/AssetEditor.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/ToolkitHost.h"
#include "Toolkits/ToolkitManager.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Documents/EditorDocumentManager.h"
#include "Documents/EditorDocumentUI.h"
#include "Editor/EditorEngineTestSupport.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
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
	class FSessionTestToolkit : public FBaseAssetToolkit
	{
	public:
		explicit FSessionTestToolkit(bool bViewport = false, bool bDetails = false) : FBaseAssetToolkit(bViewport, bDetails) {}
		auto GetToolkitId() const -> const FAssetEditorToolkitId& override { return Type; }
		auto OpenDocument(const FEditorDocument&) -> EDocumentOpenResult override { return bDeferOpen ? EDocumentOpenResult::Deferred : EDocumentOpenResult::Opened; }
		auto ActivateDocument(const FEditorDocument&) -> void override {}
		auto OnDrawToolkit(bool) -> bool override { return Drawing ? Drawing() : false; }
		auto ResetLayout() -> void override {}
		auto RequestDeactivate() -> bool override { return Deactivate ? Deactivate() : bCanDeactivate; }
		auto RequestCloseDocument(const FEditorDocument& Document) -> EDocumentCloseResult override
		{ return IsDocumentDirty(Document) ? EDocumentCloseResult::PendingConfirmation : EDocumentCloseResult::Closed; }
		auto OnIsDocumentDirty(const FEditorDocument& Document) const -> bool override { return bDirty || DirtyDocuments.contains(Document.Id.Value); }
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
		std::function<bool()> Drawing;
		int Saves = 0;
		int Undos = 0;
		FAssetEditorToolkitId Type{"SessionTest"};
	};

	class FSessionTestEditor : public DAssetEditor
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FSessionTestEditor, DAssetEditor, FSessionTestEditor::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FSessionTestEditor)
	public:
		explicit FSessionTestEditor(const FObjectInitializer& Initializer) : Super(Initializer) {}
		auto CreateToolkit() -> std::shared_ptr<FAssetEditorToolkit> override
		{ return bFailCreation ? nullptr : Factory ? Factory() : std::make_shared<FSessionTestToolkit>(); }
		inline static bool bFailCreation = false;
		inline static std::function<std::shared_ptr<FAssetEditorToolkit>()> Factory;
	};
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FSessionTestEditor)

	class FGenericSessionTestDefinition : public DAssetDefinitionDefault
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FGenericSessionTestDefinition, DAssetDefinitionDefault, FGenericSessionTestDefinition::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FGenericSessionTestDefinition)
	public:
		explicit FGenericSessionTestDefinition(const FObjectInitializer& Initializer) : Super(Initializer) {}
		auto GetAssetClassPath() const -> std::string override { return AssetClassPath; }
		auto GetAssetEditorClass() const -> DClass* override { return EditorClassOverride ? EditorClassOverride : Super::GetAssetEditorClass(); }
		std::string AssetClassPath = DObject::StaticClass()->GetQualifiedName().ToString();
		DClass* EditorClassOverride = nullptr;
		auto CanRegisterStatically() const -> bool override { return false; }
		auto OpenAssets(const FAssetOpenArgs& Args) const -> FAssetOpenResult override
		{ return OpenOverride ? OpenOverride(Args) : Super::OpenAssets(Args); }
		auto GetAssetOpenSupport(const FAssetOpenSupportArgs& Args) const -> FAssetOpenSupport override
		{ auto Support = Super::GetAssetOpenSupport(Args); Support.RequiredToolkitMode = RequiredMode; return Support; }
		std::optional<EToolkitMode> RequiredMode;
		std::function<FAssetOpenResult(const FAssetOpenArgs&)> OpenOverride;
	};
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FGenericSessionTestDefinition)
}

namespace
{
	using namespace Durin;
	class FAssetEditorSessionTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			Testing::InitializeDObjectSystemForTests();
			Mounts = std::make_unique<Testing::FScopedMountRegistryFixture>();
			Testing::RegisterMountPointForTests("/AssetEditorSessions/", Testing::GetTestWorkDirectory().generic_string() + "/");
			Host = NewObject<DEditorEngine>(nullptr, {});
			ASSERT_NO_THROW(FEditorEngineTestAccess::StartEditor(*Host));
			Service = Host->GetEditorSubsystem<DAssetEditorSubsystem>();
			ServiceLifetime = Service;
			FPackagePath Path;
			ASSERT_TRUE(FPackagePath::TryCreate("/AssetEditorSessions/Package", Path));
			Package = CreatePackage(Path);
		}
		auto TearDown() -> void override
		{
			FSessionTestEditor::bFailCreation = false;
			FSessionTestEditor::Factory = {};
			const auto Remaining = ServiceLifetime.IsValid() ? ServiceLifetime.Get()->GetOwnedAssetEditors()
				: std::vector<TStrongObjectPtr<DAssetEditor>>{};
			for (const auto& Session : Remaining)
			{
				if (auto Toolkit = std::dynamic_pointer_cast<FSessionTestToolkit>(Session->GetToolkit())) { Toolkit->bDirty = false; Toolkit->DirtyDocuments.clear(); }
				(void)Session->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed);
			}
			Host->PreExit();
			MarkObjectHierarchyAsGarbage(Host.Get());
			Host.Reset();
			if (Package) MarkObjectHierarchyAsGarbage(Package.Get());
			Package.Reset();
			CollectGarbage();
			Mounts.reset();
		}
		auto Asset(const char* Name) -> DObject* { return NewObject<DObject>(Package.Get(), Name); }
		auto Open(std::vector<DObject*> Assets, EAssetOpenMethod Method = EAssetOpenMethod::Edit,
			bool bPrimary = true, std::shared_ptr<IToolkitHost> ToolkitHost = {}, std::string Role = {},
			EToolkitMode Mode = EToolkitMode::Standalone) -> DAssetEditor*
		{
			return Service->OpenAssetEditor(FSessionTestEditor::StaticClass(), {
				.OpenArgs = {.AssetEditors = Service, .OpenMethod = Method, .ToolkitMode = Mode, .Host = std::move(ToolkitHost)},
				.Assets = std::move(Assets), .bPrimary = bPrimary, .InstanceKey = std::move(Role)});
		}
		std::unique_ptr<Testing::FScopedMountRegistryFixture> Mounts;
		TStrongObjectPtr<DEditorEngine> Host;
		TStrongObjectPtr<DPackage> Package;
		DAssetEditorSubsystem* Service = nullptr;
		TWeakObjectPtr<DAssetEditorSubsystem> ServiceLifetime;
	};

	TEST_F(FAssetEditorSessionTests, LifecyclePublishesCommittedAssociationsAndAllowsReentrantClose)
	{
		std::vector<EAssetEditorEvent> Events;
		DAssetEditor* Other = Open({Asset("OtherLifecycle")});
		ASSERT_NE(nullptr, Other);
		auto Subscription = Service->Subscribe([&](const FAssetEditorEvent& Event) {
			Events.push_back(Event.Type);
			if (Event.Type == EAssetEditorEvent::Opened && Other) { auto* Closing = std::exchange(Other, nullptr); Closing->CloseWindow(EAssetEditorCloseReason::CloseAllAssetEditors); }
			if (Event.Type == EAssetEditorEvent::AssetRemoved)
				EXPECT_TRUE(Service->FindEditorsForAsset(Event.Target.AssetPath.IsValid() ? FAssetOpenArgs{}.LoadAsset(Event.Target) : nullptr).empty());
		});
		auto* Session = Open({Asset("Lifecycle")});
		ASSERT_NE(nullptr, Session);
		const auto Opened = Service->GetUsage().Opened;
		Session->SynchronizeAssets();
		EXPECT_EQ(Opened, Service->GetUsage().Opened);
		EXPECT_EQ(1u, Service->GetOwnedAssetEditors().size());
		EXPECT_EQ(EAssetEditorEvent::Opened, Events.front());
		EXPECT_EQ(1u, Service->GetUsage().Closed);
		Subscription.Reset();
		const auto Count = Events.size();
		Session->CloseWindow(EAssetEditorCloseReason::CloseAllAssetEditors);
		EXPECT_EQ(Count, Events.size());
	}

	TEST_F(FAssetEditorSessionTests, GroupDispatchCreatesOnceAndOrdinaryBatchKeepsIndividualSessions)
	{
		auto* Definition = NewObject<FGenericSessionTestDefinition>(nullptr, {});
		auto Registration = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition);
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
		ASSERT_TRUE(Service->OpenAssets({.AssetEditors = Service, .Assets = Targets, .bCreateSingleSession = true}).IsAccepted());
		EXPECT_EQ(1, Calls);
		ASSERT_EQ(1u, Service->GetOwnedAssetEditors().size());
		EXPECT_EQ(2u, Service->GetOwnedAssetEditors().front()->GetObjectsToEdit().size());
		ASSERT_TRUE(Service->CloseAllAssetEditors());
		ASSERT_TRUE(Service->OpenAssets({.AssetEditors = Service, .Assets = Targets}).IsAccepted());
		EXPECT_EQ(2, Calls);
		EXPECT_EQ(2u, Service->GetOwnedAssetEditors().size());
	}

	TEST_F(FAssetEditorSessionTests, MixedDefinitionGroupExecutesOnceAndInvalidGroupsPublishNothing)
	{
		auto* First = NewObject<FGenericSessionTestDefinition>(nullptr, {});
		auto* Second = NewObject<FGenericSessionTestDefinition>(nullptr, {});
		Second->AssetClassPath = FSessionTestEditor::StaticClass()->GetQualifiedName().ToString();
		auto RegistrationA = FAssetDefinitionRegistry::Get().RegisterDefinition(*First);
		auto RegistrationB = FAssetDefinitionRegistry::Get().RegisterDefinition(*Second);
		ASSERT_TRUE(RegistrationA); ASSERT_TRUE(RegistrationB);
		auto* A = Asset("MixedA");
		auto* B = NewObject<FSessionTestEditor>(Package.Get(), "MixedB");
		FSoftObjectPath PathA, PathB, Missing;
		ASSERT_TRUE(FSoftObjectPath::TryCreate(A->GetObjectPath(), PathA));
		ASSERT_TRUE(FSoftObjectPath::TryCreate(B->GetObjectPath(), PathB));
		ASSERT_TRUE(FSoftObjectPath::TryCreate("/AssetEditorSessions/Missing.Asset", Missing));
		int Calls = 0;
		First->OpenOverride = [&](const FAssetOpenArgs& Args) { ++Calls; return First->DAssetDefinitionDefault::OpenAssets(Args); };
		Second->OpenOverride = [&](const FAssetOpenArgs&) { ADD_FAILURE() << "Grouped selection must execute once"; return FAssetOpenResult{}; };
		FAssetOpenArgs Args{.AssetEditors = Service, .Assets = {{PathA, First->AssetClassPath}, {PathB, Second->AssetClassPath}}, .bCreateSingleSession = true};
		ASSERT_TRUE(Service->OpenAssets(Args).IsAccepted());
		EXPECT_EQ(1, Calls); EXPECT_EQ(1u, Service->GetOwnedAssetEditors().size());
		ASSERT_TRUE(Service->CloseAllAssetEditors());
		Second->EditorClassOverride = FSessionTestEditor::StaticClass();
		EXPECT_FALSE(Service->OpenAssets(Args).IsAccepted());
		EXPECT_TRUE(Service->GetOwnedAssetEditors().empty());
		Args.Assets[1] = {Missing, First->AssetClassPath};
		const auto MissingResult = Service->OpenAssets(Args);
		ASSERT_EQ(2u, MissingResult.Items.size());
		for (const auto& Item : MissingResult.Items) EXPECT_EQ(EAssetOpenStatus::Failed, Item.Status);
		EXPECT_TRUE(Service->GetOwnedAssetEditors().empty());
	}

	TEST_F(FAssetEditorSessionTests, ConfiguredSessionInitializationAndSharedHostHaveOneOwner)
	{
		const auto SharedHost = std::make_shared<FNonWindowToolkitHost>(EToolkitMode::WorldCentric);
		Service->SetToolkitHostFactory([&](const FAssetOpenArgs&) { return SharedHost; });
		auto* A = NewObject<FSessionTestEditor>(Host.Get(), {});
		FAssetEditorInitArgs Args{.OpenArgs = {.AssetEditors = Service, .ToolkitMode = EToolkitMode::WorldCentric}, .Assets = {Asset("OwnerA")}};
		ASSERT_EQ(A, Service->InitializeAssetEditor(*A, Args));
		EXPECT_EQ(nullptr, Service->InitializeAssetEditor(*A, Args));
		auto* B = Open({Asset("OwnerB")}, EAssetOpenMethod::Edit, true, {}, {}, EToolkitMode::WorldCentric);
		ASSERT_NE(nullptr, B);
		EXPECT_EQ(A->GetToolkitHost(), B->GetToolkitHost());
		const auto ToolkitB = B->GetToolkit();
		ASSERT_TRUE(A->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
		EXPECT_EQ(SharedHost, Service->GetToolkitManager().GetHost(*ToolkitB));
		EXPECT_TRUE(SharedHost->IsAvailable()); EXPECT_FALSE(B->IsClosed());
		auto* Foreign = NewObject<FSessionTestEditor>(nullptr, {});
		EXPECT_EQ(nullptr, Service->InitializeAssetEditor(*Foreign, Args));
	}

	TEST_F(FAssetEditorSessionTests, DirectAdmissionResolvesDefinitionRequirementsBeforeCreatingToolkit)
	{
		auto* Definition = NewObject<FGenericSessionTestDefinition>(nullptr, {});
		Definition->RequiredMode = EToolkitMode::WorldCentric;
		auto Registration = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition);
		int Created = 0;
		FSessionTestEditor::Factory = [&] { ++Created; return std::make_shared<FSessionTestToolkit>(); };
		auto* A = Asset("RequiredHost");
		EXPECT_EQ(nullptr, Open({A}, EAssetOpenMethod::Edit, true, std::make_shared<FNonWindowToolkitHost>()));
		EXPECT_EQ(0, Created);
		auto* Session = Open({A}); ASSERT_NE(nullptr, Session);
		EXPECT_EQ(EToolkitMode::WorldCentric, Session->GetToolkitMode()); EXPECT_EQ(1, Created);
		EXPECT_FALSE(Session->TransferToHost(std::make_shared<FNonWindowToolkitHost>()));
		EXPECT_EQ(EToolkitMode::WorldCentric, Session->GetToolkitMode());
		EXPECT_EQ(nullptr, Open({Asset("InvalidMethod")}, static_cast<EAssetOpenMethod>(255)));
		EXPECT_EQ(1, Created);
	}

	TEST_F(FAssetEditorSessionTests, RestoreRoundTripsGroupsRolesViewAndReportsMissingItems)
	{
		auto* Definition = NewObject<FGenericSessionTestDefinition>(nullptr, {});
		auto Registration = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition);
		ASSERT_TRUE(Registration);
		auto* A = Asset("RestoreA"); auto* B = Asset("RestoreB");
		FSoftObjectPath PathA, PathB, MissingPath;
		ASSERT_TRUE(FSoftObjectPath::TryCreate(A->GetObjectPath(), PathA));
		ASSERT_TRUE(FSoftObjectPath::TryCreate(B->GetObjectPath(), PathB));
		ASSERT_TRUE(FSoftObjectPath::TryCreate("/AssetEditorSessions/Missing.Missing", MissingPath));
		const FAssetOpenTarget TargetA{PathA, A->GetClass()->GetQualifiedName().ToString()};
		const FAssetOpenTarget TargetB{PathB, B->GetClass()->GetQualifiedName().ToString()};
		const std::vector<FAssetEditorRestoreEntry> Entries{{.Assets = {TargetA, TargetB}, .Method = EAssetOpenMethod::View,
			.Mode = EToolkitMode::WorldCentric, .Role = "RestoreInspection", .bPrimary = false}};
		ASSERT_TRUE(Service->RestoreSessions(Entries).IsAccepted());
		ASSERT_EQ(1u, Service->GetOwnedAssetEditors().size());
		EXPECT_EQ(2u, Service->GetOwnedAssetEditors().front()->GetObjectsToEdit().size());
		const auto Path = (Testing::GetTestWorkDirectory() / "AssetEditorRestore.yaml").generic_string();
		ASSERT_TRUE(Service->SaveSessionState(Path));
		ASSERT_TRUE(Service->CloseAllAssetEditors());
		ASSERT_TRUE(Service->LoadSessionState(Path).IsAccepted());
		const auto Restored = Service->CaptureSessions();
		ASSERT_EQ(1u, Restored.size());
		EXPECT_EQ(2u, Restored.front().Assets.size());
		EXPECT_EQ(EAssetOpenMethod::View, Restored.front().Method);
		EXPECT_EQ(EToolkitMode::WorldCentric, Restored.front().Mode);
		EXPECT_EQ("RestoreInspection", Restored.front().Role);
		EXPECT_FALSE(Restored.front().bPrimary);
		const std::vector<FAssetEditorRestoreEntry> Mixed{{.Assets = {TargetA}},
			{.Assets = {{MissingPath, TargetA.AssetClassName}}}};
		const auto Result = Service->RestoreSessions(Mixed);
		ASSERT_EQ(2u, Result.Items.size());
		EXPECT_EQ(EAssetOpenStatus::Completed, Result.Items.front().Status);
		EXPECT_EQ(EAssetOpenStatus::Failed, Result.Items.back().Status);
		EXPECT_TRUE(Service->CloseAllAssetEditors());
		Registration.Reset();
		const auto MissingProvider = Service->LoadSessionState(Path);
		ASSERT_EQ(2u, MissingProvider.Items.size());
		EXPECT_FALSE(MissingProvider.IsAccepted());
		EXPECT_TRUE(Service->GetOwnedAssetEditors().empty());
		FYamlDocument Snapshot;
		ASSERT_TRUE(Snapshot.LoadFromFile(Path));
		Snapshot.GetMutableRoot().SetChildValue("Project", "AnotherProject.dproject");
		ASSERT_TRUE(Snapshot.SaveToFile(Path));
		const auto WrongProject = Service->LoadSessionState(Path);
		ASSERT_EQ(1u, WrongProject.Items.size());
		EXPECT_EQ(EAssetOpenStatus::Rejected, WrongProject.Items.front().Status);
	}

	TEST_F(FAssetEditorSessionTests, RestoreFocusWaitsForPresentationAndExplicitFocusCancelsIt)
	{
		class FWaitingHost : public FNonWindowToolkitHost
		{
		public:
			explicit FWaitingHost(EToolkitMode Mode) : FNonWindowToolkitHost(Mode) {}
			auto IsReadyForFocus() const -> bool override { return Ready; }
			bool Ready = false;
		};
		auto* Definition = NewObject<FGenericSessionTestDefinition>(nullptr, {});
		auto Registration = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition);
		ASSERT_TRUE(Registration);
		const auto Standalone = std::make_shared<FWaitingHost>(EToolkitMode::Standalone);
		const auto World = std::make_shared<FWaitingHost>(EToolkitMode::WorldCentric);
		Service->SetToolkitHostFactory([&](const FAssetOpenArgs& Args) -> std::shared_ptr<IToolkitHost> {
			return Args.ToolkitMode == EToolkitMode::Standalone ? Standalone : World;
		});
		auto* Object = Asset("RestoreFocus");
		FSoftObjectPath Path; ASSERT_TRUE(FSoftObjectPath::TryCreate(Object->GetObjectPath(), Path));
		const FAssetOpenTarget Target{Path, Object->GetClass()->GetQualifiedName().ToString()};
		const std::vector<FAssetEditorRestoreEntry> Entries{{.Assets = {Target}},
			{.Assets = {Target}, .Method = EAssetOpenMethod::View, .Mode = EToolkitMode::WorldCentric}};
		ASSERT_TRUE(Service->RestoreSessions(Entries).IsAccepted());
		auto* Edit = Service->FindAssetEditor(Object, EAssetOpenMethod::Edit);
		auto* View = Service->FindAssetEditor(Object, EAssetOpenMethod::View, EToolkitMode::WorldCentric);
		ASSERT_NE(nullptr, Edit); ASSERT_NE(nullptr, View);
		ASSERT_TRUE(Service->ActivateAssetEditor(*Edit));
		Service->TickAssetEditors();
		EXPECT_EQ(Edit->GetToolkit(), Service->GetActiveAssetToolkit());
		Standalone->Ready = World->Ready = true;
		Service->TickAssetEditors();
		EXPECT_EQ(View->GetToolkit(), Service->GetActiveAssetToolkit());
		Standalone->Ready = World->Ready = false;
		ASSERT_TRUE(Service->RestoreSessions(Entries).IsAccepted());
		Edit->FocusWindow();
		Standalone->Ready = World->Ready = true;
		Service->TickAssetEditors();
		EXPECT_EQ(Edit->GetToolkit(), Service->GetActiveAssetToolkit());
	}

	TEST_F(FAssetEditorSessionTests, DeferredReplacementPublishesOnlyAfterAssociationAndCancellationKeepsCurrentAsset)
	{
		auto* A = Asset("DeferredA"); auto* B = Asset("DeferredB"); auto* C = Asset("DeferredC");
		auto* Session = Open({A});
		ASSERT_NE(nullptr, Session);
		const auto Toolkit = std::dynamic_pointer_cast<FSessionTestToolkit>(Session->GetToolkit());
		const auto Documents = Session->GetDocumentManager();
		const auto Initial = Documents->GetDocuments().front();
		Toolkit->bDeferOpen = true;
		auto* Definition = NewObject<FGenericSessionTestDefinition>(nullptr, {});
		auto Registration = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition);
		ASSERT_TRUE(Registration);
		Definition->OpenOverride = [&](const FAssetOpenArgs& Args) {
			const auto Id = Documents->OpenDocument({.ToolkitId = Initial.ToolkitId, .DocumentKey = Initial.DocumentKey,
				.ResourceId = Args.Assets.front().AssetPath.ToString(), .Label = "Deferred scene"});
			EXPECT_TRUE(Id.IsValid());
			const auto Request = Service->BeginOpenRequest(Args.Assets.front(), [] { return false; });
			EXPECT_TRUE(Service->BindOpenRequestToDocument(Request, Id.Value));
			return FAssetOpenResult{{{.Target = Args.Assets.front(), .Status = EAssetOpenStatus::Pending, .RequestId = Request}}};
		};
		std::vector<EAssetEditorEvent> Events;
		auto Subscription = Service->Subscribe([&](const FAssetEditorEvent& Event) {
			Events.push_back(Event.Type);
			if (Event.Type == EAssetEditorEvent::Opened && Event.Target.AssetPath.ToString() == B->GetObjectPath()) EXPECT_EQ(Session, Service->FindEditorForAsset(B, false));
		});
		auto Request = [&](DObject* Object) {
			FSoftObjectPath Path; EXPECT_TRUE(FSoftObjectPath::TryCreate(Object->GetObjectPath(), Path));
			return Service->OpenAssets({.AssetEditors = Service, .Assets = {{Path, Object->GetClass()->GetQualifiedName().ToString()}}}).Items.front();
		};
		const auto OpenB = Request(B);
		EXPECT_EQ(EAssetOpenStatus::Pending, OpenB.Status);
		FEditorEngineTestAccess::SetStartupPhase(*Host, Editor::Startup::EBootstrapState::LoadingDefaultDocument);
		FEditorEngineTestAccess::SetStartupDocumentResult(*Host, OpenB);
		Host->AdvanceStartup();
		EXPECT_EQ(EHostStartupState::Pending, Host->GetStartupState());
		ASSERT_NE(nullptr, Open({Asset("UnrelatedSession")}));
		Service->TickAssetEditors();
		EXPECT_EQ(EAssetOpenStatus::Pending, Service->GetOpenRequest(OpenB.RequestId)->Status);
		Events.clear();
		EXPECT_EQ(Session, Service->FindEditorForAsset(A, false));
		EXPECT_EQ(nullptr, Service->FindEditorForAsset(B, false));
		ASSERT_TRUE(Documents->CompleteDeferredDocumentOpen(Initial.Id, true));
		EXPECT_EQ(EAssetOpenStatus::Completed, Service->GetOpenRequest(OpenB.RequestId)->Status);
		Host->AdvanceStartup();
		EXPECT_EQ(EHostStartupState::Ready, Host->GetStartupState()) << Host->GetStartupError();
		const auto Opened = std::ranges::find(Events, EAssetEditorEvent::Opened);
		const auto Finished = std::ranges::find(Events, EAssetEditorEvent::RequestFinished);
		ASSERT_NE(Events.end(), Opened); ASSERT_NE(Events.end(), Finished);
		EXPECT_LT(Opened - Events.begin(), Finished - Events.begin());
		const auto OpenC = Request(C);
		ASSERT_TRUE(Documents->CompleteDeferredDocumentOpen(Initial.Id, false, EAssetOpenStatus::Cancelled));
		EXPECT_EQ(EAssetOpenStatus::Cancelled, Service->GetOpenRequest(OpenC.RequestId)->Status);
		EXPECT_EQ(Session, Service->FindEditorForAsset(B, false));
		EXPECT_EQ(nullptr, Service->FindEditorForAsset(C, false));
		const auto Failed = Request(C);
		FEditorEngineTestAccess::SetStartupPhase(*Host, Editor::Startup::EBootstrapState::LoadingDefaultDocument);
		FEditorEngineTestAccess::SetStartupDocumentResult(*Host, Failed);
		ASSERT_TRUE(Documents->CompleteDeferredDocumentOpen(Initial.Id, false, EAssetOpenStatus::Failed, "level activation failed"));
		Host->AdvanceStartup();
		EXPECT_EQ(EHostStartupState::Failed, Host->GetStartupState());
		EXPECT_EQ("level activation failed", Host->GetStartupError());
		EXPECT_EQ(Session, Service->FindEditorForAsset(B, false));
		FEditorEngineTestAccess::SetStartupPhase(*Host, Editor::Startup::EBootstrapState::LoadingDefaultDocument);
		FEditorEngineTestAccess::SetStartupDocumentResult(*Host, *Service->GetOpenRequest(OpenB.RequestId));
		Service->RetireAssetEditor(*Session);
		Host->AdvanceStartup();
		EXPECT_EQ(EHostStartupState::Failed, Host->GetStartupState());
		EXPECT_NE(std::string::npos, Host->GetStartupError().find("closed before startup completed"));
	}

	TEST_F(FAssetEditorSessionTests, DefaultPanelFactoriesAreOptionalForGraphOnlySessions)
	{
		class FFactoryToolkit final : public FSessionTestToolkit
		{
		public:
			FFactoryToolkit(bool Enabled, int& Viewports, int& Details)
				: FSessionTestToolkit(Enabled, Enabled), Viewports(Viewports), Details(Details) {}
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
		FSessionTestEditor::Factory = [&] { return std::make_shared<FFactoryToolkit>(false, Viewports, Details); };
		ASSERT_NE(nullptr, Open({Asset("GraphOnly")}));
		EXPECT_EQ(0, Viewports); EXPECT_EQ(0, Details);
		FSessionTestEditor::Factory = [&] { return std::make_shared<FFactoryToolkit>(true, Viewports, Details); };
		auto* Session = Open({Asset("DefaultPanels")});
		ASSERT_NE(nullptr, Session);
		EXPECT_EQ(1, Viewports); EXPECT_EQ(1, Details);
		const auto Toolkit = std::dynamic_pointer_cast<FBaseAssetToolkit>(Session->GetToolkit());
		EXPECT_TRUE(Toolkit->IsPanelOpen("Viewport"));
		EXPECT_TRUE(Toolkit->IsPanelOpen("Details"));
	}

	TEST_F(FAssetEditorSessionTests, ScopedToolsUseTheActiveCommandContextAndReleaseCallbacks)
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

	TEST_F(FAssetEditorSessionTests, ClosingSessionReleasesToolCallbacksEvenWithAFrameLease)
	{
		auto* Session = Open({Asset("ReleaseTool")});
		ASSERT_NE(nullptr, Session);
		const auto Toolkit = std::dynamic_pointer_cast<FBaseAssetToolkit>(Session->GetToolkit());
		auto Lifetime = std::make_shared<int>(0);
		const std::weak_ptr<int> Weak = Lifetime;
		auto Tool = Toolkit->RegisterUIExtension({.Panels = {{"Tool.Panel", "Panel", [Lifetime] {}}}});
		Lifetime.reset();
		ASSERT_TRUE(Tool);
		EXPECT_TRUE(Session->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
		EXPECT_TRUE(Weak.expired());
		EXPECT_FALSE(Toolkit->RegisterUIExtension({.Commands = {{"Later", "Later", [] {}}}}));
		Tool.Reset();
	}

	TEST_F(FAssetEditorSessionTests, PanelVisibilityRestoresAndDuplicateExtensionsAreAtomic)
	{
		auto* Session = Open({Asset("PanelLayout")});
		ASSERT_NE(nullptr, Session);
		const auto Toolkit = std::dynamic_pointer_cast<FBaseAssetToolkit>(Session->GetToolkit());
		auto Panels = Toolkit->RegisterUIExtension({.Panels = {{"Preview", "Preview", [] {}}, {"Details", "Details", [] {}, false}}});
		ASSERT_TRUE(Panels);
		const auto Path = (Testing::GetTestWorkDirectory() / "session-panel-layout.yaml").generic_string();
		std::filesystem::remove(Path);
		EXPECT_TRUE(Toolkit->RestorePanelLayout(Path));
		EXPECT_TRUE(Toolkit->SetPanelOpen("Preview", false));
		EXPECT_TRUE(Toolkit->SetPanelOpen("Details", true));
		FSessionTestToolkit Restored;
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

	TEST_F(FAssetEditorSessionTests, ViewAllowsInspectionToolsButRejectsMutatingCommands)
	{
		auto* Session = Open({Asset("ViewTool")}, EAssetOpenMethod::View);
		ASSERT_NE(nullptr, Session);
		const auto Toolkit = std::dynamic_pointer_cast<FBaseAssetToolkit>(Session->GetToolkit());
		int Calls = 0;
		auto Tool = Toolkit->RegisterUIExtension({.Commands = {
			{.Id = "Mutate", .Label = "Mutate", .Execute = [&] { ++Calls; }},
			{.Id = "Inspect", .Label = "Inspect", .Execute = [&] { ++Calls; }, .bMutating = false}}});
		ASSERT_TRUE(Tool);
		EXPECT_FALSE(Toolkit->InvokeCommand("Mutate"));
		EXPECT_TRUE(Toolkit->InvokeCommand("Inspect"));
		EXPECT_EQ(1, Calls);
	}

	TEST_F(FAssetEditorSessionTests, AToolCanWithdrawItselfDuringCommandEligibility)
	{
		auto* Session = Open({Asset("ReentrantTool")});
		ASSERT_NE(nullptr, Session);
		const auto Toolkit = std::dynamic_pointer_cast<FBaseAssetToolkit>(Session->GetToolkit());
		FToolkitUIRegistration Tool;
		bool Executed = false;
		Tool = Toolkit->RegisterUIExtension({.Commands = {{.Id = "Withdraw", .Label = "Withdraw",
			.Execute = [&] { Executed = true; }, .CanExecute = [&] { Tool.Reset(); return true; }}}});
		EXPECT_FALSE(Toolkit->InvokeCommand("Withdraw"));
		EXPECT_FALSE(Executed);
	}

	TEST_F(FAssetEditorSessionTests, HostFactoryIsEditorLocalAndFailedPresentationDoesNotPublish)
	{
		int Created = 0;
		Service->SetToolkitHostFactory([&](const FAssetOpenArgs& Args) -> std::shared_ptr<IToolkitHost> {
			++Created;
			return Args.ToolkitMode == EToolkitMode::WorldCentric ? nullptr : std::make_shared<FNonWindowToolkitHost>();
		});
		EXPECT_EQ(nullptr, Open({Asset("NoWorldHost")}, EAssetOpenMethod::Edit, true, {}, {}, EToolkitMode::WorldCentric));
		EXPECT_TRUE(Service->GetOwnedAssetEditors().empty());
		ASSERT_NE(nullptr, Open({Asset("HasStandaloneHost")}));
		EXPECT_EQ(2, Created);
		EXPECT_EQ(1u, Service->GetAllOpenEditors().size());
		Service->SetToolkitHostFactory({});
	}

	TEST_F(FAssetEditorSessionTests, GroupedSessionsUseOneRootEachAndIndependentPanelLayouts)
	{
		auto* Group = Open({Asset("GroupFirst"), Asset("GroupSecond")});
		auto* Single = Open({Asset("OtherWindow")});
		ASSERT_NE(nullptr, Group);
		ASSERT_NE(nullptr, Single);
		const auto First = std::dynamic_pointer_cast<FSessionTestToolkit>(Group->GetToolkit());
		const auto Second = std::dynamic_pointer_cast<FSessionTestToolkit>(Single->GetToolkit());
		FTabManager FirstTabs, SecondTabs;
		auto Configure = [](DAssetEditor& Session, FSessionTestToolkit& Toolkit, FTabManager& Tabs) {
			Toolkit.Drawing = [&Session, &Toolkit, &Tabs] {
				return Tabs.DrawDocuments(*Session.GetDocumentManager(), Toolkit.GetToolkitId(),
					Toolkit.GetPresentationRootKey("SessionTest"), [](const auto&) { return true; },
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
			Group->DrawToolkitFrame(false);
			ImGui::SetNextWindowSize(ImVec2(500, 500), ImGuiCond_Always);
			Single->DrawToolkitFrame(true);
			ImGui::Render();
			EXPECT_EQ(0, Context->ErrorCountCurrentFrame);
		}
		const auto FirstKey = First->GetPresentationRootKey("SessionTest");
		const auto SecondKey = Second->GetPresentationRootKey("SessionTest");
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

	TEST_F(FAssetEditorSessionTests, DefaultDefinitionCreatesAndReusesGenericEditAndViewSessions)
	{
		auto* Definition = NewObject<FGenericSessionTestDefinition>(nullptr, {});
		auto Registration = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition);
		ASSERT_TRUE(Registration);
		auto* Object = Asset("GenericAsset");
		FSoftObjectPath Path;
		ASSERT_TRUE(FSoftObjectPath::TryCreate(Object->GetObjectPath(), Path));
		FAssetOpenArgs Args{.AssetEditors = Service, .Assets = {{Path, Object->GetClass()->GetQualifiedName().ToString()}}, .OpenMethod = EAssetOpenMethod::View};
		const auto Flags = Object->GetObjectFlags();
		ASSERT_EQ(EAssetOpenStatus::Completed, Service->OpenAssets(Args).Items.front().Status);
		auto* View = Service->FindAssetEditor(Object, EAssetOpenMethod::View);
		ASSERT_NE(nullptr, View);
		EXPECT_EQ("GenericAssetEditor", View->GetEditorName().ToString());
		EXPECT_EQ(Flags, Object->GetObjectFlags());
		EXPECT_FALSE(View->GetToolkit()->CanSaveActiveDocument());
		EXPECT_FALSE(View->GetToolkit()->SaveActiveDocument());
		EXPECT_EQ(EAssetOpenStatus::Completed, Service->OpenAssets(Args).Items.front().Status);
		EXPECT_EQ(1u, Service->GetOwnedAssetEditors().size());
		Args.OpenMethod = EAssetOpenMethod::Edit;
		ASSERT_EQ(EAssetOpenStatus::Completed, Service->OpenAssets(Args).Items.front().Status);
		auto* Edit = Service->FindAssetEditor(Object, EAssetOpenMethod::Edit);
		ASSERT_NE(nullptr, Edit);
		EXPECT_NE(View, Edit);
		EXPECT_TRUE(Edit->GetToolkit()->CanSaveActiveDocument());
		EXPECT_TRUE(Edit->GetToolkit()->SaveActiveDocument());
		EXPECT_FALSE(Package->IsDirty());
		EXPECT_TRUE(Edit->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
		EXPECT_TRUE(View->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
		EXPECT_TRUE(Service->GetOwnedAssetEditors().empty());
	}

	TEST_F(FAssetEditorSessionTests, TransactionScopesRestoreTheirOwnerWithoutChangingTheProcessEditor)
	{
		const auto ProcessEditor = GEditor;
		const TStrongObjectPtr<DEditorEngine> Other(NewObject<DEditorEngine>(nullptr, {}));
		EXPECT_EQ(ProcessEditor, Editor::GetTransactionEditor());
		{
			Editor::FScopedEditorTransactionContext Scope(Other.Get());
			EXPECT_EQ(Other.Get(), Editor::GetTransactionEditor());
			EXPECT_EQ(ProcessEditor, GEditor);
			{
				Editor::FScopedEditorTransactionContext Nested(nullptr);
				EXPECT_EQ(nullptr, Editor::GetTransactionEditor());
			}
			EXPECT_EQ(Other.Get(), Editor::GetTransactionEditor());
		}
		EXPECT_EQ(ProcessEditor, Editor::GetTransactionEditor());
		FEditableAssetDocumentModel Documents;
		Documents.BindEditor(nullptr);
		EXPECT_EQ(nullptr, Documents.GetEditor());
		EXPECT_FALSE(Documents.CanUndo());
		MarkObjectHierarchyAsGarbage(Other.Get());
	}

	TEST_F(FAssetEditorSessionTests, SameTypeCreatesIndependentSessionsAndIndexesEachOnlyOnce)
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
		EXPECT_EQ(2u, Service->GetAllOpenEditors().size());
		EXPECT_EQ(First, Service->FindEditorForAsset(FirstAsset, false));
		EXPECT_EQ(Second, Service->FindEditorForAsset(SecondAsset, false));
		EXPECT_EQ(1u, Service->FindEditorsForAsset(FirstAsset).size());
	}

	TEST_F(FAssetEditorSessionTests, GroupedAndSecondarySessionsDoNotAssumeOneDocumentPerInstance)
	{
		auto* A = Asset("A");
		auto* B = Asset("B");
		auto* Primary = Open({A, B});
		ASSERT_NE(nullptr, Primary);
		EXPECT_EQ(1u, Service->GetAllOpenEditors().size());
		EXPECT_EQ(2u, Primary->GetObjectsToEdit().size());
		EXPECT_EQ(Primary, Service->FindEditorForAsset(A, false));
		EXPECT_EQ(Primary, Service->FindEditorForAsset(B, false));
		auto* Secondary = Open({A}, EAssetOpenMethod::Edit, false);
		ASSERT_NE(nullptr, Secondary);
		EXPECT_EQ(2u, Service->FindEditorsForAsset(A).size());
		EXPECT_EQ(Primary, Service->FindEditorForAsset(A, true));
		EXPECT_EQ(Primary->GetToolkit(), Service->GetActiveAssetToolkit());
	}

	TEST_F(FAssetEditorSessionTests, ViewGuardsCommandsAndDoesNotPromptForAnotherEditorsDirtyAsset)
	{
		auto* Session = Open({Asset("A")}, EAssetOpenMethod::View);
		ASSERT_NE(nullptr, Session);
		auto Toolkit = std::dynamic_pointer_cast<FSessionTestToolkit>(Session->GetToolkit());
		ASSERT_TRUE(Toolkit);
		Toolkit->bDirty = true;
		EXPECT_TRUE(Toolkit->IsReadOnly());
		EXPECT_FALSE(Toolkit->CanSaveActiveDocument());
		EXPECT_FALSE(Toolkit->SaveActiveDocument());
		EXPECT_FALSE(Toolkit->CanUndo());
		EXPECT_FALSE(Toolkit->Undo());
		EXPECT_FALSE(Toolkit->SaveDocument(Session->GetDocumentManager()->GetDocuments().front()));
		EXPECT_FALSE(Toolkit->DiscardDocument(Session->GetDocumentManager()->GetDocuments().front()));
		EXPECT_EQ(0, Toolkit->Saves);
		EXPECT_EQ(0, Toolkit->Undos);
		EXPECT_TRUE(Session->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
		EXPECT_TRUE(Service->GetAllOpenEditors().empty());
	}

	TEST_F(FAssetEditorSessionTests, FailedSaveRetainsPendingCloseAndSessionUntilResolution)
	{
		auto* Session = Open({Asset("A")});
		ASSERT_NE(nullptr, Session);
		auto Toolkit = std::dynamic_pointer_cast<FSessionTestToolkit>(Session->GetToolkit());
		Toolkit->bDirty = true;
		Toolkit->bSaveSucceeds = false;
		EXPECT_FALSE(Session->CloseWindow(EAssetEditorCloseReason::CloseAllAssetEditors));
		EXPECT_EQ(EDocumentCloseResult::PendingConfirmation, Session->GetLastCloseResult());
		const auto Documents = Session->GetDocumentManager();
		EXPECT_EQ(EDocumentCloseResult::PendingConfirmation,
			Documents->ResolvePendingDocumentClose(EDocumentCloseResponse::Save));
		EXPECT_EQ(1u, Service->GetOwnedAssetEditors().size());
		EXPECT_EQ(1u, Documents->GetDocuments().size());
		Toolkit->bSaveSucceeds = true;
		EXPECT_EQ(EDocumentCloseResult::Closed, Documents->ResolvePendingDocumentClose(EDocumentCloseResponse::Save));
		Service->TickAssetEditors();
		EXPECT_TRUE(Service->GetOwnedAssetEditors().empty());
		EXPECT_TRUE(Service->GetAllOpenEditors().empty());
	}

	TEST_F(FAssetEditorSessionTests, GroupCloseContinuesAfterSaveAndStopsAfterCancel)
	{
		auto* Session = Open({Asset("GroupA"), Asset("GroupB")});
		ASSERT_NE(nullptr, Session);
		const auto Toolkit = std::dynamic_pointer_cast<FSessionTestToolkit>(Session->GetToolkit());
		const auto Documents = Session->GetDocumentManager();
		for (const auto& Document : Documents->GetDocuments()) Toolkit->DirtyDocuments.insert(Document.Id.Value);
		EXPECT_FALSE(Session->CloseWindow(EAssetEditorCloseReason::CloseAllAssetEditors));
		const auto Sequence = Documents->GetPendingCloseSequence();
		Toolkit->bSaveSucceeds = false;
		EXPECT_EQ(EDocumentCloseResult::PendingConfirmation, Documents->ResolvePendingDocumentClose(EDocumentCloseResponse::Save));
		Service->TickAssetEditors();
		EXPECT_EQ(Sequence, Documents->GetPendingCloseSequence());
		Toolkit->bSaveSucceeds = true;
		EXPECT_EQ(EDocumentCloseResult::Closed, Documents->ResolvePendingDocumentClose(EDocumentCloseResponse::Save));
		Service->TickAssetEditors();
		ASSERT_NE(nullptr, Documents->GetPendingCloseDocument());
		EXPECT_EQ(1u, Documents->GetDocuments().size());
		EXPECT_EQ(EDocumentCloseResult::Cancelled, Documents->ResolvePendingDocumentClose(EDocumentCloseResponse::Cancel));
		Service->TickAssetEditors();
		EXPECT_EQ(nullptr, Documents->GetPendingCloseDocument());
		EXPECT_FALSE(Session->IsClosed());
		EXPECT_FALSE(Session->CloseWindow(EAssetEditorCloseReason::CloseAllAssetEditors));
		EXPECT_EQ(EDocumentCloseResult::Closed, Documents->ResolvePendingDocumentClose(EDocumentCloseResponse::Save));
		Service->TickAssetEditors();
		EXPECT_TRUE(Service->GetOwnedAssetEditors().empty());
	}

	TEST_F(FAssetEditorSessionTests, CanonicalPrimarySelectionFollowsExplicitFocus)
	{
		auto* A = Asset("PrimaryA");
		auto* Group = Open({A, Asset("PrimaryB")});
		auto* Single = Open({A});
		ASSERT_NE(nullptr, Group); ASSERT_NE(nullptr, Single);
		ASSERT_NE(nullptr, Open({A}, EAssetOpenMethod::Edit, true, {}, "Inspection"));
		EXPECT_EQ(Single, Service->FindAssetEditor(A, EAssetOpenMethod::Edit, EToolkitMode::Standalone));
		Group->FocusWindow(A);
		EXPECT_EQ(Group, Service->FindAssetEditor(A, EAssetOpenMethod::Edit, EToolkitMode::Standalone));
		EXPECT_TRUE(Service->OpenEditorForAsset(A));
		EXPECT_EQ(Group->GetToolkit(), Service->GetActiveAssetToolkit());
	}

	TEST_F(FAssetEditorSessionTests, FailedInitializationPublishesNoSessionOrAssociation)
	{
		FSessionTestEditor::bFailCreation = true;
		EXPECT_EQ(nullptr, Open({Asset("A")}));
		EXPECT_TRUE(Service->GetOwnedAssetEditors().empty());
		EXPECT_TRUE(Service->GetAllOpenEditors().empty());
	}

	TEST_F(FAssetEditorSessionTests, FrameLeaseDoesNotKeepAClosedReflectedSessionAlive)
	{
		auto* Session = Open({Asset("A")});
		ASSERT_NE(nullptr, Session);
		const FObjectKey SessionKey(Session);
		const auto Toolkit = Session->GetToolkit();
		EXPECT_FALSE(Toolkit->GetOwningAssetEditor() == nullptr);
		EXPECT_TRUE(Session->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
		CollectGarbage();
		EXPECT_EQ(nullptr, SessionKey.ResolveObjectPtr());
		EXPECT_EQ(nullptr, Toolkit->GetOwningAssetEditor());
		EXPECT_FALSE(Toolkit->CanUndo());
		EXPECT_FALSE(Toolkit->Undo());
	}

	TEST_F(FAssetEditorSessionTests, NonWindowHostRetainsPresentationWhileDirtyCloseIsPending)
	{
		const auto ToolkitHost = std::make_shared<FNonWindowToolkitHost>();
		auto* Session = Open({Asset("A")}, EAssetOpenMethod::Edit, true, ToolkitHost);
		ASSERT_NE(nullptr, Session);
		const auto Toolkit = std::dynamic_pointer_cast<FSessionTestToolkit>(Session->GetToolkit());
		Toolkit->bDirty = true;
		EXPECT_EQ(ToolkitHost, Service->GetToolkitManager().GetHost(*Toolkit));
		EXPECT_EQ(1u, Service->GetToolkitManager().GetToolkits().size());
		EXPECT_FALSE(ToolkitHost->RequestClose());
		EXPECT_TRUE(ToolkitHost->IsAvailable());
		EXPECT_FALSE(Session->IsClosed());
		EXPECT_EQ(EDocumentCloseResult::Closed,
			Session->GetDocumentManager()->ResolvePendingDocumentClose(EDocumentCloseResponse::Discard));
		Service->TickAssetEditors();
		EXPECT_TRUE(Service->GetToolkitManager().GetToolkits().empty());
		EXPECT_TRUE(ToolkitHost->RequestClose());
		EXPECT_FALSE(ToolkitHost->IsAvailable());
	}

	TEST_F(FAssetEditorSessionTests, FailedHostAttachmentPublishesNoInstanceOrToolkit)
	{
		class FRejectingHost : public FNonWindowToolkitHost
		{
		public:
			auto AttachToolkit(std::shared_ptr<FAssetEditorToolkit>) -> bool override { return false; }
		};
		const auto Escaped = std::make_shared<FSessionTestToolkit>();
		int Draws = 0;
		Escaped->Drawing = [&] { ++Draws; return true; };
		FSessionTestEditor::Factory = [Escaped] { return Escaped; };
		EXPECT_EQ(nullptr, Open({Asset("A")}, EAssetOpenMethod::Edit, true, std::make_shared<FRejectingHost>()));
		EXPECT_TRUE(Escaped->IsRetired());
		EXPECT_FALSE(Escaped->IsSessionActive());
		EXPECT_FALSE(Escaped->SaveActiveDocument());
		EXPECT_FALSE(Escaped->Undo());
		EXPECT_FALSE(Escaped->DrawToolkit(true));
		EXPECT_EQ(0, Draws);
		EXPECT_TRUE(Service->GetOwnedAssetEditors().empty());
		EXPECT_TRUE(Service->GetToolkitManager().GetToolkits().empty());
		EXPECT_TRUE(Service->GetAllOpenEditors().empty());
	}

	TEST_F(FAssetEditorSessionTests, DeactivationVetoKeepsCurrentSessionAndRejectsNewSessionCreation)
	{
		auto* First = Open({Asset("First")});
		ASSERT_NE(nullptr, First);
		const auto Toolkit = std::dynamic_pointer_cast<FSessionTestToolkit>(First->GetToolkit());
		Toolkit->bCanDeactivate = false;
		EXPECT_EQ(nullptr, Open({Asset("Second")}));
		EXPECT_EQ(1u, Service->GetOwnedAssetEditors().size());
		EXPECT_EQ(Toolkit, Service->GetActiveAssetToolkit());
		Toolkit->bCanDeactivate = true;
	}

	TEST_F(FAssetEditorSessionTests, ShutdownDuringDeactivationCannotPublishAnotherSession)
	{
		auto* First = Open({Asset("First")});
		ASSERT_NE(nullptr, First);
		const auto Toolkit = std::dynamic_pointer_cast<FSessionTestToolkit>(First->GetToolkit());
		bool Requested = false;
		Toolkit->Deactivate = [&] {
			if (!Requested)
			{
				Requested = true;
				Host->RequestShutdown();
				CollectGarbage();
			}
			return true;
		};
		EXPECT_EQ(nullptr, Open({Asset("Second")}));
		EXPECT_TRUE(Requested);
		EXPECT_TRUE(Host->IsShutdownRequested());
		Host->PreExit();
		if (auto* Remaining = ServiceLifetime.Get()) EXPECT_TRUE(Remaining->IsReadyForFinishDestroy());
	}

	TEST_F(FAssetEditorSessionTests, CloseConfirmationKeepsRequestOrderAcrossIndependentControllers)
	{
		auto* A = Open({Asset("A")});
		auto* B = Open({Asset("B")});
		ASSERT_NE(nullptr, A);
		ASSERT_NE(nullptr, B);
		std::dynamic_pointer_cast<FSessionTestToolkit>(A->GetToolkit())->bDirty = true;
		std::dynamic_pointer_cast<FSessionTestToolkit>(B->GetToolkit())->bDirty = true;
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

	TEST_F(FAssetEditorSessionTests, AdditionalSessionsRequireDistinctStableRoles)
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
		EXPECT_NE(Primary->GetToolkit()->GetLayoutProfileKey(), Secondary->GetToolkit()->GetLayoutProfileKey());
	}

	TEST_F(FAssetEditorSessionTests, HostTransferPreservesSessionAndRejectsDestinationIdentityCollision)
	{
		auto* Object = Asset("A");
		auto* Session = Open({Object});
		ASSERT_NE(nullptr, Session);
		const auto Toolkit = Session->GetToolkit();
		const auto Documents = Session->GetDocumentManager();
		const auto OriginalKey = Toolkit->GetPresentationRootKey("Editor");
		const auto WorldHost = std::make_shared<FNonWindowToolkitHost>(EToolkitMode::WorldCentric);
		ASSERT_TRUE(Session->TransferToHost(WorldHost));
		EXPECT_EQ(EToolkitMode::WorldCentric, Session->GetToolkitMode());
		EXPECT_EQ(WorldHost, Service->GetToolkitManager().GetHost(*Toolkit));
		EXPECT_EQ(Toolkit, Session->GetToolkit());
		EXPECT_EQ(Documents, Session->GetDocumentManager());
		EXPECT_EQ(OriginalKey, Toolkit->GetPresentationRootKey("Editor"));
		auto* Other = Open({Object});
		ASSERT_NE(nullptr, Other);
		EXPECT_FALSE(Session->TransferToHost(Other->GetToolkitHost()));
		EXPECT_EQ(WorldHost, Session->GetToolkitHost());
	}

	TEST_F(FAssetEditorSessionTests, NamedDockTemplateRestoresIntoIndependentRuntimeWindows)
	{
		auto* A = Open({Asset("DockTemplateA")}); auto* B = Open({Asset("DockTemplateB")});
		ASSERT_NE(nullptr, A); ASSERT_NE(nullptr, B);
		const auto First = std::dynamic_pointer_cast<FSessionTestToolkit>(A->GetToolkit());
		const auto Second = std::dynamic_pointer_cast<FSessionTestToolkit>(B->GetToolkit());
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
