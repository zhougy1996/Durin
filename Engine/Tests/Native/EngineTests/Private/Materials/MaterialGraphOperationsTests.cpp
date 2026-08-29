#include "MaterialGraphOperations.h"
#include "Graph/MaterialGraphCanvas.h"

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
		Input.Environment.CompilerIdentity = "material-graph-operations-test";
		Input.Environment.Target = "vulkan-spirv-1.5";
		return NormalizeMaterialProgram(Input);
	}

	auto MakeExpandedGraphMaterial(const char* Name) -> DMaterial*
	{
		DMaterial* Material = NewObject<DMaterial>(nullptr, Name);
		FMaterialProgramValidationResult Validation;
		if (!Material || !Material->SetMaterialProgram(
			MakeCanonicalMaterialProgram(), Validation)) return nullptr;
		return Material;
	}
}

TEST(FMaterialGraphOperationsTests, PresentationSanitizationIsIndependentAndBounded)
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
	Presentation.bHasMaterialOutputPosition = true;
	Presentation.MaterialOutputX = 640;
	Presentation.MaterialOutputY = -120;

	const FMaterialGraphPresentation Sanitized =
		SanitizeMaterialGraphPresentation(Presentation, Program);
	EXPECT_EQ(Sanitized.SchemaVersion,
		CurrentMaterialGraphPresentationSchemaVersion);
	ASSERT_EQ(Sanitized.Nodes.size(), 1u);
	EXPECT_EQ(Sanitized.Nodes.front().NodeId, Program.Nodes[1].Id);
	EXPECT_EQ(Sanitized.Nodes.front().X, 20);
	EXPECT_EQ(Sanitized.Nodes.front().Y, 40);
	EXPECT_TRUE(Sanitized.bHasMaterialOutputPosition);
	EXPECT_EQ(Sanitized.MaterialOutputX, 640);
	EXPECT_EQ(Sanitized.MaterialOutputY, -120);
}

TEST(FMaterialGraphOperationsTests, PresentationReachesMaximumNodeBoundAndDuplicatesByReflection)
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
	DMaterial* Material = MakeExpandedGraphMaterial("PresentationSource");
	ASSERT_NE(Material, nullptr);
	const FGuid NodeId = Material->GetMaterialProgram()->Nodes.front().Id;
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(
		{.Nodes = {{NodeId, 100, -200}},
			.bHasMaterialOutputPosition = true,
			.MaterialOutputX = 420,
			.MaterialOutputY = -30}));
	DMaterial* Duplicate = Cast<DMaterial>(DuplicateObject(
		Material, nullptr, "PresentationDuplicate"));
	ASSERT_NE(Duplicate, nullptr);
	EXPECT_EQ(Duplicate->GetMaterialGraphPresentation(),
		Material->GetMaterialGraphPresentation());

	MarkAsGarbage(Duplicate);
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, MaterialOutputMovementIsPresentationOnlyAndTransactional)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "MaterialOutputMovement");
	ASSERT_NE(Material, nullptr);
	const uint64 Revision = Material->GetMaterialCompileStatus().AuthoredRevision;
	FTransactionManager Transactions;

	ASSERT_TRUE(FMaterialGraphOperations::MoveMaterialOutput(
		*Material, 520, -80, &Transactions));
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision, Revision);
	EXPECT_TRUE(Material->GetMaterialGraphPresentation().bHasMaterialOutputPosition);
	EXPECT_EQ(Material->GetMaterialGraphPresentation().MaterialOutputX, 520);
	EXPECT_EQ(Material->GetMaterialGraphPresentation().MaterialOutputY, -80);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_FALSE(Material->GetMaterialGraphPresentation().bHasMaterialOutputPosition);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Material->GetMaterialGraphPresentation().MaterialOutputX, 520);

	Transactions.Clear();
	FMaterialGraphMoveSession Move;
	ASSERT_TRUE(Move.BeginMaterialOutput(*Material, &Transactions));
	ASSERT_TRUE(Move.ApplyMaterialOutput(600, 40));
	ASSERT_TRUE(Move.Cancel());
	EXPECT_EQ(Material->GetMaterialGraphPresentation().MaterialOutputX, 520);
	EXPECT_FALSE(Transactions.CanUndo());
	ASSERT_TRUE(Move.BeginMaterialOutput(*Material, &Transactions));
	ASSERT_TRUE(Move.ApplyMaterialOutput(600, 40));
	ASSERT_TRUE(Move.Commit());
	EXPECT_EQ(Material->GetMaterialGraphPresentation().MaterialOutputX, 600);
	EXPECT_TRUE(Transactions.CanUndo());

	Transactions.Clear();
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, CatalogAndInspectionCoverTheClosedOpcodeDomain)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "GraphCatalogMaterial");
	ASSERT_NE(Material, nullptr);
	const std::vector<FMaterialGraphCatalogEntry> Catalog =
		FMaterialGraphOperations::EnumerateCatalog(*Material);
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
	const FMaterialGraphView View = FMaterialGraphOperations::Inspect(*Material);
	EXPECT_EQ(View.Nodes.size(), Material->GetMaterialProgram()->Nodes.size());
	for (const FMaterialGraphNodeView& Node : View.Nodes)
	{
		EXPECT_FALSE(Node.PrimaryLabel.empty());
		EXPECT_EQ(Node.Inputs.size(), Node.Node.Inputs.size());
		for (const FMaterialGraphPinView& Input : Node.Inputs)
		{
			EXPECT_FALSE(Input.Name.empty());
			EXPECT_FALSE(Input.AcceptedTypes.empty());
		}
	}
	for (const FMaterialGraphCatalogEntry& Entry : Catalog)
	{
		EXPECT_FALSE(Entry.OperationName.empty());
		EXPECT_FALSE(Entry.Category.empty());
		EXPECT_FALSE(Entry.Description.empty());
		EXPECT_EQ(Entry.InputNames.size(), Entry.AcceptedInputTypes.size());
	}
	const std::vector<FMaterialGraphCatalogEntry> MultiplyResults =
		FMaterialGraphOperations::SearchCatalog(*Material, "multiply");
	ASSERT_FALSE(MultiplyResults.empty());
	EXPECT_EQ(MultiplyResults.front().OperationName, "Multiply");
	const std::vector<FMaterialGraphCatalogEntry> TextureSourceResults =
		FMaterialGraphOperations::SearchCatalog(*Material, {},
			EMaterialProgramValueType::Texture2D);
	ASSERT_FALSE(TextureSourceResults.empty());
	for (const FMaterialGraphCatalogEntry& Entry : TextureSourceResults)
	{
		ASSERT_FALSE(Entry.AcceptedInputTypes.empty());
		EXPECT_NE(std::ranges::find(Entry.AcceptedInputTypes.front(),
			EMaterialProgramValueType::Texture2D),
			Entry.AcceptedInputTypes.front().end());
	}

	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, CanvasGeometryUsesStableMetricsAndZoomHysteresis)
{
	const FMaterialGraphCanvasMetrics& Metrics = FMaterialGraphGeometry::GetMetrics();
	EXPECT_FLOAT_EQ(Metrics.NodeWidth, 224.0f);
	EXPECT_FLOAT_EQ(Metrics.MinimumHitDiameter, 16.0f);
	EXPECT_GE(Metrics.SurfaceHeaderHeight, 48.0f);
	EXPECT_LE(Metrics.BodyPadding + Metrics.SurfaceLabelWidth
		+ Metrics.SurfaceValueGap + Metrics.SurfaceValueWidth + Metrics.BodyPadding,
		Metrics.SurfaceWidth);
	EXPECT_FLOAT_EQ(FMaterialGraphGeometry::GetNodeHeight(0), 94.0f);
	EXPECT_FLOAT_EQ(FMaterialGraphGeometry::GetNodeHeight(3), 142.0f);

	EXPECT_EQ(FMaterialGraphGeometry::SelectDetailLevel(
		0.40f, EMaterialGraphDetailLevel::Readable),
		EMaterialGraphDetailLevel::Overview);
	EXPECT_EQ(FMaterialGraphGeometry::SelectDetailLevel(
		0.45f, EMaterialGraphDetailLevel::Overview),
		EMaterialGraphDetailLevel::Overview);
	EXPECT_EQ(FMaterialGraphGeometry::SelectDetailLevel(
		0.50f, EMaterialGraphDetailLevel::Overview),
		EMaterialGraphDetailLevel::Readable);
	EXPECT_EQ(FMaterialGraphGeometry::SelectDetailLevel(
		0.84f, EMaterialGraphDetailLevel::Readable),
		EMaterialGraphDetailLevel::Editing);
	EXPECT_EQ(FMaterialGraphGeometry::SelectDetailLevel(
		0.78f, EMaterialGraphDetailLevel::Editing),
		EMaterialGraphDetailLevel::Editing);
	EXPECT_EQ(FMaterialGraphGeometry::SelectDetailLevel(
		0.70f, EMaterialGraphDetailLevel::Editing),
		EMaterialGraphDetailLevel::Readable);
}

