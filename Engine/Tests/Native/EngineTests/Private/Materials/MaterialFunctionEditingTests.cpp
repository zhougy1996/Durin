#include "Graph/MaterialGraphEditSession.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "MaterialGraph.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialShaderMap.h"
#include "FunctionPortTestFixture.h"
#include "MaterialFunctionTestSupport.h"

TEST(FMaterialFunctionEditingTests, DuplicateUsesGraphOffsetsAndRejectsOutOfRangePositions)
{
	using namespace Durin;
	using namespace Durin::MaterialEditor;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, "DuplicateFunction"));
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Function));
	Tests::FTestEditorTransactorOwner Transactions;
	const auto Created = Testing::CreateGraphConstant(*AuthoringGraph, 0.5f, 500, 300);
	ASSERT_TRUE(Created);
	const auto Before = Function->GetFunctionPresentation();
	const auto Duplicated = AuthoringGraph->DuplicateNodes(std::array{Created.value()}, -40, 60).transform(Testing::NodeIds);
	ASSERT_TRUE(Duplicated) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Duplicated);
	ASSERT_EQ(Duplicated->size(), 1u);
	const auto After = Function->GetFunctionPresentation();
	const auto Position = std::ranges::find(After.Nodes, Duplicated->front(), &FMaterialGraphNodePresentation::NodeId);
	ASSERT_NE(Position, After.Nodes.end());
	EXPECT_EQ(Position->X, 460);
	EXPECT_EQ(Position->Y, 360);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Function->GetFunctionPresentation(), Before);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Function->GetFunctionPresentation(), After);
	Transactions->Reset();
	const auto Expressions = Function->GetExpressionCollection().Expressions;
	for (const auto Offset : {std::numeric_limits<int32>::min(), std::numeric_limits<int32>::max()})
	{
		EXPECT_FALSE(AuthoringGraph->DuplicateNodes(std::array{Created.value()}, Offset, 0));
		EXPECT_FALSE(AuthoringGraph->DuplicateNodes(std::array{Created.value()}, 0, Offset));
	}
	// The anchor fits, but a later node would exceed the coordinate bound.
	const std::array Selection{Created.value(), Duplicated->front()};
	EXPECT_FALSE(AuthoringGraph->DuplicateNodes(Selection, MaterialGraphPresentationCoordinateLimit - 460, 0));
	EXPECT_EQ(Function->GetExpressionCollection().Expressions, Expressions);
	EXPECT_EQ(Function->GetFunctionPresentation(), After);
	EXPECT_FALSE(Transactions->Undo());
}

TEST(FMaterialFunctionEditingTests, PasteRejectsRecursiveDependenciesWithoutPublishingOrRecordingUndo)
{
	using namespace Durin;
	using namespace Durin::MaterialEditor;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, "PasteCycleRoot"));
	TStrongObjectPtr<DMaterialFunction> Wrapper(NewObject<DMaterialFunction>(nullptr, "PasteCycleWrapper"));
	TStrongObjectPtr<DMaterialFunction> Donor(NewObject<DMaterialFunction>(nullptr, "PasteCycleDonor"));
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Function));
	TStrongObjectPtr<DMaterialGraph> NestedAuthoringGraph(DMaterialGraph::Create(*Wrapper));
	TStrongObjectPtr<DMaterialGraph> SourceAuthoringGraph(DMaterialGraph::Create(*Donor));
	ASSERT_TRUE(Testing::CreateGraphConstant(*AuthoringGraph, 0.5f));
	const auto Direct = NestedAuthoringGraph->InsertFunctionCall(*Function, 10, 20, Durin::ETransactionMode::NotTransactable).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Direct);
	const auto Indirect = SourceAuthoringGraph->InsertFunctionCall(*Wrapper, 30, 40, Durin::ETransactionMode::NotTransactable).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Indirect);
	FMaterialGraphClipboardPayload DirectPayload, IndirectPayload;
	ASSERT_TRUE(NestedAuthoringGraph->CopySelection(std::array{Direct.value()}, DirectPayload));
	ASSERT_TRUE(SourceAuthoringGraph->CopySelection(std::array{Indirect.value()}, IndirectPayload));
	const auto Expressions = Function->GetExpressionCollection().Expressions;
	const auto Presentation = Function->GetFunctionPresentation();
	const auto Signature = Function->GetFunctionSignature();
	Tests::FTestEditorTransactorOwner Transactions;
	int Notifications = 0;
	const auto Observer = Function->GetGraphChanges().Subscribe(*Function,
		[&](const FMaterialGraphChangeSet&) { ++Notifications; });
	for (const auto* Payload : {&DirectPayload, &IndirectPayload})
	{
		const auto Rejected = AuthoringGraph->Paste(*Payload, 100, 200);
		ASSERT_FALSE(Rejected);
		EXPECT_EQ(Rejected.error().GetStatus(), EMaterialGraphCommandStatus::Rejected);
		EXPECT_EQ(Function->GetExpressionCollection().Expressions, Expressions);
		EXPECT_EQ(Function->GetFunctionPresentation(), Presentation);
		EXPECT_EQ(Function->GetFunctionSignature(), Signature);
		EXPECT_EQ(Notifications, 0);
		EXPECT_FALSE(Transactions->Undo());
	}
	Function->GetGraphChanges().Unsubscribe(Observer);
}

