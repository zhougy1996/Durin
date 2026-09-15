#include "MaterialFunctionTestSupport.h"

TEST(FMaterialFunctionEditingTests, WorkspaceSavesAndReloadsFunctionsAcrossOpenDocuments)
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
	ASSERT_TRUE(FMaterialGraphDocument(*First).SetSignature(Signature));
	EXPECT_TRUE(Workspace->IsDocumentDirty(FirstTab));
	EXPECT_EQ(Manager.RequestCloseDocument(FirstTab.Id), EDocumentCloseResult::PendingConfirmation);
	ASSERT_TRUE(Workspace->SaveDocument(FirstTab));
	EXPECT_FALSE(Workspace->IsDocumentDirty(FirstTab));
	const auto CopiedCallId = GetFunctionCalls(*Second)[0]->Id;
	FMaterialGraphClipboardPayload Clipboard;
	ASSERT_TRUE(FMaterialGraphDocument(*Second).CopySelection(std::span(&CopiedCallId, 1), Clipboard));
	Signature.Outputs[0].Name = "Discarded Surface";
	ASSERT_TRUE(FMaterialGraphDocument(*First).SetSignature(Signature));
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
	const auto Pasted = FMaterialGraphDocument(*Second).Paste(Clipboard, 300, 300);
	EXPECT_TRUE(Pasted) << Pasted.Message;
	EXPECT_EQ(GetFunctionCalls(*Second).back()->Function.Get(), Reloaded);
	EXPECT_FALSE(Workspace->IsDocumentDirty(FirstTab));
	Module.UnregisterMaterialEditor();
	Harness.Shutdown();
}

TEST(FMaterialFunctionEditingTests, PreviewWrappersCompileEveryOutputTypeWithoutChangingTheFunction)
{
	using namespace Durin;
	using namespace Durin::Editor::Material;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	auto* Function = NewObject<DMaterialFunction>(nullptr, "PreviewFunction");
	auto* Preview = NewObject<DMaterial>(nullptr, "PreviewWrapper");
	Preview->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	for (uint32 Index = 0; Index < 6; ++Index)
	{
		const auto Type = static_cast<EMaterialProgramValueType>(Index);
		auto Input = FunctionPort(1, Type, "Input");
		Input.bRequired = true;
		const auto Output = FunctionPort(2, Type, "Output");
		const FMaterialFunctionSignature Signature{{Input}, {Output}};
		auto* InputExpression = NewObject<DMaterialExpressionFunctionInput>(nullptr, NAME_None);
		InputExpression->Id = {1, 1, 1, 1}; InputExpression->PortId = Input.Id;
		auto* OutputExpression = NewObject<DMaterialExpressionFunctionOutput>(nullptr, NAME_None);
		OutputExpression->Id = {1, 1, 1, 2}; OutputExpression->PortId = Output.Id; OutputExpression->Source = {InputExpression->Id};
		ASSERT_TRUE(Function->SetFunctionExpressions(Signature, std::array<DMaterialExpression*, 2>{InputExpression, OutputExpression}));
		const auto SourceExpressions = Function->GetExpressionCollection().Expressions;
		const auto Revision = Function->GetFunctionRevision();
		const auto Built = BuildMaterialFunctionPreview(*Function, Output.Id, *Preview);
		ASSERT_TRUE(Built) << Built.Message;
		const auto& Expressions = Preview->GetExpressionCollection().Expressions;
		ASSERT_FALSE(Expressions.empty());
		const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expressions.front().Get());
		ASSERT_NE(Call, nullptr);
		EXPECT_EQ(Call->Outputs[0].OutputId, Output.Id);
		for (const auto& Expression : Expressions) EXPECT_EQ(Expression->GetOuter(), Preview);
		ASSERT_TRUE(Preview->CompileEdits());
		ASSERT_TRUE(Preview->GetAcceptedCompiledProgram());
		EXPECT_EQ(Function->GetFunctionSignature(), Signature);
		EXPECT_EQ(Function->GetExpressionCollection().Expressions, SourceExpressions);
		EXPECT_EQ(Function->GetFunctionRevision(), Revision);
		const auto Before = Preview->GetExpressionCollection().Expressions;
		const auto BeforeOutputs = Preview->GetExpressionOutputs();
		const auto BeforeRevision = Preview->GetMaterialCompileStatus().AuthoredRevision;
		EXPECT_FALSE(BuildMaterialFunctionPreview(*Function, FGuid::NewGuid(), *Preview));
		EXPECT_EQ(Preview->GetExpressionCollection().Expressions, Before);
		EXPECT_EQ(Preview->GetExpressionOutputs(), BeforeOutputs);
		EXPECT_EQ(Preview->GetMaterialCompileStatus().AuthoredRevision, BeforeRevision);
	}
	MarkAsGarbage(Preview); MarkAsGarbage(Function); CollectGarbage();
}

