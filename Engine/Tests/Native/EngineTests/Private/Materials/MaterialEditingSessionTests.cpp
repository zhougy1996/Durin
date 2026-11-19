#include "Graph/MaterialGraphCatalog.h"
#include "Graph/MaterialGraphEditSession.h"
#include "MaterialGraphDocument.h"
#include "MaterialGraph.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "FunctionPortTestFixture.h"
#include "TypedMaterialGraphTestFixture.h"
#include "Widgets/MaterialEditingSession.h"
#include "MaterialGraphOperations.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "Asset/AssetCompilingManager.h"
#include "DObject/Package.h"
#include "DObject/ObjectLifecycle.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialFunction.h"
#include "Modules/ModuleManager.h"
#include "NativeDObjectTestSupport.h"
#include "EngineTestSupport.h"
#include "Misc/MountPathTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::MaterialEditor;

	class FMaterialEditingSessionTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			FModuleManager::Get().LoadModule("RenderCore");
			Testing::RegisterMountPointForTests("/SessionTests/",
				Testing::GetTestWorkDirectory().generic_string() + "/");
			FPackagePath Path;
			ASSERT_TRUE(FPackagePath::TryCreate(std::format("/SessionTests/{}",
				FGuid::NewGuid().ToString()), Path));
			Package = CreatePackage(Path);
			ASSERT_TRUE(Package);
			Source = NewObject<DMaterial>(Package, "Material");
			ASSERT_TRUE(Source);
			Package->ClearDirty();
		}
		auto TearDown() -> void override
		{
			MarkAsGarbage(Source);
			MarkAsGarbage(Package);
			CollectGarbage(Durin::EObjectFlags::Standalone);
		}
		auto Edit(DMaterial& Material, float Roughness) -> void
		{
			auto Outputs = Material.GetExpressionOutputs();
			Outputs.Get(EMaterialProperty::MP_Roughness).SetConstant({Roughness});
			std::vector<DMaterialExpression*> Expressions;
			for (const auto& Expression : Material.GetExpressionCollection().Expressions) Expressions.push_back(Expression.Get());
			ASSERT_TRUE(Material.SetMaterialExpressions(Expressions, Outputs));
		}
		DPackage* Package = nullptr;
		DMaterial* Source = nullptr;
		Testing::FScopedMountRegistryFixture MountRegistry;
	};
}

