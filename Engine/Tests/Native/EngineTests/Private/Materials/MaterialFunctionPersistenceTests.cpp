#include "Editor/AssetEditorTestHost.h"
#include "Editor/AssetEditorTestQueries.h"
#include "Misc/UnifiedError.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "FunctionPortTestFixture.h"
#include "MaterialGraph.h"
#include "MaterialFunctionTestSupport.h"
#include "Toolkits/ToolkitManager.h"
#include "Asset/PackageReload.h"

TEST(FMaterialFunctionPersistenceTests, WorkspaceSavesAndReloadsFunctionsAcrossOpenDocuments)
{
	using namespace Durin;
	using namespace Durin;
	using namespace Durin::MaterialEditor;
	InitializeDObjectSystem();
	const auto Root = Testing::CreateTestFixtureDirectory("FunctionWorkspace");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/FunctionWorkspace/", .Owner = EMountOwner::Test,
		.Root = Root, .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid());
	ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath FirstPath, SecondPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/FunctionWorkspace/First", FirstPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/FunctionWorkspace/Second", SecondPath));
	DMaterialFunction* First = nullptr;
	DMaterialFunction* Second = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(FirstPath, First));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(SecondPath, Second));
	ASSERT_TRUE(FMaterialGraphDocument(*Second).InsertFunctionCall(*First, 0, 300, Durin::ETransactionMode::NotTransactable));
	ASSERT_TRUE(SavePackage(First->GetPackage()));
	ASSERT_TRUE(SavePackage(Second->GetPackage()));
	FMaterialEditorModule Module;
	FModuleTestHarness Harness("MaterialEditor");
	Harness.Start(Module);
	Testing::FScopedAssetEditorTestHost TestHost;
	ASSERT_NE(nullptr, TestHost.GetAssetEditorSubsystem());
	const auto Class = DMaterialFunction::StaticClass()->GetQualifiedName().ToString();
	const auto Resource = First->GetObjectPath();
	ASSERT_TRUE(TestHost.Open(Resource, Class));
	ASSERT_TRUE(TestHost.Open(Resource, Class));
	ASSERT_EQ(Durin::Testing::GetReflectedEditors(TestHost.GetAssetEditorSubsystem()).size(), 1u);
	auto* FirstEditor = Durin::Testing::GetReflectedEditors(TestHost.GetAssetEditorSubsystem()).front().Get();
	ASSERT_TRUE(TestHost.Open(Second->GetObjectPath(), Class));
	ASSERT_EQ(Durin::Testing::GetReflectedEditors(TestHost.GetAssetEditorSubsystem()).size(), 2u);
	auto Workspace = FirstEditor->GetToolkit();
	ASSERT_TRUE(Workspace);
	auto* Working = FMaterialFunctionEditorTestAccess::Document(*Workspace, Resource).Function();
	ASSERT_NE(First, Working);
	auto& Session = FMaterialFunctionEditorTestAccess::Document(*Workspace, Resource);
	const auto OutputId = Working->GetFunctionSignature().Outputs.front().Id;
	auto* OutputNode = Testing::FindFunctionTerminal<DMaterialExpressionFunctionOutput>(*Session.Graph, OutputId);
	ASSERT_TRUE(OutputNode);
	ASSERT_TRUE(OutputNode->OnRenameNode("Saved Surface", Durin::ETransactionMode::NotTransactable));
	EXPECT_TRUE(Workspace->IsAssetDirty(*First));
	EXPECT_EQ(Workspace->RequestCloseAsset(*First), EAssetEditorCloseResult::PendingConfirmation);
	EXPECT_EQ(TestHost.GetAssetEditorSubsystem()->ResolvePendingClose(
		TestHost.GetAssetEditorSubsystem()->GetPendingClosePrompt()->RequestId, EAssetEditorCloseResponse::Cancel), EAssetEditorCloseResult::Cancelled);
	EXPECT_NE(First->GetFunctionSignature().Outputs[0].Name, "Saved Surface");
	std::string ApplyError;
	ASSERT_TRUE(FMaterialFunctionEditorTestAccess::Document(*Workspace, Resource).Publish(ApplyError)) << ApplyError;
	EXPECT_TRUE(First->GetPackage()->IsDirty());
	ASSERT_TRUE(Workspace->SaveAsset(*First));
	EXPECT_FALSE(Workspace->IsAssetDirty(*First));
	const auto CopiedCallId = GetFunctionCalls(*Second)[0]->Id;
	FMaterialGraphClipboardPayload Clipboard;
	TStrongObjectPtr<DMaterialGraph> SecondEditorGraph(DMaterialGraph::Create(*Second));
	ASSERT_TRUE(SecondEditorGraph->CopySelection(std::span(&CopiedCallId, 1), Clipboard));
	FMaterialFunctionPreviewInvalidation PreviewInvalidation;
	PreviewInvalidation.SetFunction(Second);
	EXPECT_TRUE(PreviewInvalidation.ConsumeRefreshRequest());
	OutputNode = Testing::FindFunctionTerminal<DMaterialExpressionFunctionOutput>(*Session.Graph, OutputId);
	ASSERT_TRUE(OutputNode);
	ASSERT_TRUE(OutputNode->OnRenameNode("Discarded Surface", Durin::ETransactionMode::NotTransactable));
	EXPECT_FALSE(PreviewInvalidation.ConsumeRefreshRequest());
	const bool Discarded = Workspace->DiscardAsset(*First);
	if (!Discarded)
	{
		const auto Error = Workspace->GetLastOperationError();
		TestHost.Shutdown(); Workspace.reset(); Harness.Shutdown();
		FAIL() << Error;
	}
	DMaterialFunction* Reloaded = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedValueError;
		auto LoadedValue = LoadObject<DMaterialFunction>(Testing::MakePackageLeafAssetObjectPathForTests(FirstPath), nullptr, &LoadedValueError);
		Reloaded = LoadedValue;
		ASSERT_TRUE(LoadedValue);
	}
	ASSERT_NE(Reloaded, nullptr);
	EXPECT_EQ(Reloaded->GetFunctionSignature().Outputs[0].Name, "Saved Surface");
	EXPECT_EQ(GetFunctionCalls(*Second)[0]->Function.Get(), Reloaded);
	EXPECT_FALSE(PreviewInvalidation.ConsumeRefreshRequest());
	const auto Pasted = SecondEditorGraph->Paste(Clipboard, 300, 300, Durin::ETransactionMode::NotTransactable);
	EXPECT_TRUE(Pasted) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Pasted);
	EXPECT_EQ(GetFunctionCalls(*Second).back()->Function.Get(), Reloaded);
	EXPECT_FALSE(Workspace->IsAssetDirty(*First));
	TestHost.Shutdown(); Workspace.reset();
	Harness.Shutdown();
}

