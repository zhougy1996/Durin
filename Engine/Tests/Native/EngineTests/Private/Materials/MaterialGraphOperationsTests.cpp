#include "Misc/MountPathTestSupport.h"
#include "MaterialGraphOperations.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "MaterialAssetCreation.h"
#include "MaterialDocumentSnapshot.h"
#include "Graph/MaterialGraphCanvas.h"

#include "MaterialTestSupport.h"

#include "Asset/AssetCompilingManager.h"
#include "Asset/Asset.h"
#include "AssetRegistry/Scan.h"
#include "DObject/DefaultObjectGraph.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Editor/Transaction.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialProgramCompiler.h"

#include <gtest/gtest.h>

#include "NativeDObjectTestSupport.h"

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
			MakeCanonicalMaterialProgram(), Validation)
			|| !FMaterialGraphOperations::Layout(*Material)) return nullptr;
		return Material;
	}
}

TEST(FMaterialGraphOperationsTests,
	GraphViewRevisionsTrackRelevantAuthoredState)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "GraphViewRevisions");
	ASSERT_NE(Material, nullptr);
	const uint64 InitialProgramRevision = Material->GetMaterialProgramRevision();
	const uint64 InitialPresentationRevision =
		Material->GetMaterialGraphPresentationRevision();
	const uint64 InitialSchemaRevision =
		Material->GetParameterDefinitionSchemaRevision();

	FMaterialGraphPresentation Presentation =
		Material->GetMaterialGraphPresentation();
	Presentation.MaterialOutputX += 64;
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(Presentation));
	EXPECT_EQ(Material->GetMaterialProgramRevision(), InitialProgramRevision);
	EXPECT_GT(Material->GetMaterialGraphPresentationRevision(),
		InitialPresentationRevision);
	EXPECT_EQ(Material->GetParameterDefinitionSchemaRevision(),
		InitialSchemaRevision);
	const uint64 MovedPresentationRevision =
		Material->GetMaterialGraphPresentationRevision();
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(Presentation));
	EXPECT_EQ(Material->GetMaterialGraphPresentationRevision(),
		MovedPresentationRevision);

	FMaterialProgram Program = *Material->GetMaterialProgram();
	Program.Outputs.RoughnessDefault.X = 0.75f;
	FMaterialProgramValidationResult Validation;
	ASSERT_TRUE(Material->SetMaterialProgram(std::move(Program), Validation));
	EXPECT_GT(Material->GetMaterialProgramRevision(), InitialProgramRevision);
	EXPECT_EQ(Material->GetMaterialGraphPresentationRevision(),
		MovedPresentationRevision);
	EXPECT_EQ(Material->GetParameterDefinitionSchemaRevision(),
		InitialSchemaRevision);

	ASSERT_TRUE(Material->SetVectorParameterValue(
		MaterialParameters::BaseColorName(), {0.2f, 0.3f, 0.4f}));
	EXPECT_EQ(Material->GetParameterDefinitionSchemaRevision(),
		InitialSchemaRevision);
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialAssetCreationTests, NewBaseMaterialIsRenderableBeforePublication)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Material = Durin::NewObject<Durin::DMaterial>(
		nullptr, "NewBaseMaterial");
	ASSERT_NE(Material, nullptr);

	std::string Error;
	ASSERT_TRUE(Durin::PrepareNewMaterialForEditing(*Material, Error)) << Error;
	EXPECT_EQ(Material->GetMaterialCompileStatus().State,
		Durin::EMaterialCompileState::Ready);
	EXPECT_TRUE(Material->GetAcceptedCompiledProgram());
	EXPECT_TRUE(Material->GetMaterialProgram()->Nodes.empty());
	EXPECT_TRUE(Material->GetMaterialGraphPresentation().bHasMaterialOutputPosition);
	EXPECT_EQ(Material->GetMaterialProgram()->Outputs.BaseColorDefault,
		(Durin::FMaterialProgramLiteral{0.5f, 0.5f, 0.5f, 0.0f}));

	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialAssetCreationTests, BuiltInMaterialsHaveCompletePersistentGraphPresentation)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(FMountPaths::InitDefaultMountPoints());
	const Asset::FAssetCatalogRefreshResult Refresh =
		Asset::RefreshAssetRegistry(Asset::EAssetRegistryScanMode::FullValidation);
	ASSERT_TRUE(Refresh) << (Refresh.Errors.empty()
		? "Asset catalog refresh failed without a diagnostic."
		: Refresh.Errors.front().Message);

	for (const std::string_view PathString : {
		"/Engine/Materials/DefaultMaterial",
		"/Engine/Materials/ImportedSurface"})
	{
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate(PathString, Path));
		DMaterial* Material = nullptr;
		const Asset::FAssetResult Loaded = Asset::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Material);
		ASSERT_TRUE(Loaded) << Loaded.Message;
		ASSERT_NE(Material, nullptr);
		const FMaterialGraphPresentation& Presentation =
			Material->GetMaterialGraphPresentation();
		EXPECT_EQ(Presentation.Nodes.size(),
			Material->GetMaterialProgram()->Nodes.size());
		EXPECT_TRUE(Presentation.bHasMaterialOutputPosition);
		const FMaterialGraphView View = FMaterialGraphOperations::Inspect(*Material);
		EXPECT_EQ(View.Nodes.size(), Material->GetMaterialProgram()->Nodes.size());
		ASSERT_TRUE(Asset::UnloadPackage(Path));
	}
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests,
	StandardSurfaceCatalogAndAggregateCommandsAreAtomic)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "AggregateSurfaceCommands");
	ASSERT_NE(Material, nullptr);
	const auto Catalog = FMaterialGraphOperations::EnumerateCatalog(*Material);
	const auto Entry = std::ranges::find(Catalog,
		EMaterialProgramOpcode::StandardSurface,
		[](const FMaterialGraphCatalogEntry& Value) {
			return Value.NodeTemplate.Opcode;
		});
	ASSERT_NE(Entry, Catalog.end());
	EXPECT_EQ(Entry->NodeTemplate.ResultType, EMaterialProgramValueType::Surface);
	FMaterialGraphCreateNodeRequest Request{.Node = Entry->NodeTemplate, .X = 100, .Y = 100};
	Request.Node.Id = {0x57face01, 1, 2, 3};
	ASSERT_TRUE(FMaterialGraphOperations::CreateNode(*Material, Request));
	ASSERT_TRUE(FMaterialGraphOperations::AssignAggregateSurface(
		*Material, Request.Node.Id));
	EXPECT_EQ(Material->GetMaterialProgram()->Outputs.Surface.SourceNodeId,
		Request.Node.Id);
	EXPECT_FALSE(Material->GetMaterialProgram()->Outputs.BaseColor.SourceNodeId.IsValid());
	const auto Normalized = Normalize(*Material);
	ASSERT_TRUE(Normalized);
	EXPECT_TRUE(Normalized.IR.SurfaceRoot.bAggregate);
	EXPECT_EQ(Normalized.IR.Nodes.size(), 1u);
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(FMaterialGraphOperations::CopySelection(
		*Material, std::array{Request.Node.Id}, Payload));
	EXPECT_TRUE(Payload.bConnectAggregateSurface);
	ASSERT_TRUE(FMaterialGraphOperations::Paste(*Material, Payload, 300, 100));
	EXPECT_NE(Material->GetMaterialProgram()->Outputs.Surface.SourceNodeId,
		Request.Node.Id);
	ASSERT_TRUE(FMaterialGraphOperations::DisconnectAggregateSurface(*Material));
	EXPECT_FALSE(Material->GetMaterialProgram()->Outputs.Surface.SourceNodeId.IsValid());
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialDocumentSnapshotTests,
	DiscardRestoresAuthoredAndRenderableBaseMaterialState)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "DiscardMaterialState");
	ASSERT_NE(Material, nullptr);
	std::string Error;
	ASSERT_TRUE(PrepareNewMaterialForEditing(*Material, Error)) << Error;
	const FMaterialProgram OriginalProgram = *Material->GetMaterialProgram();
	const FMaterialGraphPresentation OriginalPresentation =
		Material->GetMaterialGraphPresentation();
	const std::shared_ptr<const FMaterialCompilerResult> OriginalCompiled =
		Material->GetAcceptedCompiledProgram();
	ASSERT_TRUE(OriginalCompiled);
	FVector3 OriginalBaseColor;
	ASSERT_TRUE(Material->GetVectorParameterValue(
		MaterialParameters::BaseColorName(), OriginalBaseColor));

	FMaterialDocumentSnapshot Snapshot;
	ASSERT_TRUE(Snapshot.Capture(*Material, Error)) << Error;
	ASSERT_TRUE(FMaterialGraphOperations::SetSurfaceDefault(*Material, {
		.Output = EMaterialSurfaceOutput::BaseColor,
		.Value = {0.1f, 0.2f, 0.3f, 0.0f}}));
	ASSERT_TRUE(Material->SetMaterialGraphPresentation({
		.bHasMaterialOutputPosition = true,
		.MaterialOutputX = 700,
		.MaterialOutputY = 300}));
	ASSERT_TRUE(Material->SetVectorParameterValue(
		MaterialParameters::BaseColorName(), {0.8, 0.7, 0.6}));
	(void)FAssetCompilingManager::Get().FinishCompilationForObject(*Material);
	ASSERT_NE(*Material->GetMaterialProgram(), OriginalProgram);

	ASSERT_TRUE(Snapshot.Restore(*Material, Error)) << Error;
	EXPECT_EQ(*Material->GetMaterialProgram(), OriginalProgram);
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), OriginalPresentation);
	FVector3 RestoredBaseColor;
	ASSERT_TRUE(Material->GetVectorParameterValue(
		MaterialParameters::BaseColorName(), RestoredBaseColor));
	EXPECT_EQ(RestoredBaseColor, OriginalBaseColor);
	ASSERT_TRUE(Material->GetAcceptedCompiledProgram());
	EXPECT_EQ(Material->GetAcceptedCompiledProgram()->Identity,
		OriginalCompiled->Identity);

	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialDocumentSnapshotTests, DiscardRestoresMaterialInstanceState)
{
	InitializeDObjectSystem();
	DMaterial* Parent = MakeExpandedGraphMaterial("DiscardInstanceParent");
	DMaterialInstance* Instance = NewObject<DMaterialInstance>(
		nullptr, "DiscardMaterialInstanceState");
	ASSERT_NE(Parent, nullptr);
	ASSERT_NE(Instance, nullptr);
	ASSERT_TRUE(Instance->SetParent(Parent));
	const FGuid BaseColorId = MaterialParameters::GetBuiltinParameterIds(
		MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value;
	ASSERT_TRUE(Instance->SetParameterOverride(BaseColorId,
		EMaterialParameterType::Vector,
		FMaterialParameterValue::MakeVector({0.2, 0.3, 0.4})));
	const std::vector<FMaterialParameterOverride> OriginalOverrides(
		Instance->GetParameterOverrides().begin(),
		Instance->GetParameterOverrides().end());

	FMaterialDocumentSnapshot Snapshot;
	std::string Error;
	ASSERT_TRUE(Snapshot.Capture(*Instance, Error)) << Error;
	ASSERT_TRUE(Instance->ClearParameterOverride(BaseColorId));
	ASSERT_TRUE(Instance->SetParent(nullptr));
	ASSERT_TRUE(Instance->SetStaticPropertiesOverride({.bTwoSided = true}));

	ASSERT_TRUE(Snapshot.Restore(*Instance, Error)) << Error;
	EXPECT_EQ(Instance->GetParent(), Parent);
	EXPECT_FALSE(Instance->HasStaticPropertiesOverride());
	ASSERT_EQ(Instance->GetParameterOverrides().size(), OriginalOverrides.size());
	ASSERT_EQ(Instance->GetParameterOverrides().size(), 1u);
	EXPECT_EQ(Instance->GetParameterOverrides().front().ParameterId,
		OriginalOverrides.front().ParameterId);
	EXPECT_EQ(Instance->GetParameterOverrides().front().Type,
		OriginalOverrides.front().Type);
	EXPECT_EQ(Instance->GetParameterOverrides().front().Value,
		OriginalOverrides.front().Value);

	MarkAsGarbage(Instance);
	MarkAsGarbage(Parent);
	CollectGarbage();
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
	DMaterial* Material = MakeExpandedGraphMaterial("MaterialOutputMovement");
	ASSERT_NE(Material, nullptr);
	const uint64 Revision = Material->GetMaterialCompileStatus().AuthoredRevision;
	Durin::Tests::FTestTransactorOwner Transactions;
	const FMaterialGraphPresentation OriginalPresentation =
		Material->GetMaterialGraphPresentation();

	ASSERT_TRUE(FMaterialGraphOperations::MoveMaterialOutput(
		*Material, 520, -80, Transactions.Get()));
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision, Revision);
	EXPECT_TRUE(Material->GetMaterialGraphPresentation().bHasMaterialOutputPosition);
	EXPECT_EQ(Material->GetMaterialGraphPresentation().MaterialOutputX, 520);
	EXPECT_EQ(Material->GetMaterialGraphPresentation().MaterialOutputY, -80);
	FMaterialGraphPresentation UnrelatedPresentation =
		Material->GetMaterialGraphPresentation();
	ASSERT_FALSE(UnrelatedPresentation.Nodes.empty());
	UnrelatedPresentation.Nodes.front().X += 37;
	const FMaterialGraphNodePresentation UnrelatedPosition =
		UnrelatedPresentation.Nodes.front();
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(UnrelatedPresentation));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->GetMaterialGraphPresentation().MaterialOutputX,
		OriginalPresentation.MaterialOutputX);
	const auto PreservedNode = std::ranges::find(
		Material->GetMaterialGraphPresentation().Nodes,
		UnrelatedPosition.NodeId, &FMaterialGraphNodePresentation::NodeId);
	ASSERT_NE(PreservedNode, Material->GetMaterialGraphPresentation().Nodes.end());
	EXPECT_EQ(*PreservedNode, UnrelatedPosition);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Material->GetMaterialGraphPresentation().MaterialOutputX, 520);

	Transactions->Reset();
	FMaterialGraphMoveSession Move;
	ASSERT_TRUE(Move.BeginMaterialOutput(*Material, Transactions.Get()));
	ASSERT_TRUE(Move.ApplyMaterialOutput(600, 40));
	ASSERT_TRUE(Move.Cancel());
	EXPECT_EQ(Material->GetMaterialGraphPresentation().MaterialOutputX, 520);
	EXPECT_FALSE(Transactions->CanUndo());
	ASSERT_TRUE(Move.BeginMaterialOutput(*Material, Transactions.Get()));
	ASSERT_TRUE(Move.ApplyMaterialOutput(600, 40));
	ASSERT_TRUE(Move.Commit());
	EXPECT_EQ(Material->GetMaterialGraphPresentation().MaterialOutputX, 600);
	EXPECT_TRUE(Transactions->CanUndo());

	Transactions->Reset();
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
		EXPECT_FALSE(Entry.NormalizedSearchFields.front().empty());
		EXPECT_EQ(Entry.InputNames.size(), Entry.AcceptedInputTypes.size());
	}
	const std::vector<FMaterialGraphCatalogEntry> MultiplyResults =
		FMaterialGraphOperations::SearchCatalog(*Material, "multiply");
	const std::vector<FMaterialGraphCatalogEntry> CachedMultiplyResults =
		FMaterialGraphOperations::SearchCatalog(Catalog, "multiply");
	ASSERT_FALSE(MultiplyResults.empty());
	ASSERT_EQ(CachedMultiplyResults.size(), MultiplyResults.size());
	EXPECT_EQ(MultiplyResults.front().OperationName, "Multiply");
	EXPECT_EQ(CachedMultiplyResults.front().OperationName,
		MultiplyResults.front().OperationName);
	const std::vector<size_t> MultiplyIndices =
		FMaterialGraphOperations::SearchCatalogIndices(Catalog, "MuLtIpLy");
	ASSERT_EQ(MultiplyIndices.size(), MultiplyResults.size());
	EXPECT_EQ(Catalog[MultiplyIndices.front()].OperationName,
		MultiplyResults.front().OperationName);
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
	EXPECT_FLOAT_EQ(FMaterialGraphGeometry::GetSurfacePinOffset(0),
		Metrics.SurfaceHeaderHeight + Metrics.PinRowHeight * 0.5f);
	EXPECT_FLOAT_EQ(FMaterialGraphGeometry::GetSurfacePinOffset(7),
		Metrics.SurfaceHeaderHeight + Metrics.PinRowHeight * 7.5f);

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
	Durin::Tests::FTestTransactorOwner Transactions;
	const FMaterialGraphCommandResult Created =
		FMaterialGraphOperations::CreateNodeWithDefaultInputs(*Material, {
			.Node = Multiply->NodeTemplate,
			.X = 400,
			.Y = 200,
		}, Multiply->AcceptedInputTypes, Transactions.Get());
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
		EXPECT_EQ(Default->Presentation.NodeId, Default->Node.Id);
	}
	const float DefaultHeight = FMaterialGraphGeometry::GetNodeHeight(0);
	const float DefaultGap = FMaterialGraphGeometry::GetMetrics().RowGap;
	std::vector<int32> DefaultRows;
	for (const FMaterialProgramLink& Link : Node->Node.Inputs)
	{
		const FMaterialGraphNodeView* Default = FindViewNode(View, Link.SourceNodeId);
		ASSERT_NE(Default, nullptr);
		DefaultRows.push_back(Default->Presentation.Y);
	}
	std::ranges::sort(DefaultRows);
	for (size_t Index = 1; Index < DefaultRows.size(); ++Index)
		EXPECT_GE(DefaultRows[Index] - DefaultRows[Index - 1],
			static_cast<int32>(DefaultHeight + DefaultGap));
	const FMaterialGraphNodeView* IdentityDefault =
		FindViewNode(View, Node->Node.Inputs[1].SourceNodeId);
	ASSERT_NE(IdentityDefault, nullptr);
	EXPECT_FLOAT_EQ(IdentityDefault->Node.Literal.X, 1.0f);
	ASSERT_TRUE(Transactions->Undo());
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
			const auto& PositionA = LayoutView.Nodes[A].Presentation;
			const auto& PositionB = LayoutView.Nodes[B].Presentation;
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