TEST_F(FMaterialEditingSessionTests, EditorGraphsRebuildFromWorkingAssetsWithoutEditingThem)
{
	Tests::FTestEditorTransactorOwner Transactions;
	TStrongObjectPtr<DMaterialFunction> Callee(NewObject<DMaterialFunction>(nullptr, "GraphCallee"));
	FMaterialGraphDocument FunctionDocument(*Callee);
	const FMaterialFunctionPort Port{.Id = FGuid::NewGuid(), .Type = EMaterialProgramValueType::Float, .Name = "Result"};
	ASSERT_TRUE(FunctionDocument.AddPort(true, Port, {}, 10, 20, ETransactionMode::NotTransactable));
	FMaterialGraphDocument MaterialDocument(*Source);
	const auto Call = MaterialDocument.InsertFunctionCall(*Callee, 100, 200, ETransactionMode::NotTransactable);
	ASSERT_TRUE(Call);
	const auto ResultLink = FMaterialProgramLink{Call.GeneratedNodeIds.front(), 0, Port.Id};
	ASSERT_TRUE(GraphEditInternals::WriteExpressionConnection(MaterialDocument.GetOwner(), FMaterialGraphPinAddress::MaterialOutput(Source->GetOutputNode()->Id,
		EMaterialProperty::MP_Roughness), FMaterialGraphPinAddress::Output(ResultLink), true, ETransactionMode::NotTransactable));
	DMaterialExpressionFunctionCall* Expression = nullptr;
	for (const auto& Entry : Source->GetExpressionCollection().Expressions)
		if (Entry->Id == Call.GeneratedNodeIds.front()) Expression = Cast<DMaterialExpressionFunctionCall>(Entry.Get());
	ASSERT_TRUE(Expression);
	const auto Missing = FGuid::NewGuid();
	Expression->Outputs.push_back({Missing, EMaterialProgramValueType::Float});
	Package->ClearDirty();
	TWeakObjectPtr<DMaterialGraph> RetiredGraph;
	TWeakObjectPtr<DMaterialGraphNode> RetiredNode;
	{
		FMaterialEditingSession Session;
		std::string Error;
		ASSERT_TRUE(Session.Initialize(*Source, EMaterialEditCompileMode::Manual, Error, Transactions.Get())) << Error;
		TStrongObjectPtr<DMaterialGraph> FunctionGraph(DMaterialGraph::Create(*Callee));
		for (auto* Graph : {Session.Graph.Get(), FunctionGraph.Get()})
		{
			ASSERT_TRUE(Graph);
			auto* Owner = Graph->GetMaterialOrFunction();
			const auto Expected = ProjectMaterialGraph(&*Owner);
			const auto History = Transactions.Get()->GetUndoCount();
			for (int Pass = 0; Pass < 2; ++Pass)
			{
				Graph->RebuildGraph();
				Graph->LinkGraphNodesFromMaterial();
				ASSERT_EQ(Graph->GetNodes().size(), Expected.Nodes.size());
				for (const auto& View : Expected.Nodes)
				{
					auto* Node = Graph->FindNode(View.Node.Id);
					ASSERT_TRUE(Node);
					ASSERT_TRUE(Node->GetMaterialExpression());
					EXPECT_EQ(Graph->AddExpression(*Node->GetMaterialExpression()), Node);
					Node->ReconstructNode();
					EXPECT_EQ(Node->GetView().Presentation, View.Presentation);
					EXPECT_EQ(Cast<DMaterialGraphNode_Root>(Node) != nullptr, View.Node.bMaterialOutput);
					ASSERT_EQ(Node->GetView().Inputs.size(), View.Inputs.size());
					for (size_t I = 0; I < View.Inputs.size(); ++I)
					{
						const auto& Pin = Node->GetView().Inputs[I];
						EXPECT_EQ(Node->GetView().InputAddress(Pin), View.InputAddress(View.Inputs[I]));
						EXPECT_EQ(Pin.Link, View.Inputs[I].Link);
						EXPECT_EQ(Pin.AcceptedTypes, View.Inputs[I].AcceptedTypes);
					}
					ASSERT_EQ(Node->GetView().Outputs.size(), View.Outputs.size());
					for (size_t I = 0; I < View.Outputs.size(); ++I)
					{
						const auto& Pin = Node->GetView().Outputs[I];
						EXPECT_EQ(Pin.PortId, View.Outputs[I].PortId);
						EXPECT_EQ(Pin.Type, View.Outputs[I].Type);
						EXPECT_EQ(Pin.bMissing, View.Outputs[I].bMissing);
					}
				}
			}
			EXPECT_EQ(Transactions.Get()->GetUndoCount(), History);
		}
		auto* Node = Session.Graph->FindNode(Expression->Id);
		ASSERT_TRUE(Node);
		ASSERT_TRUE(Session.Graph->FindOutputPin(ResultLink));
		EXPECT_EQ(Session.Graph->FindOutputPin(ResultLink)->PortId, Port.Id);
		EXPECT_TRUE(std::ranges::any_of(Node->GetView().Outputs, [&](const auto& Pin) { return Pin.PortId == Missing && Pin.bMissing; }));
		EXPECT_FALSE(Session.GetWorkingMaterial()->GetPackage()->IsDirty());
		RetiredGraph = Session.Graph.Get();
		RetiredNode = Node;
	}
	CollectGarbage(EObjectFlags::Standalone);
	EXPECT_FALSE(RetiredGraph.IsValid());
	EXPECT_FALSE(RetiredNode.IsValid());
}

