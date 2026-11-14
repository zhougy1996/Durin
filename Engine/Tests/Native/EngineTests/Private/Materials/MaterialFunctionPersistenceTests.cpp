#include "Editor/AssetEditorTestHost.h"
#include "Misc/UnifiedError.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "FunctionPortTestFixture.h"
#include "MaterialFunctionTestSupport.h"

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
	ASSERT_NE(nullptr, TestHost.GetService());
	const auto Class = DMaterialFunction::StaticClass()->GetQualifiedName().ToString();
	const auto Resource = First->GetObjectPath();
	ASSERT_TRUE(TestHost.Open(Resource, Class));
	ASSERT_TRUE(TestHost.Open(Resource, Class));
	ASSERT_EQ(TestHost.GetService()->GetOwnedAssetEditors().size(), 1u);
	auto* FirstSession = TestHost.GetService()->GetOwnedAssetEditors().front().Get();
	auto& Manager = *FirstSession->GetDocumentManager();
	const auto FirstTab = *Manager.GetActiveDocument();
	ASSERT_TRUE(TestHost.Open(Second->GetObjectPath(), Class));
	ASSERT_EQ(TestHost.GetService()->GetOwnedAssetEditors().size(), 2u);
	auto Workspace = FirstSession->GetToolkit();
	ASSERT_TRUE(Workspace);
	auto Signature = First->GetFunctionSignature();
	Signature.Outputs[0].Name = "Saved Surface";
	ASSERT_TRUE(FMaterialGraphDocument(*First).SetPort(true, Signature.Outputs[0], Durin::ETransactionMode::NotTransactable));
	EXPECT_TRUE(Workspace->IsDocumentDirty(FirstTab));
	EXPECT_EQ(Manager.RequestCloseDocument(FirstTab.Id), EDocumentCloseResult::PendingConfirmation);
	ASSERT_TRUE(Workspace->SaveDocument(FirstTab));
	EXPECT_FALSE(Workspace->IsDocumentDirty(FirstTab));
	const auto CopiedCallId = GetFunctionCalls(*Second)[0]->Id;
	FMaterialGraphClipboardPayload Clipboard;
	ASSERT_TRUE(FMaterialGraphDocument(*Second).CopySelection(std::span(&CopiedCallId, 1), Clipboard));
	FMaterialFunctionPreviewInvalidation PreviewInvalidation;
	PreviewInvalidation.SetFunction(Second);
	EXPECT_TRUE(PreviewInvalidation.ConsumeRefreshRequest());
	Signature.Outputs[0].Name = "Discarded Surface";
	ASSERT_TRUE(FMaterialGraphDocument(*First).SetPort(true, Signature.Outputs[0], Durin::ETransactionMode::NotTransactable));
	EXPECT_TRUE(PreviewInvalidation.ConsumeRefreshRequest());
	const bool Discarded = Workspace->DiscardDocument(FirstTab);
	if (!Discarded)
	{
		const auto Error = std::string(static_cast<MMaterialFunctionEditor*>(Workspace.get())->GetLastError());
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
	EXPECT_TRUE(PreviewInvalidation.ConsumeRefreshRequest());
	const auto Pasted = FMaterialGraphDocument(*Second).Paste(Clipboard, 300, 300, Durin::ETransactionMode::NotTransactable);
	EXPECT_TRUE(Pasted) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Pasted);
	EXPECT_EQ(GetFunctionCalls(*Second).back()->Function.Get(), Reloaded);
	EXPECT_FALSE(Workspace->IsDocumentDirty(FirstTab));
	TestHost.Shutdown(); Workspace.reset();
	Harness.Shutdown();
}
