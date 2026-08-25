#include "MaterialGraphAuthoring.h"

#include "MaterialTestSupport.h"

#include "DObject/DefaultObjectGraph.h"
#include "DObject/ObjectLifecycle.h"
#include "Editor/Transaction.h"
#include "Materials/Material.h"
#include "Materials/MaterialProgramCompiler.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::Editor;
	using namespace Durin::Editor::Material;

	auto FindViewNode(const FMaterialGraphView& View, const FGuid& Id)
		-> const FMaterialGraphNodeView*
	{
		const auto It = std::ranges::find(View.Nodes, Id,
			[](const FMaterialGraphNodeView& Node) { return Node.Node.Id; });
		return It == View.Nodes.end() ? nullptr : &*It;
	}

	auto Normalize(const DMaterial& Material) -> FMaterialNormalizationResult
	{
		FMaterialCompilerInput Input;
		Input.Program = *Material.GetMaterialProgram();
		for (const FMaterialParameterDefinition& Definition
			: Material.GetParameterDefinitions())
			Input.Parameters.push_back({Definition.Id, Definition.Type});
		std::ranges::sort(Input.Parameters, {},
			&FMaterialCompilerParameterDeclaration::Id);
		Input.Environment.CompilerIdentity = "material-graph-authoring-test";
		Input.Environment.Target = "vulkan-spirv-1.5";
		return NormalizeMaterialProgram(Input);
	}
}

TEST(FMaterialGraphAuthoringTests, PresentationSanitizationIsIndependentAndBounded)
{
	const FMaterialProgram Program = MakeCanonicalMaterialProgram();
	ASSERT_GE(Program.Nodes.size(), 2u);
	FMaterialGraphPresentation Presentation;
	Presentation.SchemaVersion = 99;
	Presentation.Nodes = {
		{Program.Nodes[1].Id, 20, 40},
		{Program.Nodes[1].Id, 60, 80},
		{FGuid::NewGuid(), 10, 10},
		{Program.Nodes[0].Id, MaterialGraphPresentationCoordinateLimit + 1, 0},
	};

	const FMaterialGraphPresentation Sanitized =
		SanitizeMaterialGraphPresentation(Presentation, Program);
	EXPECT_EQ(Sanitized.SchemaVersion,
		CurrentMaterialGraphPresentationSchemaVersion);
	ASSERT_EQ(Sanitized.Nodes.size(), 1u);
	EXPECT_EQ(Sanitized.Nodes.front().NodeId, Program.Nodes[1].Id);
	EXPECT_EQ(Sanitized.Nodes.front().X, 20);
	EXPECT_EQ(Sanitized.Nodes.front().Y, 40);
}