TEST_F(FMaterialEditingSessionTests, ReconstructNodeReadsRenamedFunctionPinsAndRetainsMaterialRootBinding)
{
	Tests::FTestEditorTransactorOwner Transactions;
	const auto Mode = ETransactionMode::NotTransactable;
	TStrongObjectPtr<DMaterialFunction> Callee(NewObject<DMaterialFunction>(nullptr, "PinReconstruction"));
	FMaterialGraphDocument Function(*Callee);
	FMaterialFunctionPort Port{.Id = FGuid::NewGuid(), .Type = EMaterialProgramValueType::Float, .Name = "Value"};
	FMaterialFunctionPort InputPort{.Id = FGuid::NewGuid(), .Type = EMaterialProgramValueType::Float, .Name = "Amount"};
	InputPort.bRequired = true;
	const auto SourceTerminal = Function.AddPort(false, InputPort, {}, 0, 0, Mode);
	ASSERT_TRUE(SourceTerminal);
	const FMaterialProgramLink InternalLink{SourceTerminal.GeneratedNodeIds.at(0)};
	const auto Terminal = Function.AddPort(true, Port, InternalLink, 0, 0, Mode);
	ASSERT_TRUE(Terminal);
	ASSERT_EQ(Terminal.GeneratedNodeIds.size(), 1u);

	Edit(*Source, 0.375f);
	FMaterialGraphDocument Caller(*Source);
	const auto Constant = Testing::CreateGraphConstant(Caller, 0.5f);
	ASSERT_TRUE(Constant);
	const FMaterialProgramLink CallerSource{Constant.GeneratedNodeIds.at(0)};
	const std::array Bindings{FMaterialFunctionInputBinding{InputPort.Id, InputPort.Type, CallerSource}};
	const auto Call = Caller.InsertFunctionCall(*Callee, 0, 0, Bindings, Mode);
	ASSERT_TRUE(Call);
	ASSERT_EQ(Call.GeneratedNodeIds.size(), 1u);
	TStrongObjectPtr<DMaterialGraph> MaterialGraph(DMaterialGraph::Create(*Source));
	TStrongObjectPtr<DMaterialGraph> FunctionGraph(DMaterialGraph::Create(*Callee));
	const FMaterialProgramLink Link{Call.GeneratedNodeIds.front(), 0, Port.Id};
	const auto RootAddress = FMaterialGraphPinAddress::MaterialOutput(Source->GetOutputNode()->Id, EMaterialProperty::MP_Roughness);
	ASSERT_TRUE(MaterialGraph->GetSchema().TryCreateConnection(*MaterialGraph, FMaterialGraphPinAddress::Output(Link), RootAddress, true, Mode));

	auto* CallNode = MaterialGraph->FindNode(Link.SourceNodeId);
	auto* OutputNode = FunctionGraph->FindNode(Terminal.GeneratedNodeIds.front());
	auto* Root = MaterialGraph->FindNode(RootAddress.NodeId);
	ASSERT_TRUE(CallNode && OutputNode && Root);
	Port.Name = "Renamed";
	const auto InputAddress = FMaterialGraphPinAddress::Input(Link.SourceNodeId, 0, InputPort.Id);
	auto* InputNode = FunctionGraph->FindNode(InternalLink.SourceNodeId);
	ASSERT_TRUE(InputNode);
	const auto CheckBindings = [&](std::string_view InputName, std::string_view OutputName) {
		const auto Signature = Callee->GetFunctionSignature();
		const auto In = std::ranges::find(Signature.Inputs, InputPort.Id, &FMaterialFunctionPort::Id);
		const auto Out = std::ranges::find(Signature.Outputs, Port.Id, &FMaterialFunctionPort::Id);
		ASSERT_NE(In, Signature.Inputs.end());
		ASSERT_NE(Out, Signature.Outputs.end());
		EXPECT_EQ(InputNode->GetMaterialExpression()->GetEditableName(), InputName);
		EXPECT_EQ(OutputNode->GetMaterialExpression()->GetEditableName(), OutputName);
		EXPECT_EQ(In->Name, InputName);
		EXPECT_EQ(Out->Name, OutputName);
		const auto* CallInput = MaterialGraph->FindInputPin(InputAddress);
		const auto* CallOutput = MaterialGraph->FindOutputPin(Link);
		const auto* RootInput = MaterialGraph->FindInputPin(RootAddress);
		ASSERT_TRUE(CallInput && CallOutput && RootInput);
		EXPECT_EQ(CallInput->Name, InputName);
		EXPECT_EQ(CallInput->Link, CallerSource);
		EXPECT_EQ(CallOutput->Name, OutputName);
		EXPECT_EQ(RootInput->Link, Link);
		ASSERT_EQ(OutputNode->GetView().Inputs.size(), 1u);
		EXPECT_EQ(OutputNode->GetView().Inputs.front().Link, InternalLink);
	};
	ASSERT_TRUE(InputNode->GetMaterialExpression()->CanRenameNode());
	ASSERT_TRUE(OutputNode->GetMaterialExpression()->CanRenameNode());
	EXPECT_FALSE(CallNode->GetMaterialExpression()->CanRenameNode());
	const auto BeforeRename = Transactions->GetQueueLength();
	EXPECT_FALSE(CallNode->OnRenameNode("Unsupported"));
	EXPECT_FALSE(OutputNode->OnRenameNode(""));
	EXPECT_EQ(InputNode->OnRenameNode("Amount").Status, EMaterialGraphCommandStatus::NoChange);
	EXPECT_EQ(Transactions->GetQueueLength(), BeforeRename);
	ASSERT_TRUE(InputNode->OnRenameNode("Factor"));
	ASSERT_TRUE(OutputNode->OnRenameNode(Port.Name));
	EXPECT_EQ(Transactions->GetQueueLength(), BeforeRename + 2);
	CheckBindings("Factor", "Renamed");
	ASSERT_TRUE(Transactions->Undo());
	CheckBindings("Factor", "Value");
	ASSERT_TRUE(Transactions->Undo());
	CheckBindings("Amount", "Value");
	ASSERT_TRUE(Transactions->Redo());
	CheckBindings("Factor", "Value");
	ASSERT_TRUE(Transactions->Redo());
	CheckBindings("Factor", "Renamed");
	Package->ClearDirty();
	const auto History = Transactions.Get()->GetQueueLength();
	for (int Pass = 0; Pass < 2; ++Pass)
	{
		OutputNode->ReconstructNode();
		CallNode->ReconstructNode();
		Root->AllocateDefaultPins();
		ASSERT_EQ(OutputNode->GetView().Inputs.size(), 1u);
		EXPECT_EQ(OutputNode->GetView().Inputs.front().Name, Port.Name);
		ASSERT_EQ(CallNode->GetView().Outputs.size(), Callee->GetFunctionSignature().Outputs.size());
		const auto* Output = MaterialGraph->FindOutputPin(Link);
		ASSERT_TRUE(Output);
		EXPECT_EQ(Output->Name, Port.Name);
		const auto* Input = MaterialGraph->FindInputPin(RootAddress);
		ASSERT_TRUE(Input);
		EXPECT_EQ(Input->Link, Link);
		EXPECT_FLOAT_EQ(Input->RetainedConstant.Literal.X, 0.375f);
		EXPECT_TRUE(Root->GetView().Outputs.empty());
	}
	Port.Type = EMaterialProgramValueType::Float3;
	ASSERT_TRUE(OutputNode->SetFunctionPort(Port, Mode));
	ASSERT_TRUE(MaterialGraph->FindOutputPin(Link));
	EXPECT_EQ(MaterialGraph->FindOutputPin(Link)->Type, Port.Type);
	ASSERT_TRUE(MaterialGraph->FindInputPin(RootAddress));
	EXPECT_EQ(MaterialGraph->FindInputPin(RootAddress)->SourceType, Port.Type);
	EXPECT_FALSE(Package->IsDirty());
	EXPECT_EQ(Transactions.Get()->GetQueueLength(), History);
}

