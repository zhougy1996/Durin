#include "FunctionPortTestFixture.h"
#include "MaterialFunctionTestSupport.h"

TEST(FMaterialFunctionInteractionTests, PreviewWrappersCompileEveryOutputTypeWithoutChangingTheFunction)
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
		InputExpression->Id = {1, 1, 1, 1}; InputExpression->Port.Id = Input.Id;
		auto* OutputExpression = NewObject<DMaterialExpressionFunctionOutput>(nullptr, NAME_None);
		OutputExpression->Id = {1, 1, 1, 2}; OutputExpression->Port.Id = Output.Id; OutputExpression->Source = {InputExpression->Id};
		ASSERT_TRUE(Function->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(Signature, std::array<DMaterialExpression*, 2>{InputExpression, OutputExpression})));
		const auto SourceExpressions = Function->GetExpressionCollection().Expressions;
		const auto Revision = Function->GetFunctionRevision();
		const auto Built = BuildMaterialFunctionPreview(*Function, Output.Id, *Preview);
		ASSERT_TRUE(Built) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Built);
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
		const auto MissingId = FGuid::NewGuid();
		const auto Missing = BuildMaterialFunctionPreview(*Function, MissingId, *Preview);
		ASSERT_FALSE(Missing);
		ASSERT_TRUE(Missing.DocumentCause);
		EXPECT_EQ(Missing.DocumentCause->Code, EMaterialGraphDocumentError::PreviewOutput);
		EXPECT_EQ(Missing.DocumentCause->PortId, MissingId);
		EXPECT_TRUE(Missing.DocumentCause->bOutput);
		EXPECT_EQ(Missing.DocumentCause->FunctionPath, Function->GetObjectPath());
		EXPECT_EQ(Preview->GetExpressionCollection().Expressions, Before);
		EXPECT_EQ(Preview->GetExpressionOutputs(), BeforeOutputs);
		EXPECT_EQ(Preview->GetMaterialCompileStatus().AuthoredRevision, BeforeRevision);
		ASSERT_TRUE(BuildMaterialFunctionPreview(*Function, Output.Id, *Preview));
		EXPECT_EQ(Missing.DocumentCause->PortId, MissingId);
	}
	MarkAsGarbage(Preview); MarkAsGarbage(Function); CollectGarbage();
}

TEST(FMaterialFunctionInteractionTests, PreviewInvalidationTracksTransitiveEditsAndExplicitRequests)
{
	using namespace Durin;
	using namespace Durin::Editor::Material;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterialFunction> Leaf(NewObject<DMaterialFunction>(nullptr, "PreviewLeaf"));
	TStrongObjectPtr<DMaterialFunction> Wrapper(NewObject<DMaterialFunction>(nullptr, "PreviewParent"));
	TStrongObjectPtr<DMaterialFunction> Root(NewObject<DMaterialFunction>(nullptr, "PreviewRoot"));
	TStrongObjectPtr<DMaterialFunction> Other(NewObject<DMaterialFunction>(nullptr, "PreviewOther"));
	ASSERT_TRUE(FMaterialGraphDocument(*Wrapper).InsertFunctionCall(*Leaf, 0, 100));
	ASSERT_TRUE(FMaterialGraphDocument(*Root).InsertFunctionCall(*Wrapper, 0, 100));
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
	auto Port = Other->GetFunctionSignature().Outputs.front();
	Port.Name = "Unrelated edit";
	ASSERT_TRUE(FMaterialGraphDocument(*Other).SetPort(true, Port));
	EXPECT_FALSE(First.ConsumeRefreshRequest());

	Tests::FTestTransactorOwner Transactions;
	Port = Leaf->GetFunctionSignature().Outputs.front();
	Port.Name = "Transitive edit";
	ASSERT_TRUE(FMaterialGraphDocument(*Leaf).SetPort(true, Port, Transactions.Get()));
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
	Port.Name = "Former dependency";
	ASSERT_TRUE(FMaterialGraphDocument(*Leaf).SetPort(true, Port));
	EXPECT_FALSE(First.ConsumeRefreshRequest());
	EXPECT_TRUE(Second.ConsumeRefreshRequest());
	EXPECT_TRUE(Transactions->Reset());
}