TEST(FMaterialGraphAuthoringTests, PresentationReachesMaximumNodeBoundAndDuplicatesByReflection)
{
	FMaterialProgram MaximumProgram;
	FMaterialGraphPresentation MaximumPresentation;
	for (uint32 Index = 0; Index < MaterialProgramMaxNodeCount; ++Index)
	{
		const FGuid Id(Index + 1, 0, 0, 1);
		MaximumProgram.Nodes.push_back({.Id = Id});
		MaximumPresentation.Nodes.push_back(
			{Id, static_cast<int32>(Index * 10), static_cast<int32>(Index * -5)});
	}
	const FMaterialGraphPresentation Sanitized =
		SanitizeMaterialGraphPresentation(MaximumPresentation, MaximumProgram);
	EXPECT_EQ(Sanitized.Nodes.size(), MaterialProgramMaxNodeCount);

	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "PresentationSource");
	ASSERT_NE(Material, nullptr);
	const FGuid NodeId = Material->GetMaterialProgram()->Nodes.front().Id;
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(
		{.Nodes = {{NodeId, 100, -200}}}));
	std::string Error;
	DMaterial* Duplicate = Cast<DMaterial>(DuplicateObjectGraph(
		Material, nullptr, "PresentationDuplicate", &Error));
	ASSERT_NE(Duplicate, nullptr) << Error;
	EXPECT_EQ(Duplicate->GetMaterialGraphPresentation(),
		Material->GetMaterialGraphPresentation());

	MarkAsGarbage(Duplicate);
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphAuthoringTests, CatalogAndInspectionCoverTheClosedOpcodeDomain)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "GraphCatalogMaterial");
	ASSERT_NE(Material, nullptr);
	const std::vector<FMaterialGraphCatalogEntry> Catalog =
		FMaterialGraphService::EnumerateCatalog(*Material);
	EXPECT_FALSE(Catalog.empty());
	for (uint8 Value = static_cast<uint8>(EMaterialProgramOpcode::Constant);
		Value <= static_cast<uint8>(EMaterialProgramOpcode::BlendNormalsRNM);
		++Value)
	{
		EXPECT_TRUE(std::ranges::any_of(Catalog,
			[Value](const FMaterialGraphCatalogEntry& Entry) {
				return static_cast<uint8>(Entry.NodeTemplate.Opcode) == Value;
			})) << "Missing opcode " << static_cast<uint32>(Value);
	}
	const FMaterialGraphView View = FMaterialGraphService::Inspect(*Material);
	EXPECT_EQ(View.Nodes.size(), Material->GetMaterialProgram()->Nodes.size());
	for (const FMaterialGraphNodeView& Node : View.Nodes)
	{
		EXPECT_EQ(Node.Inputs.size(), Node.Node.Inputs.size());
		for (const FMaterialGraphPinView& Input : Node.Inputs)
			EXPECT_FALSE(Input.AcceptedTypes.empty());
	}

	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphAuthoringTests, MaximumGraphLayoutIsDeterministicAndPresentationOnly)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "MaximumLayoutMaterial");
	ASSERT_NE(Material, nullptr);
	FMaterialProgram MaximumProgram = *Material->GetMaterialProgram();
	while (MaximumProgram.Nodes.size() < MaterialProgramMaxNodeCount)
	{
		FMaterialProgramNode Node;
		Node.Id = FGuid::NewGuid();
		Node.Opcode = EMaterialProgramOpcode::Constant;
		Node.ResultType = EMaterialProgramValueType::Float;
		MaximumProgram.Nodes.push_back(Node);
	}
	FMaterialProgramValidationResult Validation;
	ASSERT_TRUE(Material->SetMaterialProgram(MaximumProgram, Validation));
	const uint64 SemanticRevision =
		Material->GetMaterialCompileStatus().AuthoredRevision;

	const auto Begin = std::chrono::steady_clock::now();
	const FMaterialGraphCommandResult First =
		FMaterialGraphService::Layout(*Material);
	const auto Duration = std::chrono::steady_clock::now() - Begin;
	ASSERT_TRUE(First) << First.Message;
	EXPECT_LT(Duration, std::chrono::seconds(1));
	EXPECT_EQ(Material->GetMaterialGraphPresentation().Nodes.size(),
		MaterialProgramMaxNodeCount);
	EXPECT_EQ(*Material->GetMaterialProgram(), MaximumProgram);
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision,
		SemanticRevision);
	const FMaterialGraphPresentation FirstLayout =
		Material->GetMaterialGraphPresentation();
	const FMaterialGraphCommandResult Second =
		FMaterialGraphService::Layout(*Material);
	EXPECT_EQ(Second.Status, EMaterialGraphCommandStatus::NoChange);
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), FirstLayout);

	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphAuthoringTests, ClipboardPasteRemapsInternalIdentityAndRejectsAtomically)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "ClipboardMaterial");
	ASSERT_NE(Material, nullptr);
	ASSERT_TRUE(FMaterialGraphService::Layout(*Material));
	std::vector<FGuid> AllNodes;
	for (const FMaterialProgramNode& Node : Material->GetMaterialProgram()->Nodes)
		AllNodes.push_back(Node.Id);
	FMaterialGraphClipboardPayload Payload;
	const FMaterialGraphCommandResult Copied =
		FMaterialGraphService::CopySelection(*Material, AllNodes, Payload);
	ASSERT_TRUE(Copied) << Copied.Message;
	ASSERT_EQ(Payload.Nodes.size(), AllNodes.size());
	EXPECT_TRUE(std::ranges::any_of(Payload.Nodes,
		[](const FMaterialGraphClipboardNode& Node) {
			return Node.RelativeX == 0;
		}));
	EXPECT_TRUE(std::ranges::any_of(Payload.Nodes,
		[](const FMaterialGraphClipboardNode& Node) {
			return Node.RelativeY == 0;
		}));

	const FMaterialProgram BeforeProgram = *Material->GetMaterialProgram();
	const FMaterialGraphPresentation BeforePresentation =
		Material->GetMaterialGraphPresentation();
	const FMaterialNormalizationResult BeforeIdentity = Normalize(*Material);
	ASSERT_TRUE(BeforeIdentity);
	FTransactionManager Transactions;
	const FMaterialGraphCommandResult Pasted = FMaterialGraphService::Paste(
		*Material, Payload, 1200, 400, &Transactions);
	ASSERT_TRUE(Pasted) << Pasted.Message;
	ASSERT_EQ(Pasted.GeneratedNodeIds.size(), Payload.Nodes.size());
	std::unordered_set<FGuid> OriginalIds(AllNodes.begin(), AllNodes.end());
	for (const FGuid& Id : Pasted.GeneratedNodeIds)
		EXPECT_FALSE(OriginalIds.contains(Id));
	const FMaterialNormalizationResult AfterIdentity = Normalize(*Material);
	ASSERT_TRUE(AfterIdentity);
	EXPECT_EQ(AfterIdentity.Identity, BeforeIdentity.Identity);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(*Material->GetMaterialProgram(), BeforeProgram);
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), BeforePresentation);
	ASSERT_TRUE(Transactions.Redo());

	FMaterialGraphClipboardPayload UnknownVersion = Payload;
	UnknownVersion.SchemaVersion = 99;
	const FMaterialProgram BeforeRejected = *Material->GetMaterialProgram();
	const FMaterialGraphCommandResult Rejected = FMaterialGraphService::Paste(
		*Material, UnknownVersion, 0, 0, &Transactions);
	EXPECT_EQ(Rejected.Status, EMaterialGraphCommandStatus::Rejected);
	EXPECT_EQ(*Material->GetMaterialProgram(), BeforeRejected);

	const auto Dependent = std::ranges::find_if(
		Material->GetMaterialProgram()->Nodes,
		[](const FMaterialProgramNode& Node) { return !Node.Inputs.empty(); });
	ASSERT_NE(Dependent, Material->GetMaterialProgram()->Nodes.end());
	FMaterialGraphClipboardPayload Incomplete;
	ASSERT_TRUE(FMaterialGraphService::CopySelection(
		*Material, std::span(&Dependent->Id, 1), Incomplete));
	const FMaterialGraphCommandResult IncompletePaste =
		FMaterialGraphService::Paste(*Material, Incomplete, 0, 0, &Transactions);
	EXPECT_EQ(IncompletePaste.Status, EMaterialGraphCommandStatus::Rejected);
	EXPECT_EQ(*Material->GetMaterialProgram(), BeforeRejected);

	FMaterialGraphCreateNodeRequest Standalone;
	Standalone.Node.Opcode = EMaterialProgramOpcode::Constant;
	Standalone.Node.ResultType = EMaterialProgramValueType::Float;
	Standalone.Node.Literal.X = 0.75f;
	Standalone.X = 80;
	Standalone.Y = 120;
	const FMaterialGraphCommandResult StandaloneCreated =
		FMaterialGraphService::CreateNode(*Material, Standalone, &Transactions);
	ASSERT_TRUE(StandaloneCreated);
	const FGuid StandaloneId = StandaloneCreated.GeneratedNodeIds.front();
	const FMaterialGraphCommandResult Duplicated =
		FMaterialGraphService::DuplicateNodes(
			*Material, std::span(&StandaloneId, 1), 40, 40, &Transactions);
	ASSERT_TRUE(Duplicated) << Duplicated.Message;
	ASSERT_EQ(Duplicated.GeneratedNodeIds.size(), 1u);
	EXPECT_NE(Duplicated.GeneratedNodeIds.front(), StandaloneId);
	FMaterialGraphClipboardPayload CutPayload;
	const FMaterialGraphCommandResult Cut = FMaterialGraphService::CutSelection(
		*Material, std::span(&StandaloneId, 1), CutPayload, &Transactions);
	ASSERT_TRUE(Cut) << Cut.Message;
	ASSERT_EQ(CutPayload.Nodes.size(), 1u);
	EXPECT_EQ(CutPayload.Nodes.front().Node.Id, StandaloneId);
	EXPECT_EQ(FindViewNode(FMaterialGraphService::Inspect(*Material), StandaloneId),
		nullptr);
	ASSERT_TRUE(Transactions.Undo());
	const FMaterialGraphView RestoredCut = FMaterialGraphService::Inspect(*Material);
	EXPECT_NE(FindViewNode(RestoredCut, StandaloneId), nullptr);

	Transactions.Clear();
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphAuthoringTests, CommandsAreAtomicAndTransactionsRestoreSemanticAndPresentationState)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "GraphAuthoringMaterial");
	ASSERT_NE(Material, nullptr);
	const FMaterialProgram OriginalProgram = *Material->GetMaterialProgram();
	const uint64 OriginalRevision =
		Material->GetMaterialCompileStatus().AuthoredRevision;
	FTransactionManager Transactions;

	FMaterialGraphCreateNodeRequest Create;
	Create.Node.Opcode = EMaterialProgramOpcode::Constant;
	Create.Node.ResultType = EMaterialProgramValueType::Float;
	Create.Node.Literal.X = 0.25f;
	Create.X = 120;
	Create.Y = -80;
	const FMaterialGraphCommandResult Created =
		FMaterialGraphService::CreateNode(*Material, Create, &Transactions);
	ASSERT_TRUE(Created) << Created.Message;
	ASSERT_EQ(Created.GeneratedNodeIds.size(), 1u);
	const FGuid CreatedId = Created.GeneratedNodeIds.front();
	EXPECT_EQ(Material->GetMaterialProgram()->Nodes.size(),
		OriginalProgram.Nodes.size() + 1);
	EXPECT_GT(Material->GetMaterialCompileStatus().AuthoredRevision,
		OriginalRevision);
	const uint64 SemanticRevision =
		Material->GetMaterialCompileStatus().AuthoredRevision;

	const FMaterialGraphNodePresentation Moved{CreatedId, 320, 160};
	const FMaterialGraphCommandResult Move =
		FMaterialGraphService::MoveNodes(*Material, std::span(&Moved, 1), &Transactions);
	ASSERT_TRUE(Move) << Move.Message;
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision,
		SemanticRevision);
	const FMaterialGraphView MovedGraph = FMaterialGraphService::Inspect(*Material);
	const FMaterialGraphNodeView* MovedView = FindViewNode(MovedGraph, CreatedId);
	ASSERT_NE(MovedView, nullptr);
	ASSERT_TRUE(MovedView->Presentation.has_value());
	EXPECT_EQ(MovedView->Presentation->X, 320);

	FMaterialProgram BeforeRejected = *Material->GetMaterialProgram();
	const auto CreatedNodeIt = std::ranges::find(
		BeforeRejected.Nodes, CreatedId, &FMaterialProgramNode::Id);
	ASSERT_NE(CreatedNodeIt, BeforeRejected.Nodes.end());
	FMaterialProgramNode Invalid = *CreatedNodeIt;
	Invalid.ResultType = EMaterialProgramValueType::Texture2D;
	const FMaterialGraphCommandResult Rejected =
		FMaterialGraphService::ReplaceNode(*Material, std::move(Invalid), &Transactions);
	EXPECT_EQ(Rejected.Status, EMaterialGraphCommandStatus::Rejected);
	EXPECT_FALSE(Rejected.Diagnostics.empty());
	EXPECT_EQ(*Material->GetMaterialProgram(), BeforeRejected);
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision,
		SemanticRevision);

	ASSERT_TRUE(Transactions.Undo());
	const FMaterialGraphView UnmovedGraph = FMaterialGraphService::Inspect(*Material);
	const FMaterialGraphNodeView* UnmovedView = FindViewNode(UnmovedGraph, CreatedId);
	ASSERT_NE(UnmovedView, nullptr);
	ASSERT_TRUE(UnmovedView->Presentation.has_value());
	EXPECT_EQ(UnmovedView->Presentation->X, 120);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(*Material->GetMaterialProgram(), OriginalProgram);
	ASSERT_TRUE(Transactions.Redo());
	const FMaterialGraphView RecreatedGraph = FMaterialGraphService::Inspect(*Material);
	EXPECT_NE(FindViewNode(RecreatedGraph, CreatedId), nullptr);
	ASSERT_TRUE(Transactions.Redo());
	const FMaterialGraphView RemovedGraph = FMaterialGraphService::Inspect(*Material);
	const FMaterialGraphNodeView* RedoneView = FindViewNode(RemovedGraph, CreatedId);
	ASSERT_NE(RedoneView, nullptr);
	ASSERT_TRUE(RedoneView->Presentation.has_value());
	EXPECT_EQ(RedoneView->Presentation->X, 320);

	FMaterialGraphMoveSession MoveSession;
	ASSERT_TRUE(MoveSession.Begin(*Material, std::span(&CreatedId, 1), &Transactions));
	const FMaterialGraphNodePresentation FirstPreview{CreatedId, 400, 200};
	const FMaterialGraphNodePresentation SecondPreview{CreatedId, 480, 240};
	ASSERT_TRUE(MoveSession.Apply(std::span(&FirstPreview, 1)));
	ASSERT_TRUE(MoveSession.Apply(std::span(&SecondPreview, 1)));
	ASSERT_TRUE(MoveSession.Commit());
	ASSERT_TRUE(Transactions.Undo());
	const FMaterialGraphView CoalescedUndoGraph = FMaterialGraphService::Inspect(*Material);
	const FMaterialGraphNodeView* CoalescedUndo = FindViewNode(
		CoalescedUndoGraph, CreatedId);
	ASSERT_NE(CoalescedUndo, nullptr);
	ASSERT_TRUE(CoalescedUndo->Presentation.has_value());
	EXPECT_EQ(CoalescedUndo->Presentation->X, 320);

	Transactions.Clear();
	MarkAsGarbage(Material);
	CollectGarbage();
}