TEST_F(FMaterialEditingSessionTests, TypedExpressionCopiesStayIndependentAcrossApplyDiscardAndCollection)
{
	TStrongObjectPtr<DMaterial> SourceOwner(Source);
	TStrongObjectPtr<DMaterialExpressionScalarParameter> Parameter(
		NewObject<DMaterialExpressionScalarParameter>(nullptr, "SessionParameter"));
	Parameter->Id = FGuid::NewGuid();
	Parameter->Metadata.Id = FGuid::NewGuid();
	Parameter->Metadata.Name = FName("SessionValue");
	Parameter->DefaultValue = 0.23f;
	DMaterialExpression* Inputs[] = {Parameter.Get()};
	auto Outputs = Source->GetExpressionOutputs();
	Outputs.Get(EMaterialProperty::MP_Roughness).Connection.Expression = Parameter.Get();
	ASSERT_TRUE(Source->SetMaterialExpressions(Inputs, Outputs));
	Package->ClearDirty();
	TObjectPtr<DMaterialExpression> RetiredDraftExpression;
	{
		FMaterialEditingSession Session;
		std::string Error;
		ASSERT_TRUE(Session.Initialize(*Source, EMaterialEditCompileMode::Manual, Error)) << Error;
		auto* Draft = Session.GetWorkingMaterial();
		auto* Original = Cast<DMaterialExpressionScalarParameter>(Source->GetExpressionCollection().Expressions[0].Get());
		auto* Edited = Cast<DMaterialExpressionScalarParameter>(Draft->GetExpressionCollection().Expressions[0].Get());
		ASSERT_NE(Original, nullptr);
		ASSERT_NE(Edited, nullptr);
		EXPECT_NE(Original, Edited);
		EXPECT_EQ(Original->GetOuter(), Source);
		EXPECT_EQ(Edited->GetOuter(), Draft);
		EXPECT_EQ(Original->Id, Edited->Id);
		EXPECT_FALSE(Session.HasUnappliedChanges());
		Edited->DefaultValue = 0.64f;
		Edited->Metadata.DisplayName = "Edited parameter";
		Edited->MarkPackageDirty();
		Edited->PostEditChangeProperty({});
		EXPECT_TRUE(Session.HasUnappliedChanges());
		EXPECT_FLOAT_EQ(Original->DefaultValue, 0.23f);
		CollectGarbage(Durin::EObjectFlags::Standalone);
		ASSERT_TRUE(Session.FinishAndApply(Error)) << Error;
		auto* Applied = Cast<DMaterialExpressionScalarParameter>(Source->GetExpressionCollection().Expressions[0].Get());
		ASSERT_NE(Applied, nullptr);
		EXPECT_NE(Applied, Edited);
		EXPECT_EQ(Applied->GetOuter(), Source);
		EXPECT_FLOAT_EQ(Applied->DefaultValue, 0.64f);
		EXPECT_EQ(Applied->Metadata.DisplayName, "Edited parameter");
		EXPECT_FALSE(Session.HasUnappliedChanges());
		Edited->DefaultValue = 0.91f;
		Edited->MarkPackageDirty();
		Edited->PostEditChangeProperty({});
		EXPECT_TRUE(Session.HasUnappliedChanges());
		Edited->DefaultValue = 0.64f;
		Edited->MarkPackageDirty();
		Edited->PostEditChangeProperty({});
		EXPECT_FALSE(Session.HasUnappliedChanges());
		RetiredDraftExpression = Edited;
	}
	CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_FALSE(RetiredDraftExpression.IsValid());
	auto* Applied = Cast<DMaterialExpressionScalarParameter>(Source->GetExpressionCollection().Expressions[0].Get());
	ASSERT_NE(Applied, nullptr);
	EXPECT_FLOAT_EQ(Applied->DefaultValue, 0.64f);
}