namespace
{
	using namespace Durin;
	using namespace Durin::MaterialEditor;
	class FMaterialFunctionDocumentTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			Testing::RegisterMountPointForTests("/FunctionSessions/", (Testing::GetTestWorkDirectory() / "FunctionSessions").generic_string() + "/");
			FPackagePath Path;
			ASSERT_TRUE(FPackagePath::TryCreate("/FunctionSessions/" + FGuid::NewGuid().ToString(), Path));
			DMaterialFunction* Value = nullptr;
			ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Value));
			Source = Value;
			ASSERT_TRUE(SavePackage(Source->GetPackage()));
			Harness = std::make_unique<FModuleTestHarness>("MaterialEditor");
			Harness->Start(Module);
			Host = std::make_unique<Testing::FScopedAssetEditorTestHost>();
			Resource = Source->GetObjectPath();
			ASSERT_TRUE(Host->Open(Resource, Source->GetClass()->GetQualifiedName().ToString()));
			Toolkit = Host->GetAssetEditorSubsystem()->GetActiveAssetToolkit();
			ASSERT_TRUE(Toolkit);
		}
		auto TearDown() -> void override
		{
			if (Host) Host->Shutdown();
			Toolkit.reset();
			Host.reset();
			if (Harness) Harness->Shutdown();
			if (Source) MarkObjectHierarchyAsGarbage(Source->GetPackage());
			Source.Reset();
			CollectGarbage(Durin::EObjectFlags::Standalone);
		}
		auto& Document()
		{ return FMaterialFunctionEditorTestAccess::Document(*Toolkit, Resource); }
		auto Rename(DMaterialFunction& Function, const char* Name, bool bTransactional = false) -> bool
		{
			TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(Function));
			auto* Terminal = Testing::FindFunctionTerminal<DMaterialExpressionFunctionOutput>(*Graph, Function.GetFunctionSignature().Outputs.front().Id);
			if (!Terminal) return false;
			if (!bTransactional) return static_cast<bool>(Terminal->OnRenameNode(Name, ETransactionMode::NotTransactable));
			FScopedEditorTransactionContext Context(Host->GetAssetEditorSubsystem()->GetEditor());
			return static_cast<bool>(Terminal->OnRenameNode(Name));
		}
		Testing::FScopedMountRegistryFixture Registry;
		TStrongObjectPtr<DMaterialFunction> Source;
		FMaterialEditorModule Module;
		std::unique_ptr<FModuleTestHarness> Harness;
		std::unique_ptr<Testing::FScopedAssetEditorTestHost> Host;
		std::shared_ptr<FAssetEditorToolkit> Toolkit;
		std::string Resource;
	};
}