TEST(FMaterialGraphOperationsTests,
	NewMaterialInitializationPersistsACompleteGraphPresentation)
{
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture MountRegistry;
	const std::filesystem::path Root = std::filesystem::temp_directory_path()
		/ "DurinInitializedGraphLayoutMaterial";
	Testing::RegisterMountPointForTests(
		"/MaterialGraphTests/", Root.generic_string() + "/");
	FPackagePath PackagePath;
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/MaterialGraphTests/InitializedGraphLayoutMaterial", PackagePath));
	DPackage* Package = CreatePackage(PackagePath);
	ASSERT_NE(Package, nullptr);
	DMaterial* Material = NewObject<DMaterial>(
		Package, "InitializedGraphLayoutMaterial");
	ASSERT_NE(Material, nullptr);
	FMaterialProgramValidationResult Validation;
	ASSERT_TRUE(Material->SetMaterialProgram(
		MakeCanonicalMaterialProgram(), Validation));
	std::string Error;
	ASSERT_TRUE(PrepareNewMaterialForEditing(*Material, Error)) << Error;
	const FMaterialGraphPresentation& Presentation =
		Material->GetMaterialGraphPresentation();
	EXPECT_EQ(Presentation.Nodes.size(),
		Material->GetMaterialProgram()->Nodes.size());
	EXPECT_TRUE(Presentation.bHasMaterialOutputPosition);
	const FMaterialGraphView View = FMaterialGraphOperations::Inspect(*Material);
	EXPECT_EQ(View.Nodes.size(), Material->GetMaterialProgram()->Nodes.size());
	EXPECT_EQ(View.MaterialOutputPosition,
		(std::pair{Presentation.MaterialOutputX, Presentation.MaterialOutputY}));

	MarkObjectHierarchyAsGarbage(Package);
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
		return Node ? Node->Presentation.Y : 0;
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
	const FMaterialGraphNodePresentation Occupied{
		Fixed, SelectedView->Presentation.X, 0};
	ASSERT_TRUE(FMaterialGraphOperations::MoveNodes(*Material, std::span(&Occupied, 1)));
	ASSERT_TRUE(FMaterialGraphOperations::Layout(*Material, std::span(&Selected, 1)));
	const FMaterialGraphView Relayout = FMaterialGraphOperations::Inspect(*Material);
	const auto* RelayoutSelected = FindViewNode(Relayout, Selected);
	const auto* RelayoutFixed = FindViewNode(Relayout, Fixed);
	ASSERT_NE(RelayoutSelected, nullptr);
	ASSERT_NE(RelayoutFixed, nullptr);
	const float Height = FMaterialGraphGeometry::GetNodeHeight(0);
	const float Width = FMaterialGraphGeometry::GetMetrics().NodeWidth;
	EXPECT_FALSE(RelayoutSelected->Presentation.X < RelayoutFixed->Presentation.X + Width
		&& RelayoutSelected->Presentation.X + Width > RelayoutFixed->Presentation.X
		&& RelayoutSelected->Presentation.Y < RelayoutFixed->Presentation.Y + Height
		&& RelayoutSelected->Presentation.Y + Height > RelayoutFixed->Presentation.Y);

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
	Durin::Tests::FTestTransactorOwner Transactions;
	const FMaterialGraphCommandResult Pasted = FMaterialGraphOperations::Paste(
		*Material, Payload, 1200, 400, Transactions.Get());
	ASSERT_TRUE(Pasted) << Pasted.Message;
	ASSERT_EQ(Pasted.GeneratedNodeIds.size(), Payload.Nodes.size());
	std::unordered_set<FGuid> OriginalIds(AllNodes.begin(), AllNodes.end());
	for (const FGuid& Id : Pasted.GeneratedNodeIds)
		EXPECT_FALSE(OriginalIds.contains(Id));
	const FMaterialNormalizationResult AfterIdentity = Normalize(*Material);
	ASSERT_TRUE(AfterIdentity);
	EXPECT_EQ(AfterIdentity.Identity, BeforeIdentity.Identity);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(*Material->GetMaterialProgram(), BeforeProgram);
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), BeforePresentation);
	ASSERT_TRUE(Transactions->Redo());

	FMaterialGraphClipboardPayload UnknownVersion = Payload;
	UnknownVersion.SchemaVersion = 99;
	const FMaterialProgram BeforeRejected = *Material->GetMaterialProgram();
	const FMaterialGraphCommandResult Rejected = FMaterialGraphOperations::Paste(
		*Material, UnknownVersion, 0, 0, Transactions.Get());
	EXPECT_EQ(Rejected.Status, EMaterialGraphCommandStatus::Rejected);
	EXPECT_EQ(*Material->GetMaterialProgram(), BeforeRejected);

	const auto Dependent = std::ranges::find_if(
		Material->GetMaterialProgram()->Nodes,
		[](const FMaterialProgramNode& Node) { return !Node.Inputs.empty(); });
	ASSERT_NE(Dependent, Material->GetMaterialProgram()->Nodes.end());
	const FGuid RequiredSource = Dependent->Inputs.front().SourceNodeId;
	const FMaterialGraphCommandResult RequiredRemoval =
		FMaterialGraphOperations::RemoveNodes(
			*Material, std::span(&RequiredSource, 1), Transactions.Get());
	EXPECT_EQ(RequiredRemoval.Status, EMaterialGraphCommandStatus::Rejected);
	EXPECT_EQ(*Material->GetMaterialProgram(), BeforeRejected);
	const std::vector<FMaterialProgramLink> ExternalInputs = Dependent->Inputs;
	FMaterialGraphClipboardPayload Partial;
	ASSERT_TRUE(FMaterialGraphOperations::CopySelection(
		*Material, std::span(&Dependent->Id, 1), Partial));
	ASSERT_EQ(Partial.Nodes.size(), 1u);
	EXPECT_EQ(Partial.Nodes.front().Node.Inputs, ExternalInputs);
	const FMaterialGraphCommandResult PartialPaste =
		FMaterialGraphOperations::Paste(*Material, Partial, 0, 0, Transactions.Get());
	ASSERT_TRUE(PartialPaste) << PartialPaste.Message;
	ASSERT_EQ(PartialPaste.GeneratedNodeIds.size(), 1u);
	const auto PastedDependent = std::ranges::find(
		Material->GetMaterialProgram()->Nodes, PartialPaste.GeneratedNodeIds.front(),
		&FMaterialProgramNode::Id);
	ASSERT_NE(PastedDependent, Material->GetMaterialProgram()->Nodes.end());
	EXPECT_EQ(PastedDependent->Inputs, ExternalInputs);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(*Material->GetMaterialProgram(), BeforeRejected);

	FMaterialGraphCreateNodeRequest Standalone;
	Standalone.Node.Opcode = EMaterialProgramOpcode::Constant;
	Standalone.Node.ResultType = EMaterialProgramValueType::Float;
	Standalone.Node.Literal.X = 0.75f;
	Standalone.X = 80;
	Standalone.Y = 120;
	const FMaterialGraphCommandResult StandaloneCreated =
		FMaterialGraphOperations::CreateNode(*Material, Standalone, Transactions.Get());
	ASSERT_TRUE(StandaloneCreated);
	const FGuid StandaloneId = StandaloneCreated.GeneratedNodeIds.front();
	const FMaterialGraphCommandResult Duplicated =
		FMaterialGraphOperations::DuplicateNodes(
			*Material, std::span(&StandaloneId, 1), 40, 40, Transactions.Get());
	ASSERT_TRUE(Duplicated) << Duplicated.Message;
	ASSERT_EQ(Duplicated.GeneratedNodeIds.size(), 1u);
	EXPECT_NE(Duplicated.GeneratedNodeIds.front(), StandaloneId);
	FMaterialGraphClipboardPayload CutPayload;
	const FMaterialGraphCommandResult Cut = FMaterialGraphOperations::CutSelection(
		*Material, std::span(&StandaloneId, 1), CutPayload, Transactions.Get());
	ASSERT_TRUE(Cut) << Cut.Message;
	ASSERT_EQ(CutPayload.Nodes.size(), 1u);
	EXPECT_EQ(CutPayload.Nodes.front().Node.Id, StandaloneId);
	EXPECT_EQ(FindViewNode(FMaterialGraphOperations::Inspect(*Material), StandaloneId),
		nullptr);
	ASSERT_TRUE(Transactions->Undo());
	const FMaterialGraphView RestoredCut = FMaterialGraphOperations::Inspect(*Material);
	EXPECT_NE(FindViewNode(RestoredCut, StandaloneId), nullptr);

	Transactions->Reset();
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
	EXPECT_TRUE(FirstCanvas.SelectAndFrame(FirstNode));
	EXPECT_FALSE(FirstCanvas.GetSelectedSurfaceOutput().has_value());
	EXPECT_FALSE(FirstCanvas.SelectAndFrameDiagnostic({
		.LocationKind = EMaterialProgramDiagnosticLocationKind::Program,
	}));
	EXPECT_FALSE(FirstCanvas.SelectAndFrameDiagnostic({
		.LocationKind = EMaterialProgramDiagnosticLocationKind::SurfaceOutput,
		.LocationIndex = 8,
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
	Durin::Tests::FTestTransactorOwner Transactions;
	FMaterialGraphCanvas Canvas;

	const auto DrawAtZoom = [&](float Zoom) {
		Canvas.SetViewport(Zoom, {40.0f, 40.0f});
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0.0f, 0.0f});
		ImGui::SetNextWindowSize({1200.0f, 720.0f});
		ImGui::Begin("Material Graph Render Test", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		Canvas.Draw(*Material, *Transactions.Get(), 660.0f,
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
	ASSERT_TRUE(FMaterialGraphOperations::Layout(*Material));
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
	ASSERT_TRUE(FMaterialGraphOperations::Layout(*Material));
	const int MaximumVertices = DrawAtZoom(0.30f);
	EXPECT_GT(MaximumVertices, 100);
	EXPECT_LT(MaximumVertices, 100000);

	Transactions->Reset();
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
	Durin::Tests::FTestTransactorOwner Transactions;

	FMaterialGraphCreateNodeRequest Create;
	Create.Node.Opcode = EMaterialProgramOpcode::Constant;
	Create.Node.ResultType = EMaterialProgramValueType::Float;
	Create.Node.Literal.X = 0.25f;
	Create.X = 120;
	Create.Y = -80;
	const FMaterialGraphCommandResult Created =
		FMaterialGraphOperations::CreateNode(*Material, Create, Transactions.Get());
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
		FMaterialGraphOperations::MoveNodes(*Material, std::span(&Moved, 1), Transactions.Get());
	ASSERT_TRUE(Move) << Move.Message;
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision,
		SemanticRevision);
	const FMaterialGraphView MovedGraph = FMaterialGraphOperations::Inspect(*Material);
	const FMaterialGraphNodeView* MovedView = FindViewNode(MovedGraph, CreatedId);
	ASSERT_NE(MovedView, nullptr);
	EXPECT_EQ(MovedView->Presentation.X, 320);

	FMaterialProgram BeforeRejected = *Material->GetMaterialProgram();
	const auto CreatedNodeIt = std::ranges::find(
		BeforeRejected.Nodes, CreatedId, &FMaterialProgramNode::Id);
	ASSERT_NE(CreatedNodeIt, BeforeRejected.Nodes.end());
	FMaterialProgramNode Invalid = *CreatedNodeIt;
	Invalid.Literal.X = std::numeric_limits<float>::quiet_NaN();
	const FMaterialGraphCommandResult Rejected =
		FMaterialGraphOperations::ReplaceNode(*Material, std::move(Invalid), Transactions.Get());
	EXPECT_EQ(Rejected.Status, EMaterialGraphCommandStatus::Rejected);
	EXPECT_FALSE(Rejected.Diagnostics.empty());
	EXPECT_EQ(*Material->GetMaterialProgram(), BeforeRejected);
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision,
		SemanticRevision);

	ASSERT_TRUE(Transactions->Undo());
	const FMaterialGraphView UnmovedGraph = FMaterialGraphOperations::Inspect(*Material);
	const FMaterialGraphNodeView* UnmovedView = FindViewNode(UnmovedGraph, CreatedId);
	ASSERT_NE(UnmovedView, nullptr);
	EXPECT_EQ(UnmovedView->Presentation.X, 120);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(*Material->GetMaterialProgram(), OriginalProgram);
	ASSERT_TRUE(Transactions->Redo());
	const FMaterialGraphView RecreatedGraph = FMaterialGraphOperations::Inspect(*Material);
	EXPECT_NE(FindViewNode(RecreatedGraph, CreatedId), nullptr);
	ASSERT_TRUE(Transactions->Redo());
	const FMaterialGraphView RemovedGraph = FMaterialGraphOperations::Inspect(*Material);
	const FMaterialGraphNodeView* RedoneView = FindViewNode(RemovedGraph, CreatedId);
	ASSERT_NE(RedoneView, nullptr);
	EXPECT_EQ(RedoneView->Presentation.X, 320);

	FMaterialGraphMoveSession MoveSession;
	ASSERT_TRUE(MoveSession.Begin(*Material, std::span(&CreatedId, 1), Transactions.Get()));
	const FMaterialGraphNodePresentation FirstPreview{CreatedId, 400, 200};
	const FMaterialGraphNodePresentation SecondPreview{CreatedId, 480, 240};
	ASSERT_TRUE(MoveSession.Apply(std::span(&FirstPreview, 1)));
	ASSERT_TRUE(MoveSession.Apply(std::span(&SecondPreview, 1)));
	ASSERT_TRUE(MoveSession.Commit());
	ASSERT_TRUE(Transactions->Undo());
	const FMaterialGraphView CoalescedUndoGraph = FMaterialGraphOperations::Inspect(*Material);
	const FMaterialGraphNodeView* CoalescedUndo = FindViewNode(
		CoalescedUndoGraph, CreatedId);
	ASSERT_NE(CoalescedUndo, nullptr);
	EXPECT_EQ(CoalescedUndo->Presentation.X, 320);

	Transactions->Reset();
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests,
	MovePreviewRejectsSemanticChangesAndRestoresOnlyItsPositions)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "StaleGraphMove");
	ASSERT_NE(Material, nullptr);

	FMaterialGraphCreateNodeRequest Create;
	Create.Node.Opcode = EMaterialProgramOpcode::Constant;
	Create.Node.ResultType = EMaterialProgramValueType::Float;
	Create.X = 40;
	Create.Y = 80;
	const FMaterialGraphCommandResult Created =
		FMaterialGraphOperations::CreateNode(*Material, Create);
	ASSERT_TRUE(Created) << Created.Message;
	ASSERT_EQ(Created.GeneratedNodeIds.size(), 1u);
	const FGuid NodeId = Created.GeneratedNodeIds.front();

	FMaterialGraphMoveSession MoveSession;
	ASSERT_TRUE(MoveSession.Begin(*Material, std::span(&NodeId, 1)));
	const FMaterialGraphNodePresentation Preview{NodeId, 400, 240};
	ASSERT_TRUE(MoveSession.Apply(std::span(&Preview, 1)));

	FMaterialProgram SemanticEdit = *Material->GetMaterialProgram();
	SemanticEdit.Outputs.RoughnessDefault.X = 0.37f;
	FMaterialProgramValidationResult Validation;
	ASSERT_TRUE(Material->SetMaterialProgram(std::move(SemanticEdit), Validation));
	const uint64 SemanticRevision =
		Material->GetMaterialCompileStatus().AuthoredRevision;

	const FMaterialGraphNodePresentation StalePreview{NodeId, 640, 360};
	const FMaterialGraphCommandResult Rejected =
		MoveSession.Apply(std::span(&StalePreview, 1));
	EXPECT_EQ(Rejected.Status, EMaterialGraphCommandStatus::Rejected);
	EXPECT_FALSE(MoveSession.IsActive());
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision,
		SemanticRevision);
	EXPECT_FLOAT_EQ(Material->GetMaterialProgram()->Outputs.RoughnessDefault.X,
		0.37f);
	const FMaterialGraphView RestoredView =
		FMaterialGraphOperations::Inspect(*Material);
	const FMaterialGraphNodeView* Restored = FindViewNode(RestoredView, NodeId);
	ASSERT_NE(Restored, nullptr);
	EXPECT_EQ(Restored->Presentation.X, 40);
	EXPECT_EQ(Restored->Presentation.Y, 80);

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
	Durin::Tests::FTestTransactorOwner Transactions;

	FMaterialProgramLiteral EditedBaseColor{0.2f, 0.3f, 0.4f, 0.0f};
	ASSERT_TRUE(FMaterialGraphOperations::SetSurfaceDefault(*Material, {
		.Output = EMaterialSurfaceOutput::BaseColor,
		.Value = EditedBaseColor}, Transactions.Get()));
	const FMaterialGraphCommandResult Promoted =
		FMaterialGraphOperations::PromoteSurfaceOutputToParameter(*Material, {
			.Output = EMaterialSurfaceOutput::BaseColor,
			.X = 100,
			.Y = 200}, Transactions.Get());
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
		Transactions.Get()));
	EXPECT_EQ(Material->GetMaterialCompileStatus().RequestGeneration,
		CompileGeneration);
	ASSERT_TRUE(Material->GetVectorParameterValue(
		MaterialParameters::BaseColorName(), BaseColor));
	EXPECT_EQ(BaseColor, FVector3(0.7, 0.6, 0.5));
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Material->GetVectorParameterValue(
		MaterialParameters::BaseColorName(), BaseColor));
	EXPECT_NEAR(BaseColor.x, 0.2, 1.e-6);
	EXPECT_NEAR(BaseColor.y, 0.3, 1.e-6);
	EXPECT_NEAR(BaseColor.z, 0.4, 1.e-6);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Transactions->Undo());

	ASSERT_TRUE(Transactions->Undo());
	EXPECT_TRUE(Material->GetMaterialProgram()->Nodes.empty());
	ASSERT_TRUE(Material->GetVectorParameterValue(
		MaterialParameters::BaseColorName(), BaseColor));
	EXPECT_EQ(BaseColor, FVector3(0.5));
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(FMaterialGraphOperations::DisconnectSurfaceOutput(
		*Material, EMaterialSurfaceOutput::BaseColor, Transactions.Get()));
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
			.Y = 200}, Transactions.Get());
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
	EXPECT_EQ(Normalized.IR.Nodes.size(), 5u);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FALSE(Material->GetMaterialProgram()->Outputs.Normal.SourceNodeId.IsValid());

	Transactions->Reset();
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests,
	ParameterEditSessionPublishesEveryPreviewWithoutCompiling)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "InteractiveParameterMaterial");
	ASSERT_NE(Material, nullptr);
	ASSERT_TRUE(FMaterialGraphOperations::SetSurfaceDefault(*Material, {
		.Output = EMaterialSurfaceOutput::BaseColor,
		.Value = {0.2f, 0.3f, 0.4f, 0.0f}}));
	ASSERT_TRUE(FMaterialGraphOperations::PromoteSurfaceOutputToParameter(
		*Material, {.Output = EMaterialSurfaceOutput::BaseColor}));

	const FGuid ParameterId = MaterialParameters::GetBuiltinParameterIds(
		MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value;
	const uint64 CompileGeneration =
		Material->GetMaterialCompileStatus().RequestGeneration;
	FVector3 OriginalBaseColor;
	ASSERT_TRUE(Material->GetVectorParameterValue(
		MaterialParameters::BaseColorName(), OriginalBaseColor));
	Durin::Tests::FTestTransactorOwner Transactions;
	FMaterialGraphParameterEditSession Session;
	ASSERT_TRUE(Session.Begin(*Material, ParameterId, Transactions.Get()));
	ASSERT_TRUE(Session.Apply(FMaterialParameterValue::MakeVector(
		{0.4, 0.5, 0.6})));
	FVector3 BaseColor;
	ASSERT_TRUE(Material->GetVectorParameterValue(
		MaterialParameters::BaseColorName(), BaseColor));
	EXPECT_EQ(BaseColor, FVector3(0.4, 0.5, 0.6));
	EXPECT_EQ(Material->GetMaterialCompileStatus().RequestGeneration,
		CompileGeneration);
	ASSERT_TRUE(Session.Apply(FMaterialParameterValue::MakeVector(
		{0.7, 0.8, 0.9})));
	ASSERT_TRUE(Material->GetVectorParameterValue(
		MaterialParameters::BaseColorName(), BaseColor));
	EXPECT_EQ(BaseColor, FVector3(0.7, 0.8, 0.9));
	EXPECT_EQ(Material->GetMaterialCompileStatus().RequestGeneration,
		CompileGeneration);
	ASSERT_TRUE(Session.Commit());
	ASSERT_TRUE(FMaterialGraphOperations::MoveMaterialOutput(
		*Material, 713, -91));

	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Material->GetVectorParameterValue(
		MaterialParameters::BaseColorName(), BaseColor));
	EXPECT_EQ(BaseColor, OriginalBaseColor);
	EXPECT_EQ(Material->GetMaterialGraphPresentation().MaterialOutputX, 713);
	EXPECT_EQ(Material->GetMaterialGraphPresentation().MaterialOutputY, -91);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Material->GetVectorParameterValue(
		MaterialParameters::BaseColorName(), BaseColor));
	EXPECT_EQ(BaseColor, FVector3(0.7, 0.8, 0.9));

	ASSERT_TRUE(Session.Begin(*Material, ParameterId, Transactions.Get()));
	ASSERT_TRUE(Session.Apply(FMaterialParameterValue::MakeVector(
		{0.1, 0.1, 0.1})));
	ASSERT_TRUE(Session.Cancel());
	ASSERT_TRUE(Material->GetVectorParameterValue(
		MaterialParameters::BaseColorName(), BaseColor));
	EXPECT_EQ(BaseColor, FVector3(0.7, 0.8, 0.9));
	EXPECT_EQ(Material->GetMaterialCompileStatus().RequestGeneration,
		CompileGeneration);

	Transactions->Reset();
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests,
	CompactTransactionAccountingExcludesUnchangedProgramSnapshots)
{
	InitializeDObjectSystem();
	DMaterial* Material = MakeExpandedGraphMaterial("CompactGraphTransactions");
	ASSERT_NE(Material, nullptr);
	ASSERT_FALSE(Material->GetMaterialProgram()->Nodes.empty());
	const FGuid NodeId = Material->GetMaterialProgram()->Nodes.front().Id;
	const auto OriginalPosition = std::ranges::find(
		Material->GetMaterialGraphPresentation().Nodes, NodeId,
		&FMaterialGraphNodePresentation::NodeId);
	ASSERT_NE(OriginalPosition,
		Material->GetMaterialGraphPresentation().Nodes.end());
	Durin::Tests::FTestTransactorOwner Transactions;

	const FMaterialGraphNodePresentation Moved{
		NodeId, OriginalPosition->X + 17, OriginalPosition->Y + 29};
	ASSERT_TRUE(FMaterialGraphOperations::MoveNodes(
		*Material, std::span(&Moved, 1), Transactions.Get()));
	const size_t PresentationTransactionBytes = Transactions->GetOwnedBytes();
	EXPECT_GT(PresentationTransactionBytes, 0u);

	ASSERT_TRUE(Transactions->Reset());
	const float Roughness =
		Material->GetMaterialProgram()->Outputs.RoughnessDefault.X;
	ASSERT_TRUE(FMaterialGraphOperations::SetSurfaceDefault(*Material, {
		.Output = EMaterialSurfaceOutput::Roughness,
		.Value = {Roughness == 0.41f ? 0.42f : 0.41f, 0.0f, 0.0f, 0.0f}},
		Transactions.Get()));
	const size_t SemanticTransactionBytes = Transactions->GetOwnedBytes();
	EXPECT_GT(SemanticTransactionBytes, PresentationTransactionBytes);

	Transactions->Reset();
	MarkAsGarbage(Material);
	CollectGarbage();
}