TEST(FMaterialFunctionEditingTests, CallInsertionBindsRequiredInputsAndAdmitsNewOutputPorts)
{
	using namespace Durin;
	using namespace Durin::Editor::Material;
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "RequiredCallFunction");
	auto* Material = NewObject<DMaterial>(nullptr, "RequiredCallMaterial");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Graph(*Function), Root(*Material);
	auto Port = FunctionPort(50, EMaterialProgramValueType::Float, "Required Amount");
	Port.bRequired = true;
	const auto Input = Graph.AddPort(false, Port);
	ASSERT_TRUE(Input);
	const auto Constant = Testing::CreateGraphConstant(Root, 0.6f);
	ASSERT_TRUE(Constant);
	EXPECT_FALSE(Root.InsertFunctionCall(*Function, 0, 0));
	const std::array Inputs{FMaterialFunctionInputBinding{Port.Id, Port.Type, {Constant.GeneratedNodeIds[0]}}};
	const auto Call = Root.InsertFunctionCall(*Function, 0, 0, Inputs);
	ASSERT_TRUE(Call);
	const auto Output = FunctionPort(51, EMaterialProgramValueType::Float, "New Amount");
	ASSERT_TRUE(Graph.AddPort(true, Output, {Input.GeneratedNodeIds[0]}));
	ASSERT_EQ(GetFunctionCalls(*Material)[0]->Outputs.size(), 1u);
	ASSERT_TRUE(Root.AssignMaterialOutput(EMaterialSurfaceOutput::Roughness, {Call.GeneratedNodeIds[0], 0, Output.Id}));
	ASSERT_EQ(GetFunctionCalls(*Material)[0]->Outputs.size(), 2u);
	EXPECT_EQ(Material->GetExpressionOutputs().Roughness.OutputId, Output.Id);
	MarkAsGarbage(Material); MarkAsGarbage(Function); CollectGarbage();
}

TEST(FMaterialFunctionEditingTests, SharedDocumentsEditStableCallsAndInterfacesWithUndo)
{
	using namespace Durin;
	using namespace Durin::Editor::Material;
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "CommandFunction");
	auto* Wrapper = NewObject<DMaterialFunction>(nullptr, "CommandWrapper");
	auto* Material = NewObject<DMaterial>(nullptr, "CommandMaterial");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	Tests::FTestTransactorOwner Transactions;
	FMaterialGraphDocument FunctionDocument(*Function), WrapperDocument(*Wrapper), MaterialDocument(*Material);
	auto Signature = Function->GetFunctionSignature();
	Signature.Inputs[0].Name = "Renamed Surface";
	ASSERT_TRUE(FunctionDocument.SetSignature(Signature, Transactions.Get()));
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_NE(Function->GetFunctionSignature().Inputs[0].Name, Signature.Inputs[0].Name);
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Function->GetFunctionSignature(), Signature);
	const auto Nested = WrapperDocument.InsertFunctionCall(*Function, 10, 20, Transactions.Get());
	ASSERT_TRUE(Nested);
	EXPECT_FALSE(FunctionDocument.InsertFunctionCall(*Wrapper, 0, 0));
	const auto Inserted = MaterialDocument.InsertFunctionCall(*Wrapper, 30, 40, Transactions.Get());
	ASSERT_TRUE(Inserted);
	ASSERT_EQ(GetFunctionCalls(*Material).size(), 1u);
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_TRUE(GetFunctionCalls(*Material).empty());
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(GetFunctionCalls(*Material)[0]->Id, Inserted.GeneratedNodeIds[0]);
	FMaterialGraphDocumentState State;
	ASSERT_TRUE(WrapperDocument.Capture(State));
	const auto InputNode = State.Expressions[0]->Id;
	const auto InputPort = Function->GetFunctionSignature().Inputs[0].Id;
	ASSERT_TRUE(WrapperDocument.ConnectCallInput(Nested.GeneratedNodeIds[0], InputPort, {InputNode}, false, Transactions.Get()));
	EXPECT_EQ(GetFunctionCalls(*Wrapper)[0]->Inputs[0].InputId, InputPort);
	ASSERT_TRUE(WrapperDocument.DisconnectCallInput(Nested.GeneratedNodeIds[0], InputPort, Transactions.Get()));
	EXPECT_TRUE(GetFunctionCalls(*Wrapper)[0]->Inputs.empty());
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(GetFunctionCalls(*Wrapper)[0]->Inputs[0].Input.ExpressionId, InputNode);
	ASSERT_TRUE(MaterialDocument.Capture(State));
	const auto Before = State;
	State.Expressions.emplace_back(Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>({73, 1, 2, 3}).Get());
	ASSERT_TRUE(MaterialDocument.Commit(State, "Add Numeric Constant"));
	EXPECT_FALSE(MaterialDocument.ConnectCallInput(Inserted.GeneratedNodeIds[0],
		Wrapper->GetFunctionSignature().Inputs[0].Id, {{73, 1, 2, 3}}));
	EXPECT_TRUE(GetFunctionCalls(*Material)[0]->Inputs.empty());
	ASSERT_TRUE(MaterialDocument.Commit(Before, "Remove Numeric Constant"));
	MarkAsGarbage(Material);
	MarkAsGarbage(Wrapper);
	MarkAsGarbage(Function);
	CollectGarbage();
}