TEST_F(FMaterialFunctionDocumentTests, EditorGraphFollowsTheFunctionSessionLifetime)
{
	auto* Graph = Document().Graph.Get();
	ASSERT_TRUE(Graph);
	ASSERT_EQ(Graph->GetMaterialOrFunction(), Document().Function());
	ASSERT_NE(Graph->GetMaterialOrFunction(), Source.Get());
	auto* History = Host->GetAssetEditorSubsystem()->GetEditor()->GetTransactor();
	const auto Before = History->GetUndoCount();
	for (int Pass = 0; Pass < 2; ++Pass)
	{
		Graph->RebuildGraph();
		ASSERT_FALSE(Graph->GetNodes().empty());
		for (const auto& Node : Graph->GetNodes())
		{
			EXPECT_FALSE(Node->GetView().Node.bMaterialOutput);
			ASSERT_TRUE(Node->GetMaterialExpression());
			EXPECT_EQ(Node->GetMaterialExpression()->GetOuter(), Document().Function());
		}
		EXPECT_FALSE(Document().IsDirty());
		EXPECT_EQ(History->GetUndoCount(), Before);
	}
	TWeakObjectPtr<DMaterialGraph> RetiredGraph(Graph);
	TWeakObjectPtr<DMaterialGraphNode> RetiredNode(Graph->GetNodes().front().Get());
	FToolkitManager::Get().CloseToolkit(*Toolkit);
	CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_FALSE(RetiredGraph.IsValid());
	EXPECT_FALSE(RetiredNode.IsValid());
}