TEST(FMaterialGraphOperationsTests, PaletteCreationAddsVisibleDefaultsInOneTransaction)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "PaletteCreationMaterial");
	ASSERT_NE(Material, nullptr);
	const std::vector<FMaterialGraphCatalogEntry> Catalog =
		FMaterialGraphOperations::SearchCatalog(*Material, "multiply");
	const auto Multiply = std::ranges::find_if(Catalog,
		[](const FMaterialGraphCatalogEntry& Entry) {
			return Entry.OperationName == "Multiply"
				&& Entry.NodeTemplate.ResultType == EMaterialProgramValueType::Float3;
		});
	ASSERT_NE(Multiply, Catalog.end());

	const FMaterialProgram Before = *Material->GetMaterialProgram();
	FTransactionManager Transactions;
	const FMaterialGraphCommandResult Created =
		FMaterialGraphOperations::CreateNodeWithDefaultInputs(*Material, {
			.Node = Multiply->NodeTemplate,
			.X = 400,
			.Y = 200,
		}, Multiply->AcceptedInputTypes, &Transactions);
	ASSERT_TRUE(Created) << Created.Message;
	ASSERT_EQ(Created.GeneratedNodeIds.size(), 3u);
	const FMaterialGraphView View = FMaterialGraphOperations::Inspect(*Material);
	const FMaterialGraphNodeView* Node = FindViewNode(View, Created.GeneratedNodeIds.front());
	ASSERT_NE(Node, nullptr);
	ASSERT_EQ(Node->Node.Inputs.size(), 2u);
	for (const FMaterialProgramLink& Link : Node->Node.Inputs)
	{
		EXPECT_TRUE(Link.SourceNodeId.IsValid());
		const FMaterialGraphNodeView* Default = FindViewNode(View, Link.SourceNodeId);
		ASSERT_NE(Default, nullptr);
		EXPECT_EQ(Default->Node.Opcode, EMaterialProgramOpcode::Constant);
		EXPECT_EQ(Default->Node.ResultType, EMaterialProgramValueType::Float3);
		EXPECT_TRUE(Default->Presentation.has_value());
	}
	const float DefaultHeight = FMaterialGraphGeometry::GetNodeHeight(0);
	const float DefaultGap = FMaterialGraphGeometry::GetMetrics().RowGap;
	std::vector<int32> DefaultRows;
	for (const FMaterialProgramLink& Link : Node->Node.Inputs)
	{
		const FMaterialGraphNodeView* Default = FindViewNode(View, Link.SourceNodeId);
		ASSERT_TRUE(Default && Default->Presentation);
		DefaultRows.push_back(Default->Presentation->Y);
	}
	std::ranges::sort(DefaultRows);
	for (size_t Index = 1; Index < DefaultRows.size(); ++Index)
		EXPECT_GE(DefaultRows[Index] - DefaultRows[Index - 1],
			static_cast<int32>(DefaultHeight + DefaultGap));
	const FMaterialGraphNodeView* IdentityDefault =
		FindViewNode(View, Node->Node.Inputs[1].SourceNodeId);
	ASSERT_NE(IdentityDefault, nullptr);
	EXPECT_FLOAT_EQ(IdentityDefault->Node.Literal.X, 1.0f);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(*Material->GetMaterialProgram(), Before);

	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, MaximumGraphLayoutIsDeterministicAndPresentationOnly)
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
		FMaterialGraphOperations::Layout(*Material);
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
	const FMaterialGraphView LayoutView = FMaterialGraphOperations::Inspect(*Material);
	for (size_t A = 0; A < LayoutView.Nodes.size(); ++A)
		for (size_t B = A + 1; B < LayoutView.Nodes.size(); ++B)
		{
			ASSERT_TRUE(LayoutView.Nodes[A].Presentation);
			ASSERT_TRUE(LayoutView.Nodes[B].Presentation);
			const auto& PositionA = *LayoutView.Nodes[A].Presentation;
			const auto& PositionB = *LayoutView.Nodes[B].Presentation;
			const float HeightA = FMaterialGraphGeometry::GetNodeHeight(
				static_cast<uint32>(LayoutView.Nodes[A].Inputs.size()));
			const float HeightB = FMaterialGraphGeometry::GetNodeHeight(
				static_cast<uint32>(LayoutView.Nodes[B].Inputs.size()));
			const float Width = FMaterialGraphGeometry::GetMetrics().NodeWidth;
			EXPECT_FALSE(PositionA.X < PositionB.X + Width
				&& PositionA.X + Width > PositionB.X
				&& PositionA.Y < PositionB.Y + HeightB
				&& PositionA.Y + HeightA > PositionB.Y);
		}
	const FMaterialGraphCommandResult Second =
		FMaterialGraphOperations::Layout(*Material);
	EXPECT_EQ(Second.Status, EMaterialGraphCommandStatus::NoChange);
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), FirstLayout);
	std::vector<std::chrono::microseconds> Samples;
	Samples.reserve(100);
	for (uint32 Sample = 0; Sample < 100; ++Sample)
	{
		const auto SampleBegin = std::chrono::steady_clock::now();
		EXPECT_TRUE(FMaterialGraphOperations::Layout(*Material));
		Samples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - SampleBegin));
	}
	std::ranges::sort(Samples);
	EXPECT_LT(Samples[50], std::chrono::milliseconds(25));
	EXPECT_LT(Samples[95], std::chrono::milliseconds(50));

	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, LayoutReducesDenseCrossingsAndAvoidsSelectedCollisions)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "DenseLayoutMaterial");
	ASSERT_NE(Material, nullptr);
	FMaterialProgram Program = *Material->GetMaterialProgram();
	std::array<FGuid, 8> Sources;
	std::array<FGuid, 8> Consumers;
	for (uint32 Index = 0; Index < Sources.size(); ++Index)
	{
		Sources[Index] = FGuid(100 + Index, 0, 0, 1);
		Consumers[Index] = FGuid(200 + Index, 0, 0, 1);
	}
	for (uint32 Index = 0; Index < Sources.size(); ++Index)
	{
		Program.Nodes.push_back({
			.Id = Sources[Index],
			.Opcode = EMaterialProgramOpcode::Constant,
			.ResultType = EMaterialProgramValueType::Float,
		});
		Program.Nodes.push_back({
			.Id = Consumers[Index],
			.Opcode = EMaterialProgramOpcode::Saturate,
			.ResultType = EMaterialProgramValueType::Float,
			.Inputs = {{Sources[Sources.size() - Index - 1], 0}},
		});
	}
	FMaterialProgramValidationResult Validation;
	ASSERT_TRUE(Material->SetMaterialProgram(Program, Validation));
	ASSERT_TRUE(FMaterialGraphOperations::Layout(*Material));
	const FMaterialGraphView View = FMaterialGraphOperations::Inspect(*Material);
	auto Y = [&](const FGuid& Id) {
		const FMaterialGraphNodeView* Node = FindViewNode(View, Id);
		EXPECT_NE(Node, nullptr);
		EXPECT_TRUE(Node && Node->Presentation);
		return Node && Node->Presentation ? Node->Presentation->Y : 0;
	};
	uint32 Crossings = 0;
	for (size_t A = 0; A < Sources.size(); ++A)
		for (size_t B = A + 1; B < Sources.size(); ++B)
			if (static_cast<int64>(Y(Sources[Sources.size() - A - 1])
				- Y(Sources[Sources.size() - B - 1]))
				* static_cast<int64>(Y(Consumers[A]) - Y(Consumers[B])) < 0)
				++Crossings;
	EXPECT_LE(Crossings, 2u);

	const FGuid Selected = Sources.front();
	const FGuid Fixed = Sources.back();
	const FMaterialGraphNodeView* SelectedView = FindViewNode(View, Selected);
	ASSERT_NE(SelectedView, nullptr);
	ASSERT_TRUE(SelectedView->Presentation);
	const FMaterialGraphNodePresentation Occupied{
		Fixed, SelectedView->Presentation->X, 0};
	ASSERT_TRUE(FMaterialGraphOperations::MoveNodes(*Material, std::span(&Occupied, 1)));
	ASSERT_TRUE(FMaterialGraphOperations::Layout(*Material, std::span(&Selected, 1)));
	const FMaterialGraphView Relayout = FMaterialGraphOperations::Inspect(*Material);
	const auto* RelayoutSelected = FindViewNode(Relayout, Selected);
	const auto* RelayoutFixed = FindViewNode(Relayout, Fixed);
	ASSERT_TRUE(RelayoutSelected && RelayoutSelected->Presentation);
	ASSERT_TRUE(RelayoutFixed && RelayoutFixed->Presentation);
	const float Height = FMaterialGraphGeometry::GetNodeHeight(0);
	const float Width = FMaterialGraphGeometry::GetMetrics().NodeWidth;
	EXPECT_FALSE(RelayoutSelected->Presentation->X < RelayoutFixed->Presentation->X + Width
		&& RelayoutSelected->Presentation->X + Width > RelayoutFixed->Presentation->X
		&& RelayoutSelected->Presentation->Y < RelayoutFixed->Presentation->Y + Height
		&& RelayoutSelected->Presentation->Y + Height > RelayoutFixed->Presentation->Y);

	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests,
	ClipboardPasteRemapsIdentityPreservesExternalLinksAndRejectsAtomically)
{
	InitializeDObjectSystem();
	DMaterial* Material = MakeExpandedGraphMaterial("ClipboardMaterial");
	ASSERT_NE(Material, nullptr);
	ASSERT_TRUE(FMaterialGraphOperations::Layout(*Material));
	std::vector<FGuid> AllNodes;
	for (const FMaterialProgramNode& Node : Material->GetMaterialProgram()->Nodes)
		AllNodes.push_back(Node.Id);
	FMaterialGraphClipboardPayload Payload;
	const FMaterialGraphCommandResult Copied =
		FMaterialGraphOperations::CopySelection(*Material, AllNodes, Payload);
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
	const FMaterialGraphCommandResult Pasted = FMaterialGraphOperations::Paste(
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
	const FMaterialGraphCommandResult Rejected = FMaterialGraphOperations::Paste(
		*Material, UnknownVersion, 0, 0, &Transactions);
	EXPECT_EQ(Rejected.Status, EMaterialGraphCommandStatus::Rejected);
	EXPECT_EQ(*Material->GetMaterialProgram(), BeforeRejected);

	const auto Dependent = std::ranges::find_if(
		Material->GetMaterialProgram()->Nodes,
		[](const FMaterialProgramNode& Node) { return !Node.Inputs.empty(); });
	ASSERT_NE(Dependent, Material->GetMaterialProgram()->Nodes.end());
	const FGuid RequiredSource = Dependent->Inputs.front().SourceNodeId;
	const FMaterialGraphCommandResult RequiredRemoval =
		FMaterialGraphOperations::RemoveNodes(
			*Material, std::span(&RequiredSource, 1), &Transactions);
	EXPECT_EQ(RequiredRemoval.Status, EMaterialGraphCommandStatus::Rejected);
	EXPECT_EQ(*Material->GetMaterialProgram(), BeforeRejected);
	const std::vector<FMaterialProgramLink> ExternalInputs = Dependent->Inputs;
	FMaterialGraphClipboardPayload Partial;
	ASSERT_TRUE(FMaterialGraphOperations::CopySelection(
		*Material, std::span(&Dependent->Id, 1), Partial));
	ASSERT_EQ(Partial.Nodes.size(), 1u);
	EXPECT_EQ(Partial.Nodes.front().Node.Inputs, ExternalInputs);
	const FMaterialGraphCommandResult PartialPaste =
		FMaterialGraphOperations::Paste(*Material, Partial, 0, 0, &Transactions);
	ASSERT_TRUE(PartialPaste) << PartialPaste.Message;
	ASSERT_EQ(PartialPaste.GeneratedNodeIds.size(), 1u);
	const auto PastedDependent = std::ranges::find(
		Material->GetMaterialProgram()->Nodes, PartialPaste.GeneratedNodeIds.front(),
		&FMaterialProgramNode::Id);
	ASSERT_NE(PastedDependent, Material->GetMaterialProgram()->Nodes.end());
	EXPECT_EQ(PastedDependent->Inputs, ExternalInputs);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(*Material->GetMaterialProgram(), BeforeRejected);

	FMaterialGraphCreateNodeRequest Standalone;
	Standalone.Node.Opcode = EMaterialProgramOpcode::Constant;
	Standalone.Node.ResultType = EMaterialProgramValueType::Float;
	Standalone.Node.Literal.X = 0.75f;
	Standalone.X = 80;
	Standalone.Y = 120;
	const FMaterialGraphCommandResult StandaloneCreated =
		FMaterialGraphOperations::CreateNode(*Material, Standalone, &Transactions);
	ASSERT_TRUE(StandaloneCreated);
	const FGuid StandaloneId = StandaloneCreated.GeneratedNodeIds.front();
	const FMaterialGraphCommandResult Duplicated =
		FMaterialGraphOperations::DuplicateNodes(
			*Material, std::span(&StandaloneId, 1), 40, 40, &Transactions);
	ASSERT_TRUE(Duplicated) << Duplicated.Message;
	ASSERT_EQ(Duplicated.GeneratedNodeIds.size(), 1u);
	EXPECT_NE(Duplicated.GeneratedNodeIds.front(), StandaloneId);
	FMaterialGraphClipboardPayload CutPayload;
	const FMaterialGraphCommandResult Cut = FMaterialGraphOperations::CutSelection(
		*Material, std::span(&StandaloneId, 1), CutPayload, &Transactions);
	ASSERT_TRUE(Cut) << Cut.Message;
	ASSERT_EQ(CutPayload.Nodes.size(), 1u);
	EXPECT_EQ(CutPayload.Nodes.front().Node.Id, StandaloneId);
	EXPECT_EQ(FindViewNode(FMaterialGraphOperations::Inspect(*Material), StandaloneId),
		nullptr);
	ASSERT_TRUE(Transactions.Undo());
	const FMaterialGraphView RestoredCut = FMaterialGraphOperations::Inspect(*Material);
	EXPECT_NE(FindViewNode(RestoredCut, StandaloneId), nullptr);

	Transactions.Clear();
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, DiagnosticNavigationIsLocatedAndDocumentLocal)
{
	const FGuid FirstNode = FGuid::NewGuid();
	const FGuid SecondNode = FGuid::NewGuid();
	FMaterialGraphCanvas FirstCanvas;
	FMaterialGraphCanvas SecondCanvas;
	EXPECT_TRUE(FirstCanvas.SelectAndFrameDiagnostic({
		.LocationKind = EMaterialProgramDiagnosticLocationKind::Input,
		.NodeId = FirstNode,
		.LocationIndex = 1,
	}));
	EXPECT_TRUE(FirstCanvas.GetSelection().contains(FirstNode));
	EXPECT_TRUE(SecondCanvas.GetSelection().empty());
	EXPECT_TRUE(SecondCanvas.SelectAndFrame(SecondNode));
	EXPECT_TRUE(SecondCanvas.GetSelection().contains(SecondNode));
	EXPECT_FALSE(FirstCanvas.GetSelection().contains(SecondNode));

	EXPECT_TRUE(FirstCanvas.SelectAndFrameDiagnostic({
		.LocationKind = EMaterialProgramDiagnosticLocationKind::SurfaceOutput,
		.LocationIndex = static_cast<uint32>(EMaterialSurfaceOutput::Roughness),
	}));
	EXPECT_TRUE(FirstCanvas.GetSelection().empty());
	EXPECT_EQ(FirstCanvas.GetSelectedSurfaceOutput(),
		EMaterialSurfaceOutput::Roughness);
	EXPECT_FALSE(FirstCanvas.SelectAndFrameDiagnostic({
		.LocationKind = EMaterialProgramDiagnosticLocationKind::Program,
	}));
	EXPECT_FALSE(FirstCanvas.SelectAndFrameDiagnostic({
		.LocationKind = EMaterialProgramDiagnosticLocationKind::SurfaceOutput,
		.LocationIndex = 99,
	}));
}

TEST(FMaterialGraphOperationsTests, CanvasProducesBoundedEditingDrawData)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "RenderedGraphMaterial");
	ASSERT_NE(Material, nullptr);
	ImGuiContext* Context = ImGui::CreateContext();
	ASSERT_NE(Context, nullptr);
	ImGuiIO& IO = ImGui::GetIO();
	IO.DisplaySize = {1200.0f, 720.0f};
	IO.DeltaTime = 1.0f / 60.0f;
	IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault();
	IO.Fonts->Build();
	FTransactionManager Transactions;
	FMaterialGraphCanvas Canvas;

	const auto DrawAtZoom = [&](float Zoom) {
		Canvas.SetViewport(Zoom, {40.0f, 40.0f});
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0.0f, 0.0f});
		ImGui::SetNextWindowSize({1200.0f, 720.0f});
		ImGui::Begin("Material Graph Render Test", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		Canvas.Draw(*Material, Transactions, 660.0f,
			[](std::string Message) { FAIL() << Message; });
		ImGui::End();
		ImGui::Render();
		const ImDrawData* DrawData = ImGui::GetDrawData();
		EXPECT_NE(DrawData, nullptr);
		return DrawData ? DrawData->TotalVtxCount : 0;
	};
	const int EditingVertices = DrawAtZoom(1.0f);
	const int OverviewVertices = DrawAtZoom(0.30f);
	EXPECT_GT(EditingVertices, 100);
	EXPECT_LT(EditingVertices, 100000);
	EXPECT_GT(OverviewVertices, 100);
	EXPECT_LT(OverviewVertices, 100000);
	EXPECT_NE(OverviewVertices, EditingVertices);

	FMaterialProgram DenseProgram = *Material->GetMaterialProgram();
	std::array<FGuid, 8> DenseSources;
	for (uint32 Index = 0; Index < DenseSources.size(); ++Index)
		DenseSources[Index] = FGuid(300 + Index, 0, 0, 1);
	for (uint32 Index = 0; Index < DenseSources.size(); ++Index)
	{
		DenseProgram.Nodes.push_back({
			.Id = DenseSources[Index],
			.Opcode = EMaterialProgramOpcode::Constant,
			.ResultType = EMaterialProgramValueType::Float,
			.DisplayName = Index == 0
				? "Ambient Occlusion Texture With A Deliberately Long Authored Name" : "",
		});
		DenseProgram.Nodes.push_back({
			.Id = FGuid(400 + Index, 0, 0, 1),
			.Opcode = EMaterialProgramOpcode::Saturate,
			.ResultType = EMaterialProgramValueType::Float,
			.Inputs = {{DenseSources[DenseSources.size() - Index - 1], 0}},
		});
	}
	FMaterialProgramValidationResult Validation;
	ASSERT_TRUE(Material->SetMaterialProgram(DenseProgram, Validation));
	const int DenseVertices = DrawAtZoom(0.55f);
	EXPECT_GT(DenseVertices, 100);
	EXPECT_LT(DenseVertices, 100000);

	FMaterialProgram MaximumProgram = *Material->GetMaterialProgram();
	uint32 MaximumIndex = 1000;
	while (MaximumProgram.Nodes.size() < MaterialProgramMaxNodeCount)
	{
		MaximumProgram.Nodes.push_back({
			.Id = FGuid(MaximumIndex++, 0, 0, 1),
			.Opcode = EMaterialProgramOpcode::Constant,
			.ResultType = EMaterialProgramValueType::Float,
		});
	}
	ASSERT_TRUE(Material->SetMaterialProgram(MaximumProgram, Validation));
	const int MaximumVertices = DrawAtZoom(0.30f);
	EXPECT_GT(MaximumVertices, 100);
	EXPECT_LT(MaximumVertices, 100000);

	Transactions.Clear();
	ImGui::DestroyContext(Context);
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, CanvasPointerGesturesCancelDeselectAndReconnect)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "CanvasInteractionMaterial");
	ASSERT_NE(Material, nullptr);
	FTransactionManager Transactions;
	FMaterialGraphCreateNodeRequest Create;
	Create.Node.Opcode = EMaterialProgramOpcode::Constant;
	Create.Node.ResultType = EMaterialProgramValueType::Float3;
	Create.Node.Literal.X = 0.5f;
	Create.X = 120;
	Create.Y = 120;
	const FMaterialGraphCommandResult Created =
		FMaterialGraphOperations::CreateNode(*Material, Create, &Transactions);
	ASSERT_TRUE(Created) << Created.Message;
	ASSERT_EQ(Created.GeneratedNodeIds.size(), 1u);
	const FGuid NodeId = Created.GeneratedNodeIds.front();
	Transactions.Clear();

	ImGuiContext* Context = ImGui::CreateContext();
	ASSERT_NE(Context, nullptr);
	ImGuiIO& IO = ImGui::GetIO();
	IO.DisplaySize = {1200.0f, 720.0f};
	IO.DeltaTime = 1.0f / 60.0f;
	IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault();
	IO.Fonts->Build();
	FMaterialGraphCanvas Canvas;
	Canvas.SetViewport(1.0f, {40.0f, 40.0f});
	std::vector<std::string> Errors;
	const auto DrawFrame = [&] {
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0.0f, 0.0f});
		ImGui::SetNextWindowSize({1200.0f, 720.0f});
		ImGui::Begin("Material Graph Interaction Test", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		Canvas.Draw(*Material, Transactions, 660.0f,
			[&Errors](std::string Message) { Errors.push_back(std::move(Message)); });
		ImGui::End();
		ImGui::Render();
	};
	const auto Position = [&]() {
		const FMaterialGraphView View = FMaterialGraphOperations::Inspect(*Material);
		const FMaterialGraphNodeView* Node = FindViewNode(View, NodeId);
		EXPECT_NE(Node, nullptr);
		EXPECT_TRUE(Node && Node->Presentation);
		return Node && Node->Presentation
			? *Node->Presentation : FMaterialGraphNodePresentation{};
	};

	IO.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
	DrawFrame();
	constexpr ImVec2 EmptyCanvasPoint{1000.0f, 600.0f};
	IO.AddMousePosEvent(EmptyCanvasPoint.x, EmptyCanvasPoint.y);
	DrawFrame();
	IO.AddMouseButtonEvent(ImGuiMouseButton_Middle, true);
	DrawFrame();
	IO.AddMousePosEvent(EmptyCanvasPoint.x + 60.0f, EmptyCanvasPoint.y + 30.0f);
	DrawFrame();
	const auto [PannedZoom, PannedOffset] = Canvas.GetViewport();
	EXPECT_FLOAT_EQ(PannedZoom, 1.0f);
	EXPECT_GT(PannedOffset.x, 40.0f);
	EXPECT_GT(PannedOffset.y, 40.0f);
	IO.AddMouseButtonEvent(ImGuiMouseButton_Middle, false);
	DrawFrame();
	Canvas.SetViewport(1.0f, {40.0f, 40.0f});
	const FMaterialGraphNodePresentation AuthoredPosition{NodeId, 120, 120};
	ASSERT_TRUE(FMaterialGraphOperations::MoveNodes(
		*Material, std::span(&AuthoredPosition, 1)));
	const FMaterialGraphNodePresentation Before = Position();
	// With the default ImGui style, this is the center of the authored node header:
	// child content origin + canvas toolbar + viewport pan + graph position.
	constexpr ImVec2 NodeHeaderPoint{288.0f, 230.0f};
	IO.AddMousePosEvent(NodeHeaderPoint.x, NodeHeaderPoint.y);
	DrawFrame();
	IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	DrawFrame();
	ASSERT_TRUE(Canvas.GetSelection().contains(NodeId));
	IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	DrawFrame();
	IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	DrawFrame();
	IO.AddMousePosEvent(NodeHeaderPoint.x + 80.0f, NodeHeaderPoint.y + 40.0f);
	DrawFrame();
	EXPECT_NE(Position(), Before);
	IO.AddKeyEvent(ImGuiKey_Escape, true);
	DrawFrame();
	EXPECT_EQ(Position(), Before);
	IO.AddKeyEvent(ImGuiKey_Escape, false);
	IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	DrawFrame();
	EXPECT_FALSE(Transactions.CanUndo());

	IO.AddMousePosEvent(NodeHeaderPoint.x, NodeHeaderPoint.y);
	DrawFrame();
	IO.AddKeyEvent(ImGuiMod_Ctrl, true);
	IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	DrawFrame();
	EXPECT_TRUE(Canvas.GetSelection().empty());
	EXPECT_TRUE(Errors.empty());
	IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	IO.AddKeyEvent(ImGuiMod_Ctrl, false);
	DrawFrame();

	// Material Output is a derived terminal, but manual node movement must not drag
	// the terminal along with the node while editing the authored presentation.
	IO.AddMousePosEvent(NodeHeaderPoint.x, NodeHeaderPoint.y);
	DrawFrame();
	IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	DrawFrame();
	IO.AddMousePosEvent(NodeHeaderPoint.x + 40.0f, NodeHeaderPoint.y);
	DrawFrame();
	IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	DrawFrame();
	EXPECT_EQ(Position().X, Before.X + 40);

	constexpr ImVec2 SurfaceBaseColorPin{496.0f, 164.0f};
	constexpr ImVec2 NodeOutputPin{440.0f, 254.0f};
	IO.AddMousePosEvent(SurfaceBaseColorPin.x, SurfaceBaseColorPin.y);
	DrawFrame();
	IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	DrawFrame();
	EXPECT_EQ(Canvas.GetSelectedSurfaceOutput(), EMaterialSurfaceOutput::BaseColor);
	IO.AddMousePosEvent(NodeOutputPin.x, NodeOutputPin.y);
	DrawFrame();
	IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	DrawFrame();
	EXPECT_EQ(Material->GetMaterialProgram()->Outputs.BaseColor.SourceNodeId, NodeId);

	Transactions.Clear();
	constexpr ImVec2 MaterialOutputHeaderPoint{620.0f, 138.0f};
	IO.AddMousePosEvent(MaterialOutputHeaderPoint.x, MaterialOutputHeaderPoint.y);
	DrawFrame();
	IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	DrawFrame();
	IO.AddMousePosEvent(
		MaterialOutputHeaderPoint.x + 60.0f,
		MaterialOutputHeaderPoint.y + 30.0f);
	DrawFrame();
	IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	DrawFrame();
	EXPECT_TRUE(Material->GetMaterialGraphPresentation().bHasMaterialOutputPosition);
	EXPECT_TRUE(Transactions.CanUndo());
	const FMaterialGraphPresentation MovedOutput =
		Material->GetMaterialGraphPresentation();
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_FALSE(Material->GetMaterialGraphPresentation().bHasMaterialOutputPosition);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), MovedOutput);

	Transactions.Clear();
	ImGui::DestroyContext(Context);
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, CommandsAreAtomicAndTransactionsRestoreSemanticAndPresentationState)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "GraphOperationsMaterial");
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
		FMaterialGraphOperations::CreateNode(*Material, Create, &Transactions);
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
		FMaterialGraphOperations::MoveNodes(*Material, std::span(&Moved, 1), &Transactions);
	ASSERT_TRUE(Move) << Move.Message;
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision,
		SemanticRevision);
	const FMaterialGraphView MovedGraph = FMaterialGraphOperations::Inspect(*Material);
	const FMaterialGraphNodeView* MovedView = FindViewNode(MovedGraph, CreatedId);
	ASSERT_NE(MovedView, nullptr);
	ASSERT_TRUE(MovedView->Presentation.has_value());
	EXPECT_EQ(MovedView->Presentation->X, 320);

	FMaterialProgram BeforeRejected = *Material->GetMaterialProgram();
	const auto CreatedNodeIt = std::ranges::find(
		BeforeRejected.Nodes, CreatedId, &FMaterialProgramNode::Id);
	ASSERT_NE(CreatedNodeIt, BeforeRejected.Nodes.end());
	FMaterialProgramNode Invalid = *CreatedNodeIt;
	Invalid.Literal.X = std::numeric_limits<float>::quiet_NaN();
	const FMaterialGraphCommandResult Rejected =
		FMaterialGraphOperations::ReplaceNode(*Material, std::move(Invalid), &Transactions);
	EXPECT_EQ(Rejected.Status, EMaterialGraphCommandStatus::Rejected);
	EXPECT_FALSE(Rejected.Diagnostics.empty());
	EXPECT_EQ(*Material->GetMaterialProgram(), BeforeRejected);
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision,
		SemanticRevision);

	ASSERT_TRUE(Transactions.Undo());
	const FMaterialGraphView UnmovedGraph = FMaterialGraphOperations::Inspect(*Material);
	const FMaterialGraphNodeView* UnmovedView = FindViewNode(UnmovedGraph, CreatedId);
	ASSERT_NE(UnmovedView, nullptr);
	ASSERT_TRUE(UnmovedView->Presentation.has_value());
	EXPECT_EQ(UnmovedView->Presentation->X, 120);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(*Material->GetMaterialProgram(), OriginalProgram);
	ASSERT_TRUE(Transactions.Redo());
	const FMaterialGraphView RecreatedGraph = FMaterialGraphOperations::Inspect(*Material);
	EXPECT_NE(FindViewNode(RecreatedGraph, CreatedId), nullptr);
	ASSERT_TRUE(Transactions.Redo());
	const FMaterialGraphView RemovedGraph = FMaterialGraphOperations::Inspect(*Material);
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
	const FMaterialGraphView CoalescedUndoGraph = FMaterialGraphOperations::Inspect(*Material);
	const FMaterialGraphNodeView* CoalescedUndo = FindViewNode(
		CoalescedUndoGraph, CreatedId);
	ASSERT_NE(CoalescedUndo, nullptr);
	ASSERT_TRUE(CoalescedUndo->Presentation.has_value());
	EXPECT_EQ(CoalescedUndo->Presentation->X, 320);

	Transactions.Clear();
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests,
	MaterialOutputPromotionTextureAndDisconnectAreAtomic)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "MaterialOutputCommands");
	ASSERT_NE(Material, nullptr);
	ASSERT_TRUE(Material->GetMaterialProgram()->Nodes.empty());
	FTransactionManager Transactions;

	FMaterialProgramLiteral EditedBaseColor{0.2f, 0.3f, 0.4f, 0.0f};
	ASSERT_TRUE(FMaterialGraphOperations::SetSurfaceDefault(*Material, {
		.Output = EMaterialSurfaceOutput::BaseColor,
		.Value = EditedBaseColor}, &Transactions));
	const FMaterialGraphCommandResult Promoted =
		FMaterialGraphOperations::PromoteSurfaceOutputToParameter(*Material, {
			.Output = EMaterialSurfaceOutput::BaseColor,
			.X = 100,
			.Y = 200}, &Transactions);
	ASSERT_TRUE(Promoted) << Promoted.Message;
	ASSERT_EQ(Promoted.GeneratedNodeIds.size(), 1u);
	EXPECT_EQ(Material->GetMaterialProgram()->Nodes.size(), 1u);
	EXPECT_TRUE(Material->GetMaterialProgram()->Outputs.BaseColor.SourceNodeId.IsValid());
	FVector3 BaseColor;
	ASSERT_TRUE(Material->GetVectorParameterValue(
		MaterialParameters::BaseColorName(), BaseColor));
	EXPECT_NEAR(BaseColor.x, 0.2, 1.e-6);
	EXPECT_NEAR(BaseColor.y, 0.3, 1.e-6);
	EXPECT_NEAR(BaseColor.z, 0.4, 1.e-6);
	const std::vector PromotedDependencies = InspectMaterialParameterDependencies(
		*Material->GetMaterialProgram(), Material->GetParameterDefinitions());
	ASSERT_EQ(PromotedDependencies.size(), 1u);
	EXPECT_EQ(PromotedDependencies.front().ParameterId,
		MaterialParameters::GetBuiltinParameterIds(MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value);
	const uint64 CompileGeneration =
		Material->GetMaterialCompileStatus().RequestGeneration;
	ASSERT_TRUE(FMaterialGraphOperations::SetParameterValue(
		*Material, MaterialParameters::GetBuiltinParameterIds(MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value,
		FMaterialParameterValue::MakeVector({0.7, 0.6, 0.5}),
		&Transactions));
	EXPECT_EQ(Material->GetMaterialCompileStatus().RequestGeneration,
		CompileGeneration);
	ASSERT_TRUE(Material->GetVectorParameterValue(
		MaterialParameters::BaseColorName(), BaseColor));
	EXPECT_EQ(BaseColor, FVector3(0.7, 0.6, 0.5));
	ASSERT_TRUE(Transactions.Undo());
	ASSERT_TRUE(Material->GetVectorParameterValue(
		MaterialParameters::BaseColorName(), BaseColor));
	EXPECT_NEAR(BaseColor.x, 0.2, 1.e-6);
	EXPECT_NEAR(BaseColor.y, 0.3, 1.e-6);
	EXPECT_NEAR(BaseColor.z, 0.4, 1.e-6);
	ASSERT_TRUE(Transactions.Redo());
	ASSERT_TRUE(Transactions.Undo());

	ASSERT_TRUE(Transactions.Undo());
	EXPECT_TRUE(Material->GetMaterialProgram()->Nodes.empty());
	ASSERT_TRUE(Material->GetVectorParameterValue(
		MaterialParameters::BaseColorName(), BaseColor));
	EXPECT_EQ(BaseColor, FVector3(0.5));
	ASSERT_TRUE(Transactions.Redo());
	ASSERT_TRUE(FMaterialGraphOperations::DisconnectSurfaceOutput(
		*Material, EMaterialSurfaceOutput::BaseColor, &Transactions));
	EXPECT_FALSE(Material->GetMaterialProgram()->Outputs.BaseColor.SourceNodeId.IsValid());
	EXPECT_EQ(Material->GetMaterialProgram()->Outputs.BaseColorDefault,
		EditedBaseColor);
	EXPECT_TRUE(InspectMaterialParameterDependencies(
		*Material->GetMaterialProgram(),
		Material->GetParameterDefinitions()).empty());

	const FMaterialGraphCommandResult Textured =
		FMaterialGraphOperations::AddTextureToSurfaceOutput(*Material, {
			.Output = EMaterialSurfaceOutput::Normal,
			.X = 400,
			.Y = 200}, &Transactions);
	ASSERT_TRUE(Textured) << Textured.Message;
	ASSERT_EQ(Textured.GeneratedNodeIds.size(), 5u);
	const std::vector TextureDependencies = InspectMaterialParameterDependencies(
		*Material->GetMaterialProgram(), Material->GetParameterDefinitions());
	ASSERT_EQ(TextureDependencies.size(), 6u);
	EXPECT_EQ(TextureDependencies.front().ParameterId,
		MaterialParameters::GetBuiltinParameterIds(MaterialParameters::EMaterialBuiltinParameterRole::Normal).Texture);
	EXPECT_EQ(TextureDependencies.back().ParameterId,
		MaterialParameters::GetBuiltinParameterIds(MaterialParameters::EMaterialBuiltinParameterRole::Normal).SamplerState);
	const FMaterialNormalizationResult Normalized = Normalize(*Material);
	ASSERT_TRUE(Normalized);
	EXPECT_EQ(Normalized.IR.Nodes.size(), 12u);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_FALSE(Material->GetMaterialProgram()->Outputs.Normal.SourceNodeId.IsValid());

	Transactions.Clear();
	MarkAsGarbage(Material);
	CollectGarbage();
}