TEST(FMaterialFunctionEditingTests, CommandsAuthorFunctionPortsAndCompileASelectedOutput)
{
	using namespace Durin;
	using namespace Durin::Editor::Material;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	auto* Function = NewObject<DMaterialFunction>(nullptr, "AuthoredFunction");
	auto* Material = NewObject<DMaterial>(nullptr, "AuthoredFunctionCaller");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Function), Caller(*Material);
	Tests::FTestTransactorOwner Transactions;
	auto Input = FunctionPort(90, EMaterialProgramValueType::Float, "Amount");
	Input.Default = {.Kind = EMaterialFunctionDefaultKind::Numeric, .Numeric = {.X = 0.37f}};
	const auto AddedInput = Document.AddPort(false, Input, {}, -200, 0, Transactions.Get());
	ASSERT_TRUE(AddedInput);
	for (const auto& Expression : Function->GetExpressionCollection().Expressions)
		EXPECT_NE(std::ranges::find(Function->GetFunctionPresentation().Nodes, Expression->Id,
			&FMaterialGraphNodePresentation::NodeId), Function->GetFunctionPresentation().Nodes.end());
	const auto Terminal = std::ranges::find(Function->GetExpressionCollection().Expressions, AddedInput.GeneratedNodeIds[0],
		[](const auto& Expression) { return Expression->Id; });
	ASSERT_NE(Terminal, Function->GetExpressionCollection().Expressions.end());
	ASSERT_NE(Cast<DMaterialExpressionFunctionInput>(Terminal->Get()), nullptr);
	EXPECT_EQ(Cast<DMaterialExpressionFunctionInput>(Terminal->Get())->PortId, Input.Id);
	const auto AddedNode = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::Saturate,
		EMaterialProgramValueType::Float, {AddedInput.GeneratedNodeIds[0]}, 100, 0, Transactions.Get());
	ASSERT_TRUE(AddedNode);
	const auto Output = FunctionPort(91, EMaterialProgramValueType::Float, "Clamped Amount");
	const auto AddedOutput = Document.AddPort(true, Output, {AddedNode.GeneratedNodeIds[0]}, 400, 0, Transactions.Get());
	ASSERT_TRUE(AddedOutput);
	EXPECT_FALSE(Document.RemovePort(false, Input.Id));
	const auto Inserted = Caller.InsertFunctionCall(*Function, 0, 0, Transactions.Get());
	ASSERT_TRUE(Inserted);
	ASSERT_TRUE(Caller.AssignMaterialOutput(EMaterialSurfaceOutput::Roughness,
		{.SourceNodeId = Inserted.GeneratedNodeIds[0], .SourceOutputId = Output.Id}, Transactions.Get()));
	ASSERT_TRUE(Material->CompileEdits());
	ASSERT_TRUE(Material->GetAcceptedCompiledProgram());
	const auto Identity = Material->GetAcceptedCompiledProgram()->Identity;
	auto Signature = Function->GetFunctionSignature();
	std::ranges::reverse(Signature.Inputs);
	std::ranges::reverse(Signature.Outputs);
	ASSERT_TRUE(Document.SetSignature(Signature, Transactions.Get()));
	ASSERT_TRUE(Material->CompileEdits());
	EXPECT_EQ(Material->GetAcceptedCompiledProgram()->Identity, Identity);
	const auto CallId = Inserted.GeneratedNodeIds[0];
	ASSERT_TRUE(Caller.RemoveNodes(std::span(&CallId, 1), Transactions.Get()));
	EXPECT_TRUE(GetFunctionCalls(*Material).empty());
	EXPECT_FALSE(Material->GetExpressionOutputs().Roughness.ExpressionId.IsValid());
	ASSERT_TRUE(Transactions.Get()->Undo());
	ASSERT_EQ(GetFunctionCalls(*Material).size(), 1u);
	EXPECT_EQ(Material->GetExpressionOutputs().Roughness.OutputId, Output.Id);
	ASSERT_TRUE(Document.RemovePort(true, Output.Id));
	const auto NodeId = AddedNode.GeneratedNodeIds[0];
	ASSERT_TRUE(Document.RemoveNodes(std::span(&NodeId, 1)));
	ASSERT_TRUE(Document.RemovePort(false, Input.Id));
	MarkAsGarbage(Material);
	MarkAsGarbage(Function);
	CollectGarbage();
}