TEST_F(FMaterialFunctionDocumentTests, WorkingEditsStayIsolatedAndUndoRedoPublishLikeUE)
{
	auto* Working = Document().Function();
	ASSERT_NE(Working, Source.Get());
	EXPECT_NE(Working->GetPackage(), Source->GetPackage());
	const auto Initial = Source->GetFunctionSignature().Outputs.front().Name;
	FMaterialFunctionPreviewInvalidation Invalidation;
	Invalidation.SetFunction(Source.Get());
	EXPECT_TRUE(Invalidation.ConsumeRefreshRequest());
	ASSERT_TRUE(Rename(*Working, "Draft", true));
	EXPECT_EQ(Initial, Source->GetFunctionSignature().Outputs.front().Name);
	EXPECT_FALSE(Source->GetPackage()->IsDirty());
	EXPECT_TRUE(Toolkit->IsAssetDirty(*Source));
	EXPECT_EQ(EAssetEditorCloseResult::PendingConfirmation, Toolkit->RequestCloseAsset(*Source));
	EXPECT_EQ(EAssetEditorCloseResult::Cancelled, Host->GetAssetEditorSubsystem()->ResolvePendingClose(
		Host->GetAssetEditorSubsystem()->GetPendingClosePrompt()->RequestId, EAssetEditorCloseResponse::Cancel));
	EXPECT_EQ("Draft", Working->GetFunctionSignature().Outputs.front().Name);
	EXPECT_FALSE(Invalidation.ConsumeRefreshRequest());
	ASSERT_TRUE(Toolkit->CanUndo());
	ASSERT_TRUE(Toolkit->Undo());
	EXPECT_EQ(Initial, Working->GetFunctionSignature().Outputs.front().Name);
	EXPECT_FALSE(Document().IsDirty());
	ASSERT_TRUE(Toolkit->Redo());
	EXPECT_EQ("Draft", Working->GetFunctionSignature().Outputs.front().Name);
	EXPECT_EQ("Draft", Source->GetFunctionSignature().Outputs.front().Name);
	EXPECT_TRUE(Invalidation.ConsumeRefreshRequest());
	std::string Error;
	ASSERT_TRUE(Document().Publish(Error)) << Error;
	EXPECT_EQ("Draft", Source->GetFunctionSignature().Outputs.front().Name);
	EXPECT_TRUE(Source->GetPackage()->IsDirty());
	EXPECT_FALSE(Document().IsDirty());
	EXPECT_FALSE(Invalidation.ConsumeRefreshRequest());
	ASSERT_TRUE(Toolkit->SaveAsset(*Source)) << Toolkit->GetLastOperationError();
	EXPECT_FALSE(Source->GetPackage()->IsDirty());
	EXPECT_FALSE(Working->GetPackage()->IsDirty());
	EXPECT_FALSE(Toolkit->IsAssetDirty(*Source));
}

TEST_F(FMaterialFunctionDocumentTests, SeparateRolesCannotOverwriteAppliedSourceChanges)
{
	auto* FirstEditor = Toolkit->GetOwningAssetEditor();
	auto* Editors = Host->GetAssetEditorSubsystem();
	auto* Second = Editors->OpenAssetEditor(FirstEditor->GetClass(), {
		.OpenArgs = {.AssetEditors = Editors, .InstanceRole = "OtherFunctionRole"}, .Assets = {Source.Get()}});
	ASSERT_NE(nullptr, Second);
	const auto Other = Second->GetToolkit();
	auto& OtherDocument = FMaterialFunctionEditorTestAccess::Document(*Other, Resource);
	const auto Initial = Source->GetFunctionSignature().Outputs.front().Name;
	ASSERT_TRUE(Rename(*Document().Function(), "First role"));
	EXPECT_EQ(Initial, OtherDocument.Function()->GetFunctionSignature().Outputs.front().Name);
	EXPECT_EQ(Initial, Source->GetFunctionSignature().Outputs.front().Name);
	// Saving the original package cannot include the isolated draft.
	ASSERT_TRUE(SavePackage(Source->GetPackage()));
	EXPECT_EQ(Initial, Source->GetFunctionSignature().Outputs.front().Name);
	std::string Error;
	ASSERT_TRUE(Document().Publish(Error)) << Error;
	ASSERT_TRUE(Rename(*OtherDocument.Function(), "Second role"));
	EXPECT_FALSE(OtherDocument.Publish(Error));
	EXPECT_NE(Error.find("changed outside"), std::string::npos);
	EXPECT_EQ("First role", Source->GetFunctionSignature().Outputs.front().Name);
	EXPECT_EQ("First role", OtherDocument.GetSource()->GetFunctionSignature().Outputs.front().Name);
}