TEST_F(FMaterialEditingSessionTests, PreviewCompilationIsIsolatedAndApplyPreservesSourceIdentity)
{
	FMaterialEditingSession Session;
	std::string Error;
	ASSERT_TRUE(Session.Initialize(*Source, EMaterialEditCompileMode::Manual, Error)) << Error;
	auto* Draft = Session.GetWorkingMaterial();
	ASSERT_NE(Draft, Source);
	EXPECT_NE(Draft->GetPackage(), Package);
	EXPECT_TRUE(Draft->GetPackage()->GetTopLevelAssets().empty());
	EXPECT_FALSE(Session.HasUnappliedChanges());
	const auto Original = Source->GetExpressionOutputs();
	const auto Generation = Source->GetMaterialCompileDebugInfo().RequestId;
	auto* Child = NewObject<DMaterialInstance>(nullptr, "ApplyChild");
	TStrongObjectPtr<DMaterialInstance> ChildRoot(Child);
	ASSERT_TRUE(Child->SetParent(Source));
	const auto ChildRevision = Child->GetMaterialCompileDebugInfo().AuthoredRevision;
	Edit(*Draft, 0.37f);
	Draft->CompileEdits();
	FAssetCompilingManager::Get().FinishCompilationForObject(*Draft);
	EXPECT_EQ(Source->GetExpressionOutputs(), Original);
	EXPECT_EQ(Source->GetMaterialCompileDebugInfo().RequestId, Generation);
	EXPECT_EQ(Child->GetMaterialCompileDebugInfo().AuthoredRevision, ChildRevision);
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(Session.HasUnappliedChanges());
	ASSERT_TRUE(Session.FinishAndApply(Error)) << Error;
	EXPECT_EQ(Child->GetParent(), Source);
	EXPECT_EQ(CaptureMaterialOutputBindings(Source->GetExpressionOutputs()), CaptureMaterialOutputBindings(Draft->GetExpressionOutputs()));
	EXPECT_GT(Child->GetMaterialCompileDebugInfo().AuthoredRevision, ChildRevision);
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_FALSE(Session.HasUnappliedChanges());
	const auto AppliedGeneration = Source->GetMaterialCompileDebugInfo().RequestId;
	ASSERT_TRUE(Session.FinishAndApply(Error)) << Error;
	EXPECT_EQ(Source->GetMaterialCompileDebugInfo().RequestId, AppliedGeneration);
	Edit(*Draft, 0.62f);
	EXPECT_TRUE(Session.HasUnappliedChanges());
	EXPECT_FLOAT_EQ(Source->GetExpressionOutputs().Get(EMaterialProperty::MP_Roughness).Constant[0], 0.37f);
	MarkAsGarbage(Child);
}

