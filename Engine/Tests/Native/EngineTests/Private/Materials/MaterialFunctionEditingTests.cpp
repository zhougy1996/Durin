#include "FunctionPortTestFixture.h"
#include "MaterialFunctionTestSupport.h"

TEST(FMaterialFunctionEditingTests, UnfinishedCallsRemainEditableAndCompileAfterConnecting)
{
	using namespace Durin;
	using namespace Durin::Editor::Material;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, "UnfinishedFunction"));
	TStrongObjectPtr<DMaterialFunction> Wrapper(NewObject<DMaterialFunction>(nullptr, "UnfinishedWrapper"));
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "UnfinishedCaller"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Graph(*Function), Nested(*Wrapper), Root(*Material);
	Tests::FTestTransactorOwner Transactions;
	auto Input = FunctionPort(401, EMaterialProgramValueType::Float, "Amount");
	Input.bRequired = true;
	const auto InputNode = Graph.AddPort(false, Input);
	ASSERT_TRUE(InputNode);
	const auto Output = FunctionPort(402, EMaterialProgramValueType::Float, "Result");
	const auto OutputNode = Graph.AddPort(true, Output, {}, 10, 20, Transactions.Get());
	ASSERT_TRUE(OutputNode);
	ASSERT_TRUE(Transactions.Get()->Undo());
	ASSERT_TRUE(Transactions.Get()->Redo());
	const auto Call = Nested.InsertFunctionCall(*Function, 30, 40, Transactions.Get());
	ASSERT_TRUE(Call) << Call.Message;
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_TRUE(GetFunctionCalls(*Wrapper).empty());
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(GetFunctionCalls(*Wrapper)[0]->Id, Call.GeneratedNodeIds[0]);
	// An unfinished dependency can be referenced and copied, but cannot recurse.
	ASSERT_TRUE(Root.InsertFunctionCall(*Wrapper, 0, 0));
	EXPECT_FALSE(Graph.InsertFunctionCall(*Wrapper, 0, 0));
	FMaterialGraphClipboardPayload Clipboard;
	ASSERT_TRUE(Nested.CopySelection(Call.GeneratedNodeIds, Clipboard));
	const auto Pasted = Nested.Paste(Clipboard, 60, 80);
	ASSERT_TRUE(Pasted) << Pasted.Message;
	ASSERT_TRUE(Nested.RemoveNodes(Pasted.GeneratedNodeIds));
	const std::array<DMaterialFunctionInterface*, 1> Roots{Wrapper.Get()};
	std::vector<FMaterialFunctionOwnerStamp> Closure;
	EXPECT_TRUE(ValidateMaterialFunctionDependencies(Roots, Closure, EMaterialFunctionValidationMode::Editing));
	EXPECT_FALSE(ValidateMaterialFunctionDependencies(Roots, Closure));
	ASSERT_TRUE(Graph.ConnectInput(OutputNode.GeneratedNodeIds[0], 0, {InputNode.GeneratedNodeIds[0]}));
	const auto Constant = Testing::CreateGraphConstant(Nested, 0.6f);
	ASSERT_TRUE(Constant);
	ASSERT_TRUE(Nested.ConnectCallInput(Call.GeneratedNodeIds[0], Input.Id, {Constant.GeneratedNodeIds[0]}, false, Transactions.Get()));
	EXPECT_TRUE(ValidateMaterialFunctionDependencies(Roots, Closure));
	ASSERT_TRUE(Nested.DisconnectCallInput(Call.GeneratedNodeIds[0], Input.Id, Transactions.Get()));
	EXPECT_FALSE(ValidateMaterialFunctionDependencies(Roots, Closure));
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_TRUE(ValidateMaterialFunctionDependencies(Roots, Closure));
	const auto RootCall = Root.InsertFunctionCall(*Function, 100, 200);
	ASSERT_TRUE(RootCall);
	ASSERT_TRUE(Root.AssignMaterialOutput(EMaterialSurfaceOutput::Roughness, {RootCall.GeneratedNodeIds[0], 0, Output.Id}));
	EXPECT_FALSE(Material->CompileEdits());
	const auto RootConstant = Testing::CreateGraphConstant(Root, 0.6f);
	ASSERT_TRUE(RootConstant);
	ASSERT_TRUE(Root.ConnectCallInput(RootCall.GeneratedNodeIds[0], Input.Id, {RootConstant.GeneratedNodeIds[0]}));
	EXPECT_TRUE(Material->CompileEdits());
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
	const auto Unbound = Root.InsertFunctionCall(*Function, 0, 0);
	ASSERT_TRUE(Unbound);
	ASSERT_TRUE(Root.RemoveNodes(Unbound.GeneratedNodeIds));
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
	ASSERT_TRUE(FunctionDocument.SetPort(false, Signature.Inputs[0], Transactions.Get()));
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
	const auto InputNode = Wrapper->GetExpressionCollection().Expressions[0]->Id;
	const auto InputPort = Function->GetFunctionSignature().Inputs[0].Id;
	ASSERT_TRUE(WrapperDocument.ConnectCallInput(Nested.GeneratedNodeIds[0], InputPort, {InputNode}, false, Transactions.Get()));
	EXPECT_EQ(GetFunctionCalls(*Wrapper)[0]->Inputs[0].InputId, InputPort);
	ASSERT_TRUE(WrapperDocument.DisconnectCallInput(Nested.GeneratedNodeIds[0], InputPort, Transactions.Get()));
	EXPECT_TRUE(GetFunctionCalls(*Wrapper)[0]->Inputs.empty());
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(GetFunctionCalls(*Wrapper)[0]->Inputs[0].Input.ExpressionId, InputNode);
	auto Constant = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>({73, 1, 2, 3});
	ASSERT_TRUE(MaterialDocument.CreateExpression(*Constant));
	ASSERT_TRUE(MaterialDocument.ConnectCallInput(Inserted.GeneratedNodeIds[0],
		Wrapper->GetFunctionSignature().Inputs[0].Id, {{73, 1, 2, 3}}));
	EXPECT_EQ(GetFunctionCalls(*Material)[0]->Inputs[0].Input.ExpressionId, (FGuid{73, 1, 2, 3}));
	ASSERT_TRUE(MaterialDocument.RemoveNodes(std::span(&Constant->Id, 1)));
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
	EXPECT_EQ(Cast<DMaterialExpressionFunctionInput>(Terminal->Get())->Port.Id, Input.Id);
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
	for (auto& Port : Signature.Inputs) Port.DisplayOrder = -Port.DisplayOrder - 1;
	for (auto& Port : Signature.Outputs) Port.DisplayOrder = -Port.DisplayOrder - 1;
	for (const auto& Port : Signature.Inputs) ASSERT_TRUE(Document.SetPort(false, Port, Transactions.Get()));
	for (const auto& Port : Signature.Outputs) ASSERT_TRUE(Document.SetPort(true, Port, Transactions.Get()));
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