TEST_F(FMaterialFunctionDocumentTests, DiscardDropsOnlyTheDraftAndPreservesAppliedSource)
{
	const auto Initial = Source->GetFunctionSignature().Outputs.front().Name;
	ASSERT_TRUE(Rename(*Document().Function(), "Unapplied", true));
	auto* Original = Source.Get();
	ASSERT_TRUE(Toolkit->DiscardAsset(*Source)) << Toolkit->GetLastOperationError();
	EXPECT_EQ(Original, Document().GetSource());
	EXPECT_EQ(Initial, Document().Function()->GetFunctionSignature().Outputs.front().Name);
	EXPECT_FALSE(Toolkit->IsAssetDirty(*Source));
	EXPECT_FALSE(Toolkit->CanUndo());
	ASSERT_TRUE(Rename(*Document().Function(), "Applied but unsaved"));
	std::string Error;
	ASSERT_TRUE(Document().Publish(Error)) << Error;
	EXPECT_TRUE(Source->GetPackage()->IsDirty());
	EXPECT_FALSE(Toolkit->IsAssetDirty(*Source));
	ASSERT_TRUE(Rename(*Document().Function(), "Discard this draft", true));
	ASSERT_TRUE(Toolkit->DiscardAsset(*Source)) << Toolkit->GetLastOperationError();
	EXPECT_EQ(Original, Document().GetSource());
	EXPECT_EQ("Applied but unsaved", Source->GetFunctionSignature().Outputs.front().Name);
	EXPECT_EQ("Applied but unsaved", Document().Function()->GetFunctionSignature().Outputs.front().Name);
	EXPECT_TRUE(Source->GetPackage()->IsDirty());
	EXPECT_FALSE(Toolkit->IsAssetDirty(*Source));
	EXPECT_FALSE(Toolkit->CanUndo());
	ASSERT_TRUE(Toolkit->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
	EXPECT_EQ("Applied but unsaved", Source->GetFunctionSignature().Outputs.front().Name);
	EXPECT_TRUE(Source->GetPackage()->IsDirty());
}

TEST_F(FMaterialFunctionDocumentTests, ApplyRejectsProspectiveSourceRecursionWithoutMutation)
{
	const auto Initial = Source->GetFunctionSignature().Outputs.front().Name;
	ASSERT_TRUE(Rename(*Document().Function(), "Recursive draft"));
	ASSERT_TRUE(FMaterialGraphDocument(*Document().Function()).InsertFunctionCall(*Source, 0, 100, ETransactionMode::NotTransactable));
	std::string Error;
	EXPECT_FALSE(Document().Publish(Error));
	EXPECT_NE(Error.find("non-recursive"), std::string::npos);
	EXPECT_EQ(Initial, Source->GetFunctionSignature().Outputs.front().Name);
	EXPECT_FALSE(Source->GetPackage()->IsDirty());
	EXPECT_TRUE(Document().IsDirty());
	EXPECT_FALSE(Toolkit->SaveAsset(*Source));
	EXPECT_FALSE(Source->GetPackage()->IsDirty());
	ASSERT_TRUE(Toolkit->DiscardAsset(*Source));
	EXPECT_FALSE(Document().IsDirty());
}

TEST_F(FMaterialFunctionDocumentTests, ApplyRejectsReentrantPublicationFromDependencyNotifications)
{
	ASSERT_TRUE(Rename(*Document().Function(), "Published once"));
	bool bReentrantSucceeded = true;
	int Notifications = 0;
	const auto Handle = GetMaterialFunctionChangedEvent().AddLambda([&](const DMaterialFunctionInterface& Changed) {
		if (&Changed != Source.Get()) return;
		++Notifications;
		std::string Error;
		bReentrantSucceeded = Document().Publish(Error);
	});
	std::string Error;
	const bool Applied = Document().Publish(Error);
	GetMaterialFunctionChangedEvent().Remove(Handle);
	ASSERT_TRUE(Applied) << Error;
	EXPECT_EQ(1, Notifications);
	EXPECT_FALSE(bReentrantSucceeded);
	EXPECT_EQ("Published once", Source->GetFunctionSignature().Outputs.front().Name);
}

TEST_F(FMaterialFunctionDocumentTests, RelocationKeepsTheDraftAndPublishesToTheRenamedSource)
{
	auto* Working = Document().Function();
	ASSERT_TRUE(Rename(*Working, "Draft across relocation"));
	const auto Profile = Toolkit->GetLayoutProfileKey();
	const auto PackageName = Source->GetPackage()->GetPackagePath();
	FPackagePath Destination;
	ASSERT_TRUE(FPackagePath::TryCreate(PackageName + "Moved", Destination));
	const auto Text = Destination.ToString();
	const FAssetRenameData Data{TWeakObjectPtr<DObject>(Source.Get()), Text.substr(0, Text.find_last_of('/')), Destination.GetPackageName()};
	ASSERT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Data, 1}));
	Resource = Source->GetObjectPath();
	EXPECT_EQ(Resource, Toolkit->GetActiveAsset()->GetObjectPath());
	EXPECT_EQ(Working, Document().Function());
	EXPECT_EQ(Profile, Toolkit->GetLayoutProfileKey());
	EXPECT_EQ(Destination.ToString(), Document().GetSource()->GetPackage()->GetPackagePath());
	ASSERT_TRUE(Toolkit->SaveAsset(*Source)) << Toolkit->GetLastOperationError();
	EXPECT_EQ("Draft across relocation", Source->GetFunctionSignature().Outputs.front().Name);
	EXPECT_FALSE(Toolkit->IsAssetDirty(*Source));
}

