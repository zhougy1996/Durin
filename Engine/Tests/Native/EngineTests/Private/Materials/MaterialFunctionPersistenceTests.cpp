#include "FunctionPortTestFixture.h"
#include "MaterialFunctionTestSupport.h"

TEST(FMaterialFunctionPersistenceTests, WorkspaceSavesAndReloadsFunctionsAcrossOpenDocuments)
{
	using namespace Durin;
	using namespace Durin::Editor;
	using namespace Durin::Editor::Material;
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
	ASSERT_TRUE(FMaterialGraphDocument(*Second).InsertFunctionCall(*First, 0, 300));
	ASSERT_TRUE(SavePackage(First->GetPackage()));
	ASSERT_TRUE(SavePackage(Second->GetPackage()));
	FWorkspaceManager Manager;
	DThumbnailManager Thumbnails;
	FMaterialEditorModule Module;
	FModuleTestHarness Harness("MaterialEditor");
	Harness.Start(Module);
	ASSERT_TRUE(Module.RegisterMaterialEditor(Manager, Thumbnails));
	const auto Class = DMaterialFunction::StaticClass()->GetQualifiedName().ToString();
	const auto Resource = First->GetObjectPath();
	ASSERT_TRUE(Manager.OpenAsset(Resource, Class));
	ASSERT_TRUE(Manager.OpenAsset(Resource, Class));
	ASSERT_EQ(Manager.GetDocuments().size(), 1u);
	const auto FirstTab = *Manager.GetActiveDocument();
	ASSERT_TRUE(Manager.OpenAsset(Second->GetObjectPath(), Class));
	ASSERT_EQ(Manager.GetDocuments().size(), 2u);
	const auto Workspace = Manager.FindWorkspace(FWorkspaceTypeId("MaterialFunctionEditor"));
	ASSERT_TRUE(Workspace);
	auto Signature = First->GetFunctionSignature();
	Signature.Outputs[0].Name = "Saved Surface";
	ASSERT_TRUE(FMaterialGraphDocument(*First).SetPort(true, Signature.Outputs[0]));
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
	ASSERT_TRUE(FMaterialGraphDocument(*First).SetPort(true, Signature.Outputs[0]));
	EXPECT_TRUE(PreviewInvalidation.ConsumeRefreshRequest());
	const bool Discarded = Workspace->DiscardDocument(FirstTab);
	if (!Discarded)
	{
		const auto Error = std::string(static_cast<MMaterialFunctionEditor*>(Workspace.get())->GetLastError());
		Module.UnregisterMaterialEditor(); Harness.Shutdown();
		FAIL() << Error;
	}
	DMaterialFunction* Reloaded = nullptr;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(FirstPath), Reloaded));
	ASSERT_NE(Reloaded, nullptr);
	EXPECT_EQ(Reloaded->GetFunctionSignature().Outputs[0].Name, "Saved Surface");
	EXPECT_EQ(GetFunctionCalls(*Second)[0]->Function.Get(), Reloaded);
	EXPECT_TRUE(PreviewInvalidation.ConsumeRefreshRequest());
	const auto Pasted = FMaterialGraphDocument(*Second).Paste(Clipboard, 300, 300);
	EXPECT_TRUE(Pasted) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Pasted);
	EXPECT_EQ(GetFunctionCalls(*Second).back()->Function.Get(), Reloaded);
	EXPECT_FALSE(Workspace->IsDocumentDirty(FirstTab));
	Module.UnregisterMaterialEditor();
	Harness.Shutdown();
}