TEST(FMaterialFunctionEditingTests, UnfinishedCallsRemainEditableAndCompileAfterConnecting)
{
	using namespace Durin;
	using namespace Durin::MaterialEditor;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, "UnfinishedFunction"));
	TStrongObjectPtr<DMaterialFunction> Wrapper(NewObject<DMaterialFunction>(nullptr, "UnfinishedWrapper"));
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "UnfinishedCaller"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Function));
	TStrongObjectPtr<DMaterialGraph> NestedAuthoringGraph(DMaterialGraph::Create(*Wrapper));
	TStrongObjectPtr<DMaterialGraph> RootAuthoringGraph(DMaterialGraph::Create(*Material));
	Tests::FTestEditorTransactorOwner Transactions;
	auto Input = FunctionPort(401, EMaterialProgramValueType::Float, "Amount");
	Input.bRequired = true;
	const auto InputNode = AuthoringGraph->AddPort(false, Input, {}, 0, 0, Durin::ETransactionMode::NotTransactable).transform(Testing::GraphNodeId);
	ASSERT_TRUE(InputNode);
	const auto Output = FunctionPort(402, EMaterialProgramValueType::Float, "Result");
	const auto OutputNode = AuthoringGraph->AddPort(true, Output, {}, 10, 20).transform(Testing::GraphNodeId);
	ASSERT_TRUE(OutputNode);
	ASSERT_TRUE(Transactions.Get()->Undo());
	ASSERT_TRUE(Transactions.Get()->Redo());
	const auto Call = NestedAuthoringGraph->InsertFunctionCall(*Function, 30, 40, Durin::ETransactionMode::Record).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Call) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Call);
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_TRUE(GetFunctionCalls(*Wrapper).empty());
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(GetFunctionCalls(*Wrapper)[0]->Id, Call.value());
	// An unfinished dependency can be referenced and copied, but cannot recurse.
	ASSERT_TRUE(RootAuthoringGraph->InsertFunctionCall(*Wrapper, 0, 0, Durin::ETransactionMode::NotTransactable));
	EXPECT_FALSE(AuthoringGraph->InsertFunctionCall(*Wrapper, 0, 0, Durin::ETransactionMode::NotTransactable));
	FMaterialGraphClipboardPayload Clipboard;
	ASSERT_TRUE(NestedAuthoringGraph->CopySelection(std::array{Call.value()}, Clipboard));
	const auto Pasted = NestedAuthoringGraph->Paste(Clipboard, 60, 80, Durin::ETransactionMode::NotTransactable).transform(Testing::NodeIds);
	ASSERT_TRUE(Pasted) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Pasted);
	ASSERT_TRUE(NestedAuthoringGraph->RemoveNodes(Pasted.value(), Durin::ETransactionMode::NotTransactable));
	const std::array<DMaterialFunctionInterface*, 1> Roots{Wrapper.Get()};
	std::vector<FMaterialFunctionOwnerStamp> Closure;
	EXPECT_TRUE(ValidateMaterialFunctionDependencies(Roots, Closure, EMaterialFunctionValidationMode::Editing));
	EXPECT_FALSE(ValidateMaterialFunctionDependencies(Roots, Closure));
	ASSERT_TRUE(GraphEditInternals::WriteExpressionConnection(Function.Get(), FMaterialGraphPinAddress::Input(OutputNode.value(), 0), FMaterialGraphPinAddress::Output({InputNode.value()}), false, Durin::ETransactionMode::NotTransactable));
	const auto Constant = Testing::CreateGraphConstant(*NestedAuthoringGraph, 0.6f);
	ASSERT_TRUE(Constant);
	ASSERT_TRUE(GraphEditInternals::WriteExpressionConnection(Wrapper.Get(), FMaterialGraphPinAddress::Input(Call.value(), 0, Input.Id), FMaterialGraphPinAddress::Output({Constant.value()}), false, Durin::ETransactionMode::Record));
	EXPECT_TRUE(ValidateMaterialFunctionDependencies(Roots, Closure));
	ASSERT_TRUE(GraphEditInternals::WriteExpressionConnection(Wrapper.Get(), FMaterialGraphPinAddress::Input(Call.value(), 0, Input.Id), FMaterialGraphPinAddress::Output({}), true, Durin::ETransactionMode::Record));
	EXPECT_FALSE(ValidateMaterialFunctionDependencies(Roots, Closure));
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_TRUE(ValidateMaterialFunctionDependencies(Roots, Closure));
	const auto RootCall = RootAuthoringGraph->InsertFunctionCall(*Function, 100, 200, Durin::ETransactionMode::NotTransactable).transform(Testing::GraphNodeId);
	ASSERT_TRUE(RootCall);
	ASSERT_TRUE(RootAuthoringGraph->GetSchema().TryCreateConnection(*RootAuthoringGraph, FMaterialGraphPinAddress::MaterialOutput(EMaterialProperty::MP_Roughness), FMaterialGraphPinAddress::Output({RootCall.value(), 0, Output.Id}), true, Durin::ETransactionMode::NotTransactable));
	EXPECT_FALSE(Material->CompileEdits());
	const auto RootConstant = Testing::CreateGraphConstant(*RootAuthoringGraph, 0.6f);
	ASSERT_TRUE(RootConstant);
	ASSERT_TRUE(RootAuthoringGraph->GetSchema().TryCreateConnection(*RootAuthoringGraph, FMaterialGraphPinAddress::Input(RootCall.value(), 0, Input.Id), FMaterialGraphPinAddress::Output({RootConstant.value()}), false, Durin::ETransactionMode::NotTransactable));
	EXPECT_TRUE(Material->CompileEdits());
}