TEST(FMaterialFunctionEditingTests, TerminalPortIsTheOnlyAuthoredInterfaceAndUndoRestoresIt)
{
	using namespace Durin;
	using namespace Durin::Editor::Material;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, "NodeOwnedInterface"));
	EXPECT_EQ(Function->GetClass()->FindPropertyByName("Signature"), nullptr);
	EXPECT_EQ(DMaterialExpressionFunctionInput::StaticClass()->FindPropertyByName("PortId"), nullptr);
	FMaterialGraphDocument Document(*Function.Get());
	Tests::FTestTransactorOwner Transactions;
	const auto Before = Function->GetFunctionSignature();
	const auto* Input = Cast<DMaterialExpressionFunctionInput>(Function->GetExpressionCollection().Expressions[0].Get());
	ASSERT_NE(Input, nullptr);
	const auto NodeId = Input->Id;
	auto Edited = Input->Port;
	Edited.Name = "Authored on node";
	Edited.Default.Surface.RoughnessDefault.X = .27f;
	std::vector<FMaterialGraphChangeSet> Events;
	const auto Handle = Function->GetGraphChanges().Subscribe(*Function.Get(), [&](const auto& Change) {
		Events.push_back(Change);
		EXPECT_EQ(Function->GetFunctionSignature().Inputs[0], Edited);
	});
	const auto Result = Document.SetPort(false, Edited, Transactions.Get());
	Function->GetGraphChanges().Unsubscribe(Handle);
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_EQ(Events.size(), 1u);
	ASSERT_EQ(Events[0].Nodes.size(), 1u);
	EXPECT_EQ(Events[0].Flags, EMaterialGraphChange::None);
	EXPECT_EQ(Events[0].Nodes[0].NodeId, NodeId);
	EXPECT_NE(Events[0].Nodes[0].Flags & EMaterialGraphNodeChange::Interface, EMaterialGraphNodeChange::None);
	EXPECT_EQ(Function->GetFunctionSignature().Inputs[0], Edited);
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Function->GetFunctionSignature(), Before);
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Function->GetFunctionSignature().Inputs[0], Edited);
	auto Output = Function->GetFunctionSignature().Outputs[0];
	Output.Id = Edited.Id;
	EXPECT_FALSE(Document.SetPort(true, Output, Transactions.Get()));
	EXPECT_EQ(Function->GetFunctionSignature().Outputs[0], Before.Outputs[0]);
}