TEST_F(FMaterialFunctionDocumentTests, SaveRejectsReentrantDiscardCloseAndUndo)
{
	ASSERT_TRUE(Rename(*Document().Function(), "Saved despite callbacks", true));
	auto* Working = Document().Function();
	int Notifications = 0;
	const auto Handle = GetMaterialFunctionChangedEvent().AddLambda([&](const DMaterialFunctionInterface& Changed) {
		if (&Changed != Source.Get()) return;
		++Notifications;
		EXPECT_FALSE(Toolkit->DiscardAsset(*Source));
		EXPECT_EQ(EAssetEditorCloseResult::Rejected, Toolkit->RequestCloseAsset(*Source));
		EXPECT_FALSE(Toolkit->Undo());
		EXPECT_EQ(Working, Document().Function());
	});
	const bool Saved = Toolkit->SaveAsset(*Source);
	GetMaterialFunctionChangedEvent().Remove(Handle);
	ASSERT_TRUE(Saved) << Toolkit->GetLastOperationError();
	EXPECT_EQ(1, Notifications);
	EXPECT_EQ("Saved despite callbacks", Source->GetFunctionSignature().Outputs.front().Name);
	EXPECT_FALSE(Source->GetPackage()->IsDirty());
	EXPECT_FALSE(Toolkit->IsAssetDirty(*Source));
}

TEST_F(FMaterialFunctionDocumentTests, ForcedCloseDuringSaveRetiresPreviewAndSkipsDiskSave)
{
	ASSERT_TRUE(Rename(*Document().Function(), "Applied during close", true));
	std::weak_ptr<FMaterialPreview> Preview = Document().Preview;
	int Notifications = 0;
	const auto Handle = GetMaterialFunctionChangedEvent().AddLambda([&](const DMaterialFunctionInterface& Changed) {
		if (&Changed != Source.Get()) return;
		++Notifications;
		FToolkitManager::Get().CloseToolkit(*Toolkit);
		EXPECT_TRUE(Toolkit->IsClosed());
		EXPECT_TRUE(Preview.expired());
		CollectGarbage(Durin::EObjectFlags::Standalone);
	});
	const bool Saved = Toolkit->SaveAsset(*Source);
	GetMaterialFunctionChangedEvent().Remove(Handle);
	EXPECT_FALSE(Saved);
	EXPECT_EQ(1, Notifications);
	EXPECT_EQ("Applied during close", Source->GetFunctionSignature().Outputs.front().Name);
	EXPECT_TRUE(Source->GetPackage()->IsDirty());
	EXPECT_TRUE(Preview.expired());
	EXPECT_EQ(nullptr, Toolkit->GetOwningAssetEditor());
	EXPECT_FALSE(Host->GetAssetEditorSubsystem()->GetEditor()->GetTransactor()->CanUndo());
}