TEST_F(FMaterialEditingSessionTests, FunctionCallDraftIsCompleteAndAppliesBindingsAtomically)
{
	auto* First = NewObject<DMaterialFunction>(Package, "FirstFunction");
	auto* Second = NewObject<DMaterialFunction>(Package, "SecondFunction");
	auto Signature = Second->GetFunctionSignature();
	Signature.Inputs[0].Default.Surface.Get(EMaterialProperty::MP_Roughness).Literal.X = 0.23f;
	std::vector<DMaterialExpression*> Body;
	for (const auto& Expression : Second->GetExpressionCollection().Expressions) Body.push_back(Expression.Get());
	ASSERT_TRUE(Second->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(Signature, Body)));
	Source->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto Output = First->GetFunctionSignature().Outputs[0];
	const FGuid CallId{72, 1, 1, 1};
	TStrongObjectPtr<DMaterialExpressionFunctionCall> Call(NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None));
	Call->Id = CallId; Call->Function = First; Call->Outputs = {{Output.Id, Output.Type}};
	const std::array<DMaterialExpression*, 1> Expressions{Call.Get()};
	FMaterialAttributeRoot Outputs;
	Outputs.Attributes = {Call.Get(), 0, Output.Id}; Outputs.bUseMaterialAttributes = true;
	ASSERT_TRUE(Source->SetMaterialExpressions(Expressions, Outputs));
	const auto GetCall = [](DMaterial& Material) {
		return Cast<DMaterialExpressionFunctionCall>(Material.GetExpressionCollection().Expressions.at(0).Get());
	};
	Tests::FTestEditorTransactorOwner Transactions;
	FMaterialEditingSession Session;
	std::string Error;
	ASSERT_TRUE(Session.Initialize(*Source, EMaterialEditCompileMode::Manual, Error, Transactions.Get())) << Error;
	auto* Draft = Session.GetWorkingMaterial();
	ASSERT_EQ(Draft->GetExpressionCollection().Expressions.size(), 2u);
	EXPECT_EQ(GetCall(*Draft)->Function.Get(), First);
	EXPECT_FALSE(Session.HasUnappliedChanges());
	{
		GraphEditInternals::FGraphEditSession Renamed(*Draft);
		const auto Position = std::ranges::find(Renamed.Presentation.Nodes, CallId, &FMaterialGraphNodePresentation::NodeId);
		if (Position == Renamed.Presentation.Nodes.end()) Renamed.Presentation.Nodes.push_back({.NodeId = CallId, .DisplayName = "Reusable Surface"});
		else Position->DisplayName = "Reusable Surface";
		ASSERT_TRUE(Renamed.Commit("Rename Call", Transactions.Get()));
	}
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(CaptureMaterialOutputBindings(Draft->GetExpressionOutputs()), CaptureMaterialOutputBindings(Outputs));
	ASSERT_TRUE(Transactions.Get()->Redo());
	const auto Label = std::ranges::find(Draft->GetMaterialGraphPresentation().Nodes, CallId, &FMaterialGraphNodePresentation::NodeId);
	ASSERT_NE(Label, Draft->GetMaterialGraphPresentation().Nodes.end());
	EXPECT_EQ(Label->DisplayName, "Reusable Surface");
	EXPECT_EQ(GetCall(*Draft)->Function.Get(), First);
	Call->Function = Second;
	ASSERT_TRUE(Draft->SetMaterialExpressions(Expressions, Outputs));
	EXPECT_TRUE(Session.HasUnappliedChanges());
	EXPECT_EQ(GetCall(*Source)->Function.Get(), First);
	ASSERT_TRUE(Session.FinishAndApply(Error)) << Error;
	EXPECT_EQ(GetCall(*Source)->Function.Get(), Second);
	EXPECT_FALSE(Session.HasUnappliedChanges());
	const auto Revision = Source->GetMaterialCompileDebugInfo().AuthoredRevision;
	Outputs.Attributes.Expression = GetCall(*Draft);
	EXPECT_FALSE(Source->SetMaterialExpressions(Expressions, Outputs));
	EXPECT_EQ(Source->GetMaterialCompileDebugInfo().AuthoredRevision, Revision);
	EXPECT_EQ(GetCall(*Source)->Id, CallId);
}