TEST(FMaterialFunctionEditingTests, CallInsertionBindsRequiredInputsAndAdmitsNewOutputPorts)
{
	using namespace Durin;
	using namespace Durin::MaterialEditor;
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "RequiredCallFunction");
	auto* Material = NewObject<DMaterial>(nullptr, "RequiredCallMaterial");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Function));
	TStrongObjectPtr<DMaterialGraph> RootAuthoringGraph(DMaterialGraph::Create(*Material));
	auto Port = FunctionPort(50, EMaterialProgramValueType::Float, "Required Amount");
	Port.bRequired = true;
	const auto Input = AuthoringGraph->AddPort(false, Port, {}, 0, 0, Durin::ETransactionMode::NotTransactable).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Input);
	const auto Constant = Testing::CreateGraphConstant(*RootAuthoringGraph, 0.6f);
	ASSERT_TRUE(Constant);
	const auto Unbound = RootAuthoringGraph->InsertFunctionCall(*Function, 0, 0, Durin::ETransactionMode::NotTransactable).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Unbound);
	ASSERT_TRUE(RootAuthoringGraph->RemoveNodes(std::array{Unbound.value()}, Durin::ETransactionMode::NotTransactable));
	const std::array Inputs{FMaterialFunctionInputBinding{Port.Id, Port.Type, {Constant.value()}}};
	const auto Call = RootAuthoringGraph->InsertFunctionCall(*Function, 0, 0, Inputs, Durin::ETransactionMode::NotTransactable).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Call);
	const auto Output = FunctionPort(51, EMaterialProgramValueType::Float, "New Amount");
	ASSERT_TRUE(AuthoringGraph->AddPort(true, Output, {Input.value()}, 0, 0, Durin::ETransactionMode::NotTransactable));
	ASSERT_EQ(GetFunctionCalls(*Material)[0]->FunctionOutputs.size(), 2u);
	ASSERT_TRUE(GraphEditInternals::WriteExpressionConnection(Material, FMaterialGraphPinAddress::MaterialOutput(EMaterialProperty::MP_Roughness), FMaterialGraphPinAddress::Output({Call.value(), 0, Output.Id}), true, Durin::ETransactionMode::NotTransactable));
	ASSERT_EQ(GetFunctionCalls(*Material)[0]->FunctionOutputs.size(), 2u);
	EXPECT_EQ(Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->Connection.GetOutputId(), Output.Id);
	MarkAsGarbage(Material); MarkAsGarbage(Function); CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialFunctionEditingTests, SharedDocumentsEditStableCallsAndInterfacesWithUndo)
{
	using namespace Durin;
	using namespace Durin::MaterialEditor;
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "CommandFunction");
	auto* Wrapper = NewObject<DMaterialFunction>(nullptr, "CommandWrapper");
	auto* Material = NewObject<DMaterial>(nullptr, "CommandMaterial");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	Tests::FTestEditorTransactorOwner Transactions;

	TStrongObjectPtr<DMaterialGraph> FunctionAuthoringGraph(DMaterialGraph::Create(*Function));
	TStrongObjectPtr<DMaterialGraph> WrapperDocumentAuthoringGraph(DMaterialGraph::Create(*Wrapper));
	TStrongObjectPtr<DMaterialGraph> MaterialAuthoringGraph(DMaterialGraph::Create(*Material));
	auto Signature = Function->GetFunctionSignature();
	Signature.Inputs[0].Name = "Renamed Surface";
	auto* InputTerminal = Testing::FindFunctionTerminal<DMaterialExpressionFunctionInput>(*FunctionAuthoringGraph, Signature.Inputs[0].Id);
	ASSERT_TRUE(InputTerminal);
	ASSERT_TRUE(InputTerminal->OnRenameNode(Signature.Inputs[0].Name));
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_NE(Function->GetFunctionSignature().Inputs[0].Name, Signature.Inputs[0].Name);
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Function->GetFunctionSignature(), Signature);
	const auto Nested = WrapperDocumentAuthoringGraph->InsertFunctionCall(*Function, 10, 20, Durin::ETransactionMode::Record).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Nested);
	EXPECT_FALSE(FunctionAuthoringGraph->InsertFunctionCall(*Wrapper, 0, 0, Durin::ETransactionMode::NotTransactable));
	const auto Inserted = MaterialAuthoringGraph->InsertFunctionCall(*Wrapper, 30, 40, Durin::ETransactionMode::Record).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Inserted);
	ASSERT_EQ(GetFunctionCalls(*Material).size(), 1u);
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_TRUE(GetFunctionCalls(*Material).empty());
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(GetFunctionCalls(*Material)[0]->Id, Inserted.value());
	const auto InputNode = Wrapper->GetExpressionCollection().Expressions[0]->Id;
	const auto InputPort = Function->GetFunctionSignature().Inputs[0].Id;
	ASSERT_TRUE(GraphEditInternals::WriteExpressionConnection(Wrapper, FMaterialGraphPinAddress::Input(Nested.value(), 0, InputPort), FMaterialGraphPinAddress::Output({InputNode}), false, Durin::ETransactionMode::Record));
	EXPECT_EQ(GetFunctionCalls(*Wrapper)[0]->FunctionInputs[0].InputId, InputPort);
	ASSERT_TRUE(GraphEditInternals::WriteExpressionConnection(Wrapper, FMaterialGraphPinAddress::Input(Nested.value(), 0, InputPort), FMaterialGraphPinAddress::Output({}), true, Durin::ETransactionMode::Record));
	EXPECT_FALSE(GetFunctionCalls(*Wrapper)[0]->FunctionInputs[0].Input.IsConnected());
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(GetFunctionCalls(*Wrapper)[0]->FunctionInputs[0].Input.GetExpressionId(), InputNode);
	auto Constant = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>({73, 1, 2, 3});
	ASSERT_TRUE(MaterialAuthoringGraph->CreateExpression(*Constant, 0, 0, Durin::ETransactionMode::NotTransactable));
	ASSERT_TRUE(GraphEditInternals::WriteExpressionConnection(Material, FMaterialGraphPinAddress::Input(Inserted.value(), 0, Wrapper->GetFunctionSignature().Inputs[0].Id), FMaterialGraphPinAddress::Output({{73, 1, 2, 3}}), false, Durin::ETransactionMode::NotTransactable));
	EXPECT_EQ(GetFunctionCalls(*Material)[0]->FunctionInputs[0].Input.GetExpressionId(), (FGuid{73, 1, 2, 3}));
	ASSERT_TRUE(MaterialAuthoringGraph->RemoveNodes(std::span(&Constant->Id, 1), Durin::ETransactionMode::NotTransactable));
	MarkAsGarbage(Material);
	MarkAsGarbage(Wrapper);
	MarkAsGarbage(Function);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialFunctionEditingTests, CommandsAuthorFunctionPortsAndCompileASelectedOutput)
{
	using namespace Durin;
	using namespace Durin::MaterialEditor;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	auto* Function = NewObject<DMaterialFunction>(nullptr, "AuthoredFunction");
	auto* Material = NewObject<DMaterial>(nullptr, "AuthoredFunctionCaller");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Function));
	TStrongObjectPtr<DMaterialGraph> CallerAuthoringGraph(DMaterialGraph::Create(*Material));
	Tests::FTestEditorTransactorOwner Transactions;
	auto Input = FunctionPort(90, EMaterialProgramValueType::Float, "Amount");
	Input.Default = {.Kind = EMaterialFunctionDefaultKind::Numeric, .Numeric = {.X = 0.37f}};
	const auto AddedInput = AuthoringGraph->AddPort(false, Input, {}, -200, 0).transform(Testing::GraphNodeId);
	ASSERT_TRUE(AddedInput);
	for (const auto& Expression : Function->GetExpressionCollection().Expressions)
		EXPECT_NE(std::ranges::find(Function->GetFunctionPresentation().Nodes, Expression->Id,
			&FMaterialGraphNodePresentation::NodeId), Function->GetFunctionPresentation().Nodes.end());
	const auto Terminal = std::ranges::find(Function->GetExpressionCollection().Expressions, AddedInput.value(),
		[](const auto& Expression) { return Expression->Id; });
	ASSERT_NE(Terminal, Function->GetExpressionCollection().Expressions.end());
	ASSERT_NE(Cast<DMaterialExpressionFunctionInput>(Terminal->Get()), nullptr);
	EXPECT_EQ(Cast<DMaterialExpressionFunctionInput>(Terminal->Get())->Port.Id, Input.Id);
	const auto AddedNode = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::Saturate, EMaterialProgramValueType::Float, {AddedInput.value()}, 100, 0, Durin::ETransactionMode::Record);
	ASSERT_TRUE(AddedNode);
	const auto Output = FunctionPort(91, EMaterialProgramValueType::Float, "Clamped Amount");
	const auto AddedOutput = AuthoringGraph->AddPort(true, Output, {AddedNode.value()[0]}, 400, 0).transform(Testing::GraphNodeId);
	ASSERT_TRUE(AddedOutput);
	EXPECT_FALSE(AuthoringGraph->RemovePort(false, Input.Id, Durin::ETransactionMode::NotTransactable));
	const auto Inserted = CallerAuthoringGraph->InsertFunctionCall(*Function, 0, 0, Durin::ETransactionMode::Record).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Inserted);
	ASSERT_TRUE(GraphEditInternals::WriteExpressionConnection(Material, FMaterialGraphPinAddress::MaterialOutput(EMaterialProperty::MP_Roughness), FMaterialGraphPinAddress::Output({.SourceNodeId = Inserted.value(), .SourceOutputId = Output.Id}), true, Durin::ETransactionMode::Record));
	ASSERT_TRUE(Material->CompileEdits());
	ASSERT_TRUE(Material->GetAcceptedCompiledProgram());
	const auto Identity = Material->GetAcceptedCompiledProgram()->GetShaderMap().ShaderMapId;
	auto Signature = Function->GetFunctionSignature();
	for (auto& Port : Signature.Inputs) Port.DisplayOrder = -Port.DisplayOrder - 1;
	for (auto& Port : Signature.Outputs) Port.DisplayOrder = -Port.DisplayOrder - 1;
	for (const auto& Port : Signature.Inputs)
	{
		auto* PortNode = Testing::FindFunctionTerminal<DMaterialExpressionFunctionInput>(*AuthoringGraph, Port.Id);
		ASSERT_TRUE(PortNode);
		ASSERT_TRUE(PortNode->SetFunctionPort(Port));
	}
	for (const auto& Port : Signature.Outputs)
	{
		auto* PortNode = Testing::FindFunctionTerminal<DMaterialExpressionFunctionOutput>(*AuthoringGraph, Port.Id);
		ASSERT_TRUE(PortNode);
		ASSERT_TRUE(PortNode->SetFunctionPort(Port));
	}
	ASSERT_TRUE(Material->CompileEdits());
	EXPECT_EQ(Material->GetAcceptedCompiledProgram()->GetShaderMap().ShaderMapId, Identity);
	const auto CallId = Inserted.value();
	ASSERT_TRUE(CallerAuthoringGraph->RemoveNodes(std::span(&CallId, 1), Durin::ETransactionMode::Record));
	EXPECT_TRUE(GetFunctionCalls(*Material).empty());
	EXPECT_FALSE(Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->Connection.IsConnected());
	ASSERT_TRUE(Transactions.Get()->Undo());
	ASSERT_EQ(GetFunctionCalls(*Material).size(), 1u);
	EXPECT_EQ(Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->Connection.GetOutputId(), Output.Id);
	const auto BeforeRemoval = Function->GetFunctionSignature();
	const auto* SourceExpression = AuthoringGraph->FindNode(AddedNode->front())->GetMaterialExpression();
	ASSERT_TRUE(AuthoringGraph->RemovePort(true, Output.Id));
	EXPECT_FALSE(AuthoringGraph->FindNode(AddedOutput.value()));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Function->GetFunctionSignature(), BeforeRemoval);
	const auto* RestoredNode = Testing::FindFunctionTerminal<DMaterialExpressionFunctionOutput>(*AuthoringGraph, Output.Id);
	ASSERT_TRUE(RestoredNode);
	EXPECT_EQ(Cast<DMaterialExpressionFunctionOutput>(RestoredNode->GetMaterialExpression())->Source.Expression.Get(), SourceExpression);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_FALSE(AuthoringGraph->FindNode(AddedOutput.value()));
	const auto NodeId = AddedNode.value()[0];
	ASSERT_TRUE(AuthoringGraph->RemoveNodes(std::span(&NodeId, 1), Durin::ETransactionMode::NotTransactable));
	ASSERT_TRUE(AuthoringGraph->RemovePort(false, Input.Id, Durin::ETransactionMode::NotTransactable));
	MarkAsGarbage(Material);
	MarkAsGarbage(Function);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialFunctionEditingTests, TerminalPortIsTheOnlyAuthoredInterfaceAndUndoRestoresIt)
{
	using namespace Durin;
	using namespace Durin::MaterialEditor;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, "NodeOwnedInterface"));
	EXPECT_EQ(Function->GetClass()->FindPropertyByName("Signature"), nullptr);
	EXPECT_EQ(DMaterialExpressionFunctionInput::StaticClass()->FindPropertyByName("PortId"), nullptr);
	TStrongObjectPtr<DMaterialGraph> FunctionGraph(DMaterialGraph::Create(*Function));
	Tests::FTestEditorTransactorOwner Transactions;
	const auto Before = Function->GetFunctionSignature();
	const auto* Input = Cast<DMaterialExpressionFunctionInput>(Function->GetExpressionCollection().Expressions[0].Get());
	ASSERT_NE(Input, nullptr);
	const auto NodeId = Input->Id;
	auto Edited = Input->Port;
	Edited.Name = "Authored on node";
	Edited.DisplayOrder = 17;
	Edited.Default.Surface.Get(EMaterialProperty::MP_Roughness).Literal.X = .27f;
	std::vector<FMaterialGraphChangeSet> Events;
	const auto Handle = Function->GetGraphChanges().Subscribe(*Function.Get(), [&](const auto& Change) {
		Events.push_back(Change);
		EXPECT_EQ(Function->GetFunctionSignature().Inputs[0], Edited);
	});
	auto* InputTerminal = FunctionGraph->FindNode(NodeId);
	ASSERT_TRUE(InputTerminal);
	const auto HistorySize = Transactions->GetQueueLength();
	const auto Result = InputTerminal->SetFunctionPort(Edited);
	Function->GetGraphChanges().Unsubscribe(Handle);
	ASSERT_TRUE(Result) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Result);
	EXPECT_EQ(Transactions->GetQueueLength(), HistorySize + 1);
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
	auto* OutputNode = Testing::FindFunctionTerminal<DMaterialExpressionFunctionOutput>(*FunctionGraph, Output.Id);
	ASSERT_TRUE(OutputNode);
	Output.Id = Edited.Id;
	EXPECT_FALSE(OutputNode->SetFunctionPort(Output));
	EXPECT_EQ(Function->GetFunctionSignature().Outputs[0], Before.Outputs[0]);
}