TEST_F(FMaterialFunctionDocumentTests, PublicationKeepsNewDraftEditsFromNotificationCallbacks)
{
	ASSERT_TRUE(Rename(*Document().Function(), "Published"));
	const auto Handle = GetMaterialFunctionChangedEvent().AddLambda([&](const DMaterialFunctionInterface& Changed) {
		if (&Changed == Source.Get()) EXPECT_TRUE(Rename(*Document().Function(), "Newer draft"));
	});
	std::string Error;
	const bool Applied = Document().Publish(Error);
	GetMaterialFunctionChangedEvent().Remove(Handle);
	ASSERT_TRUE(Applied) << Error;
	EXPECT_EQ("Published", Source->GetFunctionSignature().Outputs.front().Name);
	EXPECT_EQ("Newer draft", Document().Function()->GetFunctionSignature().Outputs.front().Name);
	EXPECT_TRUE(Toolkit->IsAssetDirty(*Source));
	ASSERT_TRUE(Document().Publish(Error)) << Error;
	EXPECT_EQ("Newer draft", Source->GetFunctionSignature().Outputs.front().Name);
	EXPECT_FALSE(Toolkit->IsAssetDirty(*Source));
}

TEST_F(FMaterialFunctionDocumentTests, UndoRedoAfterApplyRepublishTheWorkingGraph)
{
	const auto Initial = Source->GetFunctionSignature().Outputs.front().Name;
	ASSERT_TRUE(Rename(*Document().Function(), "Applied transaction", true));
	std::string Error;
	ASSERT_TRUE(Document().Publish(Error)) << Error;
	ASSERT_TRUE(Toolkit->Undo());
	EXPECT_EQ(Initial, Source->GetFunctionSignature().Outputs.front().Name);
	EXPECT_FALSE(Toolkit->IsAssetDirty(*Source));
	ASSERT_TRUE(Toolkit->Redo());
	EXPECT_EQ("Applied transaction", Source->GetFunctionSignature().Outputs.front().Name);
	EXPECT_FALSE(Toolkit->IsAssetDirty(*Source));
	EXPECT_TRUE(Source->GetPackage()->IsDirty());
}

TEST_F(FMaterialFunctionDocumentTests, ExplicitReloadRejectsDirtyDraftAndRebasesCleanDocument)
{
	const auto Initial = Source->GetFunctionSignature().Outputs.front().Name;
	ASSERT_TRUE(Rename(*Document().Function(), "Unapplied"));
	auto Reload = [&] {
		auto* Previous = Document().GetSource()->GetPackage();
		FPackageReloadRequest Request{.Packages = {Previous}};
		EXPECT_TRUE(Host->GetAssetEditorSubsystem()->AddPackageReloadParticipants(Request));
		Source.Reset();
		const auto Result = ReloadPackages(Request).Wait();
		Source = Document().GetSource();
		return Result;
	};
	EXPECT_FALSE(Reload().Succeeded());
	EXPECT_EQ("Unapplied", Document().Function()->GetFunctionSignature().Outputs.front().Name);
	ASSERT_TRUE(Toolkit->DiscardAsset(*Source));
	ASSERT_TRUE(Rename(*Document().Function(), "Applied but unsaved"));
	std::string Error;
	ASSERT_TRUE(Document().Publish(Error)) << Error;
	const auto Result = Reload();
	ASSERT_TRUE(Result.Succeeded()) << (Result.Diagnostics.empty() ? "" : FormatPackageReloadDiagnostic(Result.Diagnostics.front()));
	EXPECT_EQ(Initial, Source->GetFunctionSignature().Outputs.front().Name);
	EXPECT_EQ(Initial, Document().Function()->GetFunctionSignature().Outputs.front().Name);
	EXPECT_FALSE(Toolkit->IsAssetDirty(*Source));
}