TEST_F(FMaterialEditingSessionTests, DefaultsStaticPropertiesAndPresentationStayInTheDraft)
{
	FMaterialEditingSession Session;
	std::string Error;
	ASSERT_TRUE(Session.Initialize(*Source, EMaterialEditCompileMode::Manual, Error)) << Error;
	auto* Draft = Session.GetWorkingMaterial();
	const auto OriginalProperties = Source->GetStaticProperties();
	const auto OriginalPresentation = Source->GetMaterialGraphPresentation();
	FMaterialParameterDefinition Definition;
	Definition.Id = FGuid::NewGuid();
	Definition.Name = FName("DraftValue");
	Definition.Type = EMaterialParameterType::Scalar;
	Definition.Value.GetScalar() = 0.23f;
	TStrongObjectPtr<DMaterialExpressionScalarParameter> Parameter(NewObject<DMaterialExpressionScalarParameter>(nullptr, NAME_None));
	Parameter->Id = FGuid::NewGuid(); Parameter->Metadata = {.Id = Definition.Id, .Name = Definition.Name};
	Parameter->DefaultValue = Definition.Value.GetScalar();
	const std::array<DMaterialExpression*, 1> Expressions{Parameter.Get()};
	ASSERT_TRUE(Draft->SetMaterialExpressions(Expressions, Draft->GetExpressionOutputs()));
	auto Properties = OriginalProperties;
	Properties.bTwoSided = !Properties.bTwoSided;
	ASSERT_TRUE(Draft->SetStaticProperties(Properties));
	auto Presentation = OriginalPresentation;
	Testing::OutputPosition(*Draft, Presentation).X += 100;
	ASSERT_TRUE(Draft->SetMaterialGraphPresentation(Presentation) != Durin::EMaterialGraphPresentationResult::Rejected);
	EXPECT_EQ(Source->GetStaticProperties(), OriginalProperties);
	EXPECT_EQ(Source->GetMaterialGraphPresentation(), OriginalPresentation);
	EXPECT_EQ(Source->FindParameterDefinition(Definition.Id), nullptr);
	ASSERT_TRUE(Session.FinishAndApply(Error)) << Error;
	EXPECT_EQ(Source->GetStaticProperties(), Properties);
	EXPECT_EQ(Source->GetMaterialGraphPresentation(), Presentation);
	ASSERT_NE(Source->FindParameterDefinition(Definition.Id), nullptr);
	EXPECT_FLOAT_EQ(Source->FindParameterDefinition(Definition.Id)->Value.GetScalar(), 0.23f);
}

