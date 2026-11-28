#include "MaterialGraph.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "FunctionPortTestFixture.h"
#include "MaterialFunctionTestSupport.h"

TEST(FMaterialFunctionInteractionTests, PreviewWrappersCompileEveryOutputTypeWithoutChangingTheFunction)
{
	using namespace Durin;
	using namespace Durin::MaterialEditor;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	auto* Function = NewObject<DMaterialFunction>(nullptr, "PreviewFunction");
	auto* Preview = NewObject<DMaterial>(nullptr, "PreviewWrapper");
	Preview->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	for (uint32 Index = 0; Index <= static_cast<uint32>(EMaterialProgramValueType::MaterialAttributes); ++Index)
	{
		const auto Type = static_cast<EMaterialProgramValueType>(Index);
		auto Input = FunctionPort(1, Type, "Input");
		Input.bRequired = true;
		const auto Output = FunctionPort(2, Type, "Output");
		const FMaterialFunctionSignature Signature{{Input}, {Output}};
		auto* InputExpression = NewObject<DMaterialExpressionFunctionInput>(nullptr, NAME_None);
		InputExpression->Id = {1, 1, 1, 1}; InputExpression->Port.Id = Input.Id;
		auto* OutputExpression = NewObject<DMaterialExpressionFunctionOutput>(nullptr, NAME_None);
		OutputExpression->Id = {1, 1, 1, 2}; OutputExpression->Port.Id = Output.Id; OutputExpression->Source = {InputExpression};
		ASSERT_TRUE(Function->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(Signature, std::array<DMaterialExpression*, 2>{InputExpression, OutputExpression})));
		const auto SourceExpressions = Function->GetExpressionCollection().Expressions;
		const auto Revision = Function->GetFunctionRevision();
		const auto Built = BuildMaterialFunctionPreview(*Function, Output.Id, *Preview);
		ASSERT_TRUE(Built) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Built);
		const auto Roots = Preview->CaptureRootInputs();
		if (Type == EMaterialProgramValueType::MaterialAttributes)
		{
			EXPECT_TRUE(Roots.bUseMaterialAttributes);
			EXPECT_EQ(Roots.Attributes.GetOutputId(), Output.Id);
		}
		else
		{
			EXPECT_TRUE(Roots.Get(EMaterialProperty::MP_EmissiveColor).Connection.IsConnected());
		}
		ASSERT_TRUE(Preview->CompileEdits()) << "type " << Index << ": " << FormatMaterialProgramDiagnostics(Preview->GetMaterialCompileDiagnostics());
		ASSERT_TRUE(Preview->GetAcceptedCompiledProgram());
		EXPECT_EQ(Function->GetFunctionSignature(), Signature);
		EXPECT_EQ(Function->GetExpressionCollection().Expressions, SourceExpressions);
		EXPECT_EQ(Function->GetFunctionRevision(), Revision);
	}
	const auto BeforeOutputs = Preview->CaptureRootInputs();
	const auto Accepted = Preview->GetAcceptedCompiledProgram();
	const auto Missing = BuildMaterialFunctionPreview(*Function, FGuid::NewGuid(), *Preview);
	ASSERT_FALSE(Missing);
	EXPECT_FALSE(Missing.Message.empty());
	EXPECT_EQ(Preview->CaptureRootInputs(), BeforeOutputs);
	EXPECT_EQ(Preview->GetAcceptedCompiledProgram(), Accepted);
	ASSERT_TRUE(BuildMaterialFunctionPreview(*Function, Function->GetFunctionSignature().Outputs.front().Id, *Preview));
	MarkAsGarbage(Preview); MarkAsGarbage(Function); CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialFunctionInteractionTests, PreviewInvalidationTracksTransitiveEditsAndExplicitRequests)
{
	using namespace Durin;
	using namespace Durin::MaterialEditor;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterialFunction> Leaf(NewObject<DMaterialFunction>(nullptr, "PreviewLeaf"));
	TStrongObjectPtr<DMaterialFunction> Wrapper(NewObject<DMaterialFunction>(nullptr, "PreviewParent"));
	TStrongObjectPtr<DMaterialFunction> Root(NewObject<DMaterialFunction>(nullptr, "PreviewRoot"));
	TStrongObjectPtr<DMaterialFunction> Other(NewObject<DMaterialFunction>(nullptr, "PreviewOther"));
	ASSERT_TRUE(DMaterialGraph::Create(*Wrapper)->InsertFunctionCall(*Leaf, 0, 100, false));
	ASSERT_TRUE(DMaterialGraph::Create(*Root)->InsertFunctionCall(*Wrapper, 0, 100, false));
	FMaterialFunctionPreviewInvalidation First, Second;
	First.SetFunction(Root.Get());
	Second.SetFunction(Root.Get());
	EXPECT_TRUE(First.ConsumeRefreshRequest());
	EXPECT_TRUE(Second.ConsumeRefreshRequest());
	EXPECT_FALSE(First.ConsumeRefreshRequest());
	auto Presentation = Leaf->GetFunctionPresentation();
	Presentation.Nodes.push_back({Leaf->GetExpressionCollection().Expressions.front()->Id, 55, 44});
	ASSERT_TRUE(Leaf->SetFunctionPresentation(Presentation));
	EXPECT_FALSE(First.ConsumeRefreshRequest());
	TStrongObjectPtr<DMaterialGraph> OtherGraph(DMaterialGraph::Create(*Other));
	auto* OtherOutput = Testing::FindFunctionTerminal<DMaterialExpressionFunctionOutput>(*OtherGraph, Other->GetFunctionSignature().Outputs.front().Id);
	ASSERT_TRUE(OtherOutput);
	ASSERT_TRUE(OtherOutput->OnRenameNode("Unrelated edit", false));
	EXPECT_FALSE(First.ConsumeRefreshRequest());

	Tests::FTestEditorTransactorOwner Transactions;
	TStrongObjectPtr<DMaterialGraph> LeafGraph(DMaterialGraph::Create(*Leaf));
	auto* LeafOutput = Testing::FindFunctionTerminal<DMaterialExpressionFunctionOutput>(*LeafGraph, Leaf->GetFunctionSignature().Outputs.front().Id);
	ASSERT_TRUE(LeafOutput);
	ASSERT_TRUE(LeafOutput->OnRenameNode("Transitive edit"));
	EXPECT_TRUE(First.ConsumeRefreshRequest());
	EXPECT_TRUE(Second.ConsumeRefreshRequest());
	EXPECT_FALSE(First.ConsumeRefreshRequest());
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_TRUE(First.ConsumeRefreshRequest());
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_TRUE(First.ConsumeRefreshRequest());
	First.RequestRefresh();
	First.RequestRefresh();
	EXPECT_TRUE(First.ConsumeRefreshRequest());
	EXPECT_FALSE(First.ConsumeRefreshRequest());
	First.SetFunction(Other.Get());
	EXPECT_TRUE(First.ConsumeRefreshRequest());
	First.SetFunction(Other.Get());
	EXPECT_FALSE(First.ConsumeRefreshRequest());
	ASSERT_TRUE(LeafOutput->OnRenameNode("Former dependency", false));
	EXPECT_FALSE(First.ConsumeRefreshRequest());
	EXPECT_TRUE(Second.ConsumeRefreshRequest());
	Transactions->Reset();
}