TEST_F(FMaterialFunctionDocumentTests, GlobalHistoryPublishesOnlyTheAffectedDocument)
{
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/FunctionSessions/" + FGuid::NewGuid().ToString(), Path));
	DMaterialFunction* OtherSource = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, OtherSource));
	ASSERT_TRUE(SavePackage(OtherSource->GetPackage()));
	ASSERT_TRUE(Host->Open(OtherSource->GetObjectPath(), OtherSource->GetClass()->GetQualifiedName().ToString()));
	const auto OtherToolkit = Host->GetAssetEditorSubsystem()->GetActiveAssetToolkit();
	auto& OtherDocument = FMaterialFunctionEditorTestAccess::Document(*OtherToolkit, OtherSource->GetObjectPath());
	const auto FirstInitial = Source->GetFunctionSignature().Outputs.front().Name;
	const auto OtherInitial = OtherSource->GetFunctionSignature().Outputs.front().Name;
	ASSERT_TRUE(Rename(*OtherDocument.Function(), "Other transaction", true));
	ASSERT_TRUE(Rename(*Document().Function(), "Unrelated draft"));
	ASSERT_TRUE(Toolkit->Undo());
	EXPECT_EQ(OtherInitial, OtherSource->GetFunctionSignature().Outputs.front().Name);
	EXPECT_EQ(FirstInitial, Source->GetFunctionSignature().Outputs.front().Name);
	EXPECT_TRUE(Document().IsDirty());
	ASSERT_TRUE(Toolkit->Redo());
	EXPECT_EQ("Other transaction", OtherSource->GetFunctionSignature().Outputs.front().Name);
	EXPECT_EQ(FirstInitial, Source->GetFunctionSignature().Outputs.front().Name);
	EXPECT_TRUE(Document().IsDirty());
	ASSERT_TRUE(Toolkit->DiscardAsset(*Source));
}

TEST_F(FMaterialFunctionDocumentTests, ReloadAdmissionRejectsPublicationAndChangesAfterPreparation)
{
	FObjectReplacementMap Map;
	const std::array Entries{
		FObjectReplacementMap::FEntry{.Previous = Source->GetPackage(), .Replacement = Document().WorkingPackage.Get()},
		FObjectReplacementMap::FEntry{.Previous = Source.Get(), .Replacement = Document().Function()}};
	ASSERT_TRUE(Map.BuildFromObjects(Entries));
	auto Participant = Toolkit->CreateReloadParticipant();
	ASSERT_TRUE(Participant->Prepare(Map));
	ASSERT_TRUE(Participant->Validate());
	ASSERT_TRUE(Rename(*Document().Function(), "Changed after prepare"));
	EXPECT_FALSE(Participant->Validate());
	Participant->Abort();
	int Notifications = 0;
	const auto Handle = GetMaterialFunctionChangedEvent().AddLambda([&](const DMaterialFunctionInterface& Changed) {
		if (&Changed != Source.Get()) return;
		++Notifications;
		auto Reentrant = Toolkit->CreateReloadParticipant();
		const auto Prepared = Reentrant->Prepare(Map);
		EXPECT_FALSE(Prepared.has_value());
		if (!Prepared) EXPECT_EQ(EObjectReplacementError::Busy, Prepared.error().Code);
		Reentrant->Abort();
	});
	std::string Error;
	const bool Applied = Document().Publish(Error);
	GetMaterialFunctionChangedEvent().Remove(Handle);
	ASSERT_TRUE(Applied) << Error;
	EXPECT_EQ(1, Notifications);
}