TEST_F(FMaterialEditingSessionTests, ExternalSourceChangeRejectsApplyWithoutOverwritingEitherSide)
{
	FMaterialEditingSession Session;
	std::string Error;
	ASSERT_TRUE(Session.Initialize(*Source, EMaterialEditCompileMode::Manual, Error)) << Error;
	Edit(*Session.GetWorkingMaterial(), 0.31f);
	Edit(*Source, 0.72f);
	EXPECT_FALSE(Session.FinishAndApply(Error));
	EXPECT_NE(Error.find("outside"), std::string::npos);
	EXPECT_FLOAT_EQ(Source->GetExpressionOutputs().Get(EMaterialProperty::MP_Roughness).Constant[0], 0.72f);
	EXPECT_FLOAT_EQ(Session.GetWorkingMaterial()->GetExpressionOutputs().Get(EMaterialProperty::MP_Roughness).Constant[0], 0.31f);
	EXPECT_TRUE(Session.HasUnappliedChanges());
}

TEST_F(FMaterialEditingSessionTests, DiscardRetiresDraftAndItsHistoryWithoutChangingSource)
{
	Tests::FTestEditorTransactorOwner Transactions;
	TObjectPtr<DMaterial> RetiredDraft;
	const auto Original = Source->GetExpressionOutputs();
	{
		FMaterialEditingSession Session;
		std::string Error;
		ASSERT_TRUE(Session.Initialize(*Source, EMaterialEditCompileMode::Manual, Error,
			Transactions.Get())) << Error;
		auto* Draft = Session.GetWorkingMaterial();
		RetiredDraft = Draft;
		const auto Pin = FMaterialGraphPinAddress::MaterialOutput(Draft->GetOutputNode()->Id, EMaterialProperty::MP_Roughness);
		ASSERT_TRUE(Session.Graph->FindNode(Pin.NodeId)->PinDefaultValueChanged(Pin,
			{.Kind = EMaterialInputDefaultKind::Literal, .Type = EMaterialProgramValueType::Float, .Literal = {.X = 0.21f}}));
		EXPECT_TRUE(Session.HasUnappliedChanges());
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_FALSE(Session.HasUnappliedChanges());
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_TRUE(Session.HasUnappliedChanges());
		ASSERT_TRUE(Session.FinishAndApply(Error)) << Error;
		Session.MarkSaved();
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_TRUE(Session.HasUnappliedChanges());
		EXPECT_NE(Source->GetExpressionOutputs(), Original);
	}
	EXPECT_FALSE(RetiredDraft.IsValid());
	EXPECT_FALSE(Transactions->CanUndo());
	EXPECT_FALSE(Transactions->CanRedo());
	EXPECT_FLOAT_EQ(Source->GetExpressionOutputs().Get(EMaterialProperty::MP_Roughness).Constant[0], 0.21f);
}

TEST_F(FMaterialEditingSessionTests, DiscardWithoutApplyLeavesSourceClean)
{
	const auto Original = Source->GetExpressionOutputs();
	{
		FMaterialEditingSession Session;
		std::string Error;
		ASSERT_TRUE(Session.Initialize(*Source, EMaterialEditCompileMode::Automatic, Error)) << Error;
		Edit(*Session.GetWorkingMaterial(), 0.19f);
	}
	EXPECT_FALSE(Package->IsDirty());
	EXPECT_EQ(Source->GetExpressionOutputs(), Original);
}

TEST_F(FMaterialEditingSessionTests, RelocationDoesNotInvalidateAnUnchangedSource)
{
	FMaterialEditingSession Session;
	std::string Error;
	ASSERT_TRUE(Session.Initialize(*Source, EMaterialEditCompileMode::Manual, Error)) << Error;
	Edit(*Session.GetWorkingMaterial(), 0.46f);
	FPackagePath Destination;
	ASSERT_TRUE(FPackagePath::TryCreate(std::format("/SessionTests/Moved{}",
		FGuid::NewGuid().ToString()), Destination));
	ASSERT_TRUE(Package->RelocateAssetPackage(Destination));
	ASSERT_TRUE(Session.FinishAndApply(Error)) << Error;
	EXPECT_FLOAT_EQ(Source->GetExpressionOutputs().Get(EMaterialProperty::MP_Roughness).Constant[0], 0.46f);
}
