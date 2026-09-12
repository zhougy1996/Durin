#include "LegacyMaterialProgramTestFixture.h"
#include "Misc/MountPathTestSupport.h"
#include "MaterialGraphOperations.h"
#include "MaterialGraphDocument.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "MaterialAssetCreation.h"
#include "Graph/MaterialGraphCanvas.h"
#include "Workspace/MaterialEditorWorkspace.h"

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
#include "Texture/Texture2D.h"

#include <gtest/gtest.h>

#include "NativeDObjectTestSupport.h"

namespace Durin::Editor::Material
{
	struct FMaterialGraphCanvasTestAccess
	{
		static auto Select(FMaterialGraphCanvas& Canvas,
			std::initializer_list<FMaterialGraphCanvasNodeId> Nodes) -> void
		{ Canvas.SelectedNodes = Nodes; }
		static auto Frame(FMaterialGraphCanvas& Canvas, const FMaterialGraphView& View,
			bool bAll) -> void
		{
			Canvas.SurfaceGraphPosition = ImVec2(800.0f, 100.0f);
			Canvas.FrameNodes(View, {1000.0f, 600.0f}, bAll
				? FMaterialGraphCanvas::EFrameScope::All
				: FMaterialGraphCanvas::EFrameScope::Selection);
		}
		static auto ProgramSelection(const FMaterialGraphCanvas& Canvas) -> std::vector<FGuid>
		{ return Canvas.GetSelectedProgramNodes(); }
		static auto Prepare(FMaterialGraphCanvas& Canvas, DMaterial& Material)
			-> const FMaterialGraphView& { return Canvas.PrepareView(Material); }
		static auto PrepareVisuals(FMaterialGraphCanvas& Canvas) -> void
		{
			Canvas.PrepareVisualGraph(Canvas.CachedView, {});
		}
		static auto TopologyStale(const FMaterialGraphCanvas& Canvas) -> bool
		{ return Canvas.bVisualGraphTopologyStale; }
		static auto Idle(const FMaterialGraphCanvas& Canvas) -> bool
		{ return std::holds_alternative<FMaterialGraphCanvas::FIdleInteraction>(Canvas.Interaction); }
		static auto Linking(const FMaterialGraphCanvas& Canvas) -> bool
		{ return std::holds_alternative<FMaterialGraphCanvas::FLinkingInteraction>(Canvas.Interaction); }
		static auto Menu(const FMaterialGraphCanvas& Canvas) -> bool
		{ return std::holds_alternative<FMaterialGraphCanvas::FNodeCreationMenuInteraction>(Canvas.Interaction); }
	};
}

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
		if (!Material || !Material->SetMaterialDefinitionsAndProgram(
			MakePBRMaterialParameterDefinitions(),
			Durin::Testing::MakeLegacyPBRMaterialProgram())
			|| !FMaterialGraphOperations::Layout(*Material)) return nullptr;
		return Material;
	}
}

TEST(FMaterialGraphOperationsTests, CustomDeclarationsPersistAndDuplicateTheirIdentity)
{
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture MountRegistry;
	const auto Root = Testing::GetTestWorkDirectory() / "CustomDeclarations";
	Testing::RemoveTestWorkDirectory(Root);
	Testing::RegisterMountPointForTests("/CustomDeclarations/", Root.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CustomDeclarations/Base", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	FMaterialParameterDefinition Definition;
	Definition.Id = FGuid::NewGuid();
	Definition.Name = "LayerTint";
	Definition.Type = EMaterialParameterType::Vector4;
	Definition.Value = FMaterialParameterValue::MakeVector4({0.1, 0.2, 0.3, 0.4});
	ASSERT_TRUE(Material->SetMaterialDefinitionsAndProgram({Definition}, {}));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	auto* Duplicate = Cast<DMaterial>(DuplicateObject(Material, nullptr, "CopiedCustomDeclarations"));
	ASSERT_NE(Duplicate, nullptr);
	ASSERT_EQ(Duplicate->GetParameterDefinitions().size(), 1u);
	EXPECT_EQ(Duplicate->GetParameterDefinitions().front(), Definition);
	MarkObjectHierarchyAsGarbage(Duplicate);
	ASSERT_TRUE(UnloadPackage(Path));
	Material = nullptr;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Path), Material));
	ASSERT_NE(Material, nullptr);
	ASSERT_EQ(Material->GetParameterDefinitions().size(), 1u);
	EXPECT_EQ(Material->GetParameterDefinitions().front(), Definition);
	EXPECT_FALSE(Material->GetPackage()->IsDirty());
	ASSERT_TRUE(UnloadPackage(Path));
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, DeclarationAndFloat4ReferenceUndoTogether)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "DeclarationTransaction");
	Durin::Tests::FTestTransactorOwner Transactions;
	FMaterialParameterDefinition Definition;
	Definition.Id = FGuid::NewGuid();
	Definition.Name = "LayerTint";
	Definition.Type = EMaterialParameterType::Vector4;
	Definition.Value = FMaterialParameterValue::MakeVector4({0.1, 0.2, 0.3, 0.4});
	FMaterialProgram Program;
	FMaterialProgramNode Parameter;
	Parameter.Id = FGuid::NewGuid();
	Parameter.ParameterId = Definition.Id;
	Parameter.Opcode = EMaterialProgramOpcode::Parameter;
	Parameter.ResultType = EMaterialProgramValueType::Float4;
	Program.Nodes.push_back(Parameter);
	const auto Original = *Material->GetMaterialProgram();
	const uint64 Revision = Material->GetMaterialCompileStatus().AuthoredRevision;
	ASSERT_TRUE(FMaterialGraphOperations::ReplaceDefinitionsAndProgram(
		*Material, {Definition}, Program, Transactions.Get()));
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision, Revision + 1);
	ASSERT_NE(Material->FindParameterDefinition(Definition.Id), nullptr);
	EXPECT_EQ(Material->FindParameterDefinition(Definition.Id)->Value.Vector4Value,
		Definition.Value.Vector4Value);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->FindParameterDefinition(Definition.Id), nullptr);
	EXPECT_EQ(*Material->GetMaterialProgram(), Original);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(*Material->GetMaterialProgram(), Program);
	ASSERT_NE(Material->FindParameterDefinition(Definition.Id), nullptr);
	EXPECT_EQ(Material->FindParameterDefinition(Definition.Id)->Type, EMaterialParameterType::Vector4);
	const auto NoChange = FMaterialGraphOperations::ReplaceDefinitionsAndProgram(
		*Material, {Definition}, Program, Transactions.Get());
	EXPECT_EQ(NoChange.Status, EMaterialGraphCommandStatus::NoChange);
	MarkAsGarbage(Material);
	CollectGarbage();
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
	auto Validation = Material->SetMaterialProgram(std::move(Program));
	ASSERT_TRUE(Validation);
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
	EXPECT_TRUE(Material->GetParameterDefinitions().empty());
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
	const FAssetCatalogRefreshResult Refresh =
		RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation);
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
		const FAssetResult Loaded = LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Material);
		ASSERT_TRUE(Loaded) << Loaded.Message;
		ASSERT_NE(Material, nullptr);
		const FMaterialGraphPresentation& Presentation =
			Material->GetMaterialGraphPresentation();
		EXPECT_EQ(Presentation.Nodes.size(),
			Material->GetMaterialProgram()->Nodes.size());
		EXPECT_TRUE(Presentation.bHasMaterialOutputPosition);
		const FMaterialGraphView View = FMaterialGraphOperations::Inspect(*Material);
		EXPECT_EQ(View.Nodes.size(), Material->GetMaterialProgram()->Nodes.size());
		ASSERT_TRUE(UnloadPackage(Path));
	}
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests,
	MakeSurfaceCatalogAndAggregateCommandsAreAtomic)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "AggregateSurfaceCommands");
	ASSERT_NE(Material, nullptr);
	const auto Catalog = FMaterialGraphOperations::EnumerateCatalog();
	const auto Entry = std::ranges::find(Catalog,
		EMaterialProgramOpcode::MakeSurface,
		[](const FMaterialGraphCatalogEntry& Value) {
			return Value.NodeTemplate.Opcode;
		});
	ASSERT_NE(Entry, Catalog.end());
	EXPECT_EQ(Entry->NodeTemplate.ResultType, EMaterialProgramValueType::Surface);
	EXPECT_TRUE(std::ranges::none_of(Catalog,
		[](const FMaterialGraphCatalogEntry& Value) {
			return Value.NodeTemplate.Opcode
				== static_cast<EMaterialProgramOpcode>(30)
				|| Value.NodeTemplate.Opcode
					== static_cast<EMaterialProgramOpcode>(3);
		}));
	FMaterialProgram AggregateProgram = Durin::Testing::MakeLegacyPBRMaterialProgram();
	FMaterialProgramNode Surface;
	Surface.Id = FGuid::NewGuid();
	Surface.Opcode = EMaterialProgramOpcode::MakeSurface;
	Surface.ResultType = EMaterialProgramValueType::Surface;
	Surface.Inputs = {AggregateProgram.Outputs.BaseColor,
		AggregateProgram.Outputs.Normal, AggregateProgram.Outputs.Metallic,
		AggregateProgram.Outputs.Roughness,
		AggregateProgram.Outputs.AmbientOcclusion,
		AggregateProgram.Outputs.Emissive, AggregateProgram.Outputs.Opacity,
		AggregateProgram.Outputs.OpacityMask};
	AggregateProgram.Nodes.push_back(Surface);
	AggregateProgram.Outputs = {.Surface = {Surface.Id, 0}};
	ASSERT_TRUE(Material->SetMaterialDefinitionsAndProgram(
		MakePBRMaterialParameterDefinitions(), AggregateProgram));
	const FGuid SurfaceId = Material->GetMaterialProgram()->Outputs.Surface.SourceNodeId;
	ASSERT_TRUE(SurfaceId.IsValid());
	FMaterialGraphPresentation AggregatePresentation;
	AggregatePresentation.Nodes.push_back({SurfaceId, 100, 100});
	AggregatePresentation.bHasMaterialOutputPosition = true;
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(AggregatePresentation));
	ASSERT_TRUE(FMaterialGraphOperations::DisconnectAggregateSurface(*Material));
	ASSERT_TRUE(FMaterialGraphOperations::AssignAggregateSurface(
		*Material, SurfaceId));
	EXPECT_EQ(Material->GetMaterialProgram()->Outputs.Surface.SourceNodeId,
		SurfaceId);
	EXPECT_FALSE(Material->GetMaterialProgram()->Outputs.BaseColor.SourceNodeId.IsValid());
	const auto Normalized = Normalize(*Material);
	ASSERT_TRUE(Normalized);
	EXPECT_TRUE(Normalized.IR.SurfaceRoot.bAggregate);
	EXPECT_EQ(Normalized.IR.Nodes.size(), AggregateProgram.Nodes.size());
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(FMaterialGraphOperations::CopySelection(
		*Material, std::array{SurfaceId}, Payload));
	EXPECT_TRUE(Payload.bConnectAggregateSurface);
	ASSERT_TRUE(FMaterialGraphOperations::Paste(*Material, Payload, 300, 100));
	EXPECT_NE(Material->GetMaterialProgram()->Outputs.Surface.SourceNodeId,
		SurfaceId);
	ASSERT_TRUE(FMaterialGraphOperations::DisconnectAggregateSurface(*Material));
	EXPECT_FALSE(Material->GetMaterialProgram()->Outputs.Surface.SourceNodeId.IsValid());
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, PresentationSanitizationIsIndependentAndBounded)
{
	const FMaterialProgram Program = Durin::Testing::MakeLegacyPBRMaterialProgram();
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
	Material->PostLoad();
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
		FMaterialGraphOperations::EnumerateCatalog();
	EXPECT_FALSE(Catalog.empty());
	for (uint8 Value = static_cast<uint8>(EMaterialProgramOpcode::Constant);
		Value <= static_cast<uint8>(EMaterialProgramOpcode::BlendNormalsRNM);
		++Value)
	{
		if (Value == static_cast<uint8>(static_cast<EMaterialProgramOpcode>(3))
			|| Value == static_cast<uint8>(static_cast<EMaterialProgramOpcode>(30)))
			continue;
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
		FMaterialGraphOperations::SearchCatalog("multiply");
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
		FMaterialGraphOperations::SearchCatalog({},
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

TEST(FMaterialGraphOperationsTests, CatalogPinsAgreeWithRuntimeValidation)
{
	using Type = EMaterialProgramValueType;
	for (const auto& Entry : FMaterialGraphOperations::EnumerateCatalog())
	{
		const auto Signature = GetMaterialProgramNodeSignature(
			Entry.NodeTemplate.Opcode, Entry.NodeTemplate.ResultType);
		ASSERT_TRUE(Signature);
		ASSERT_EQ(Signature->InputCount, Entry.AcceptedInputTypes.size());
		for (uint8 Pin = 0; Pin < Signature->InputCount; ++Pin)
			for (Type SourceType : {Type::Float, Type::Float2, Type::Float3,
				Type::Float4, Type::Texture2D, Type::Surface})
			{
				SCOPED_TRACE(std::format("{} result {} pin {} source {}", Entry.OperationName,
					static_cast<uint8>(Entry.NodeTemplate.ResultType), Pin, static_cast<uint8>(SourceType)));
				FMaterialProgram Program;
				FMaterialParameterDefinition Texture;
				Texture.Id = FGuid::NewGuid();
				Texture.Name = "SignatureTexture";
				Texture.Type = EMaterialParameterType::Texture;
				const std::array Definitions{Texture};
				std::function<FGuid(Type)> AddSource = [&](Type ValueType) {
					FMaterialProgramNode Node;
					Node.Id = FGuid::NewGuid();
					Node.ResultType = ValueType;
					if (ValueType == Type::Texture2D)
					{
						Node.Opcode = EMaterialProgramOpcode::TextureParameter;
						Node.ParameterId = Texture.Id;
					}
					else if (ValueType == Type::Surface)
					{
						Node.Opcode = EMaterialProgramOpcode::MakeSurface;
						for (uint8 Index = 0; Index < 8; ++Index)
							Node.Inputs.push_back({AddSource(GetMaterialSurfaceOutputType(
								static_cast<EMaterialSurfaceOutput>(Index))), 0});
					}
					Program.Nodes.push_back(Node);
					return Node.Id;
				};
				FMaterialProgramNode Target = Entry.NodeTemplate;
				Target.Id = FGuid::NewGuid();
				// A zero mask permits every numeric source width; payload bounds are tested separately.
				Target.SwizzleX = Target.SwizzleY = Target.SwizzleZ = Target.SwizzleW = 0;
				for (uint8 Index = 0; Index < Signature->InputCount; ++Index)
					Target.Inputs[Index] = {AddSource(Index == Pin
						? SourceType : Entry.AcceptedInputTypes[Index].front()), 0};
				Program.Nodes.push_back(Target);
				const auto& Accepted = Entry.AcceptedInputTypes[Pin];
				const bool bAccepted = std::ranges::find(Accepted, SourceType) != Accepted.end();
				EXPECT_EQ(static_cast<bool>(ValidateMaterialProgram(Program, Definitions)), bAccepted);
				Program.Nodes.back().Inputs.pop_back();
				EXPECT_FALSE(ValidateMaterialProgram(Program, Definitions));
			}
	}
}

TEST(FMaterialGraphOperationsTests, SignaturesRejectInvalidResultsAndKeepSwizzlePayloadValidation)
{
	using Type = EMaterialProgramValueType;
	for (EMaterialProgramOpcode Opcode : {EMaterialProgramOpcode::Add,
		EMaterialProgramOpcode::Negate, EMaterialProgramOpcode::Clamp})
		for (Type ResultType : {Type::Texture2D, Type::Surface})
		{
			EXPECT_FALSE(GetMaterialProgramNodeSignature(Opcode, ResultType));
			FMaterialProgram Program;
			FMaterialProgramNode Source;
			Source.Id = FGuid::NewGuid();
			Program.Nodes.push_back(Source);
			FMaterialProgramNode Target;
			Target.Id = FGuid::NewGuid();
			Target.Opcode = Opcode;
			Target.ResultType = ResultType;
			Target.Inputs.resize(Opcode == EMaterialProgramOpcode::Add ? 2
				: Opcode == EMaterialProgramOpcode::Clamp ? 3 : 1, {Source.Id, 0});
			Program.Nodes.push_back(Target);
			EXPECT_FALSE(ValidateMaterialProgram(Program, {}));
		}
	EXPECT_FALSE(GetMaterialProgramNodeSignature(static_cast<EMaterialProgramOpcode>(3), Type::Float));
	EXPECT_FALSE(GetMaterialProgramNodeSignature(static_cast<EMaterialProgramOpcode>(255), Type::Float));
	EXPECT_FALSE(GetMaterialProgramNodeSignature(EMaterialProgramOpcode::Constant, static_cast<Type>(255)));
	EXPECT_FALSE(GetMaterialProgramNodeSignature(EMaterialProgramOpcode::Normalize, Type::Float));
	FMaterialProgram Program;
	FMaterialProgramNode Source;
	Source.Id = FGuid::NewGuid();
	Program.Nodes.push_back(Source);
	FMaterialProgramNode Swizzle;
	Swizzle.Id = FGuid::NewGuid();
	Swizzle.Opcode = EMaterialProgramOpcode::Swizzle;
	Swizzle.Inputs = {{Source.Id, 0}};
	Swizzle.SwizzleLength = 1;
	Program.Nodes.push_back(Swizzle);
	ASSERT_TRUE(ValidateMaterialProgram(Program, {}));
	Program.Nodes.back().SwizzleX = 1;
	EXPECT_FALSE(ValidateMaterialProgram(Program, {}));
	Program.Nodes.back().SwizzleX = 0;
	Program.Nodes.back().SwizzleLength = 2;
	EXPECT_FALSE(ValidateMaterialProgram(Program, {}));
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
		FMaterialGraphOperations::SearchCatalog("multiply");
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

TEST(FMaterialGraphOperationsTests, ConstantPaletteUsesOneEntryAndTypeChangesPreserveValidGraphs)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "GenericConstantMaterial");
	ASSERT_NE(Material, nullptr);
	auto Entries = FMaterialGraphOperations::SearchCatalog("constant");
	std::erase_if(Entries, [](const FMaterialGraphCatalogEntry& Entry) {
		return Entry.NodeTemplate.Opcode != EMaterialProgramOpcode::Constant;
	});
	ASSERT_EQ(Entries.size(), 1u);
	EXPECT_EQ(Entries.front().NodeTemplate.ResultType, EMaterialProgramValueType::Float);
	Durin::Tests::FTestTransactorOwner Transactions;
	const auto Created = FMaterialGraphOperations::CreateNodeWithDefaultInputs(*Material,
		{.Node = Entries.front().NodeTemplate}, Entries.front().AcceptedInputTypes, Transactions.Get());
	ASSERT_TRUE(Created) << Created.Message;
	auto Node = Material->GetMaterialProgram()->Nodes.back();
	Node.Literal = {0.2f, 0.4f, 0.6f, 0.8f};
	ASSERT_TRUE(FMaterialGraphOperations::ReplaceNode(*Material, Node, Transactions.Get()));
	for (auto Type : {EMaterialProgramValueType::Float2, EMaterialProgramValueType::Float3,
		EMaterialProgramValueType::Float4})
	{
		const auto Before = *Material->GetMaterialProgram();
		Node.ResultType = Type;
		ASSERT_TRUE(FMaterialGraphOperations::ReplaceNode(*Material, Node, Transactions.Get()));
		EXPECT_EQ(Material->GetMaterialProgram()->Nodes.back(), Node);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(*Material->GetMaterialProgram(), Before);
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(Material->GetMaterialProgram()->Nodes.back(), Node);
	}
	Node.ResultType = EMaterialProgramValueType::Float;
	ASSERT_TRUE(FMaterialGraphOperations::ReplaceNode(*Material, Node, Transactions.Get()));
	ASSERT_TRUE(FMaterialGraphOperations::AssignSurfaceOutput(*Material,
		{.Output = EMaterialSurfaceOutput::Roughness, .SourceNodeId = Node.Id}, Transactions.Get()));
	const auto Connected = *Material->GetMaterialProgram();
	const auto Revision = Material->GetMaterialCompileStatus().AuthoredRevision;
	Node.ResultType = EMaterialProgramValueType::Float4;
	EXPECT_FALSE(FMaterialGraphOperations::ReplaceNode(*Material, Node, Transactions.Get()));
	EXPECT_EQ(*Material->GetMaterialProgram(), Connected);
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision, Revision);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FALSE(Material->GetMaterialProgram()->Outputs.Roughness.SourceNodeId.IsValid());
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, ExistingParameterCreationDoesNotAddDeclarationsAndUsesLiveLabels)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "SharedParameterCreation");
	ASSERT_NE(Material, nullptr);
	const auto Catalog = FMaterialGraphOperations::EnumerateCatalog();
	EXPECT_TRUE(std::ranges::none_of(Catalog, [](const auto& Entry) {
		return Entry.NodeTemplate.ParameterId.IsValid();
	}));
	FMaterialParameterDefinition Definition;
	Definition.Name = FName("SharedValue");
	Definition.Type = EMaterialParameterType::Vector4;
	Definition.Value = FMaterialParameterValue::MakeVector4({1.0, 2.0, 3.0, 4.0});
	ASSERT_TRUE(FMaterialGraphOperations::CreateParameter(*Material, Definition));
	const auto* Stored = Material->FindParameterDefinition(Definition.Name);
	ASSERT_NE(Stored, nullptr);
	const auto ParameterId = Stored->Id;
	const size_t DefinitionCount = Material->GetParameterDefinitions().size();
	const auto Before = *Material->GetMaterialProgram();
	Durin::Tests::FTestTransactorOwner Transactions;
	for (int Index = 0; Index < 2; ++Index)
	{
		FMaterialProgramNode Node;
		Node.Opcode = EMaterialProgramOpcode::Parameter;
		Node.ResultType = EMaterialProgramValueType::Float4;
		Node.ParameterId = ParameterId;
		const auto Result = FMaterialGraphOperations::CreateNodeWithDefaultInputs(*Material,
			{.Node = Node}, {}, Transactions.Get());
		ASSERT_TRUE(Result) << Result.Message;
		EXPECT_EQ(Material->GetParameterDefinitions().size(), DefinitionCount);
		EXPECT_EQ(Material->GetMaterialProgram()->Nodes.back().ParameterId, ParameterId);
	}
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(*Material->GetMaterialProgram(), Before);
	EXPECT_EQ(Material->GetParameterDefinitions().size(), DefinitionCount);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(FMaterialGraphOperations::RenameParameter(*Material, ParameterId, FName("RenamedShared")));
	const auto View = FMaterialGraphOperations::Inspect(*Material, Catalog);
	const auto* Node = FindViewNode(View, Material->GetMaterialProgram()->Nodes.back().Id);
	ASSERT_NE(Node, nullptr);
	EXPECT_EQ(Node->SecondaryLabel, Material->FindParameterDefinition(ParameterId)->DisplayName);
	EXPECT_EQ(FMaterialGraphOperations::EnumerateCatalog().size(), Catalog.size());
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, GenericParametersCreateIndependentDeclarationsAndUndoAtomically)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "GenericParameterMaterial");
	ASSERT_NE(Material, nullptr);
	auto Entries = FMaterialGraphOperations::SearchCatalog("parameter");
	const auto IsParameter = [](const FMaterialGraphCatalogEntry& Entry) {
		return Entry.NodeTemplate.Opcode == EMaterialProgramOpcode::Parameter
			|| Entry.NodeTemplate.Opcode == EMaterialProgramOpcode::TextureParameter;
	};
	std::erase_if(Entries, [&](const auto& Entry) { return !IsParameter(Entry); });
	ASSERT_EQ(Entries.size(), 5u);
	Durin::Tests::FTestTransactorOwner Transactions;
	for (const auto& Entry : Entries)
	{
		EXPECT_FALSE(Entry.NodeTemplate.ParameterId.IsValid());
		const auto Before = *Material->GetMaterialProgram();
		const auto BeforePresentation = Material->GetMaterialGraphPresentation();
		const std::vector<FMaterialParameterDefinition> BeforeDefinitions(
			Material->GetParameterDefinitions().begin(), Material->GetParameterDefinitions().end());
		const auto Created = FMaterialGraphOperations::CreateNodeWithDefaultInputs(*Material,
			{.Node = Entry.NodeTemplate, .X = 400, .Y = 200}, Entry.AcceptedInputTypes, Transactions.Get());
		ASSERT_TRUE(Created) << Created.Message;
		ASSERT_EQ(Created.GeneratedNodeIds.size(), 1u);
		ASSERT_EQ(Created.AffectedParameterIds.size(), 1u);
		EXPECT_EQ(Material->GetParameterDefinitions().size(), BeforeDefinitions.size() + 1);
		const auto After = *Material->GetMaterialProgram();
		const auto AfterPresentation = Material->GetMaterialGraphPresentation();
		EXPECT_EQ(After.Nodes.back().ParameterId, Created.AffectedParameterIds.front());
		EXPECT_EQ(After.Nodes.back().ResultType, Entry.NodeTemplate.ResultType);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(*Material->GetMaterialProgram(), Before);
		EXPECT_EQ(Material->GetMaterialGraphPresentation(), BeforePresentation);
		EXPECT_TRUE(std::ranges::equal(Material->GetParameterDefinitions(), BeforeDefinitions));
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(*Material->GetMaterialProgram(), After);
		EXPECT_EQ(Material->GetMaterialGraphPresentation(), AfterPresentation);

		const auto Second = FMaterialGraphOperations::CreateNodeWithDefaultInputs(*Material,
			{.Node = Entry.NodeTemplate}, Entry.AcceptedInputTypes, Transactions.Get());
		ASSERT_TRUE(Second) << Second.Message;
		EXPECT_NE(Second.AffectedParameterIds.front(), Created.AffectedParameterIds.front());
		const auto* FirstDefinition = Material->FindParameterDefinition(Created.AffectedParameterIds.front());
		const auto* SecondDefinition = Material->FindParameterDefinition(Second.AffectedParameterIds.front());
		ASSERT_NE(FirstDefinition, nullptr);
		ASSERT_NE(SecondDefinition, nullptr);
		EXPECT_NE(FirstDefinition->Name, SecondDefinition->Name);
		auto SharedNode = Material->GetMaterialProgram()->Nodes.back();
		SharedNode.ParameterId = FirstDefinition->Id;
		SharedNode.DisplayName = FirstDefinition->DisplayName;
		ASSERT_TRUE(FMaterialGraphOperations::ReplaceNode(*Material, SharedNode, Transactions.Get()));
		EXPECT_EQ(Material->GetMaterialProgram()->Nodes.back().ParameterId, Created.AffectedParameterIds.front());
		EXPECT_EQ(std::ranges::count_if(FMaterialGraphOperations::SearchCatalog("parameter"), IsParameter), 5);
	}
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
	auto Validation = Material->SetMaterialProgram(MaximumProgram);
	ASSERT_TRUE(Validation);
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
	auto Validation = Material->SetMaterialProgram(
		Durin::Testing::MakeLegacyPBRMaterialProgram());
	ASSERT_TRUE(Validation);
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
	auto Validation = Material->SetMaterialProgram(Program);
	ASSERT_TRUE(Validation);
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
	FMaterialProgram SmallProgram;
	FMaterialProgramNode Constant;
	Constant.Id = FGuid::NewGuid();
	Constant.Opcode = EMaterialProgramOpcode::Constant;
	Constant.ResultType = EMaterialProgramValueType::Float3;
	Constant.Literal = {0.2f, 0.4f, 0.6f};
	SmallProgram.Nodes.push_back(Constant);
	FMaterialProgramNode Saturate;
	Saturate.Id = FGuid::NewGuid();
	Saturate.Opcode = EMaterialProgramOpcode::Saturate;
	Saturate.ResultType = EMaterialProgramValueType::Float3;
	Saturate.Inputs = {{Constant.Id, 0}};
	SmallProgram.Nodes.push_back(Saturate);
	SmallProgram.Outputs.BaseColor = {Saturate.Id, 0};
	ASSERT_TRUE(Material->SetMaterialProgram(SmallProgram));
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

TEST(FMaterialGraphOperationsTests, CanvasFramesExplicitScopeWithMaterialOutputIdentity)
{
	FMaterialGraphView View;
	FMaterialGraphNodeView Node;
	Node.Node.Id = FGuid::NewGuid();
	Node.Presentation.X = -1000;
	Node.Presentation.Y = -100;
	View.Nodes.push_back(Node);
	FMaterialGraphCanvas Canvas;
	const auto& Metrics = FMaterialGraphGeometry::GetMetrics();
	const auto ExpectCenter = [&](float X, float Y)
	{
		const auto [Zoom, Pan] = Canvas.GetViewport();
		EXPECT_NEAR((500.0f - Pan.x) / Zoom, X, 0.001f);
		EXPECT_NEAR((300.0f - Pan.y) / Zoom, Y, 0.001f);
	};
	for (const bool bAggregate : {false, true})
	{
		View.Outputs.Surface.SourceNodeId = bAggregate ? Node.Node.Id : FGuid{};
		const float OutputHeight = Metrics.SurfaceHeaderHeight
			+ Metrics.PinRowHeight * (bAggregate ? 1.0f : 8.0f) + Metrics.BodyPadding;
		FMaterialGraphCanvasTestAccess::Select(Canvas, {EMaterialGraphTerminal::MaterialOutput});
		FMaterialGraphCanvasTestAccess::Frame(Canvas, View, false);
		ExpectCenter(800.0f + Metrics.SurfaceWidth * 0.5f, 100.0f + OutputHeight * 0.5f);
		EXPECT_TRUE(FMaterialGraphCanvasTestAccess::ProgramSelection(Canvas).empty());
		const auto Selection = Canvas.GetSelection();
		FMaterialGraphCanvasTestAccess::Frame(Canvas, View, true);
		ExpectCenter((-1000.0f + 800.0f + Metrics.SurfaceWidth) * 0.5f,
			(-100.0f + 100.0f + OutputHeight) * 0.5f);
		EXPECT_EQ(Canvas.GetSelection(), Selection);
		const auto AllViewport = Canvas.GetViewport();
		FMaterialGraphCanvasTestAccess::Select(Canvas, {Node.Node.Id, EMaterialGraphTerminal::MaterialOutput});
		FMaterialGraphCanvasTestAccess::Frame(Canvas, View, false);
		EXPECT_FLOAT_EQ(Canvas.GetViewport().first, AllViewport.first);
		EXPECT_FLOAT_EQ(Canvas.GetViewport().second.x, AllViewport.second.x);
		EXPECT_FLOAT_EQ(Canvas.GetViewport().second.y, AllViewport.second.y);
		EXPECT_EQ(FMaterialGraphCanvasTestAccess::ProgramSelection(Canvas), std::vector<FGuid>{Node.Node.Id});
	}
	FMaterialGraphCanvasTestAccess::Select(Canvas, {Node.Node.Id});
	FMaterialGraphCanvasTestAccess::Frame(Canvas, View, false);
	ExpectCenter(-1000.0f + Metrics.NodeWidth * 0.5f,
		-100.0f + FMaterialGraphGeometry::GetNodeHeight(0) * 0.5f);
	FMaterialGraphCanvasTestAccess::Select(Canvas, {});
	const auto Before = Canvas.GetViewport();
	FMaterialGraphCanvasTestAccess::Frame(Canvas, View, false);
	EXPECT_FLOAT_EQ(Canvas.GetViewport().first, Before.first);
	EXPECT_FLOAT_EQ(Canvas.GetViewport().second.x, Before.second.x);
	EXPECT_FLOAT_EQ(Canvas.GetViewport().second.y, Before.second.y);
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
	EXPECT_TRUE(FirstCanvas.GetSelection().contains(EMaterialGraphTerminal::MaterialOutput));
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

TEST(FMaterialGraphOperationsTests, CanvasPositionRefreshPreservesTopologyStorage)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "CanvasPositionCache");
	FMaterialParameterDefinition Definition;
	Definition.Id = FGuid::NewGuid();
	Definition.Name = "CanvasParameter";
	Definition.Type = EMaterialParameterType::Scalar;
	Definition.Value = FMaterialParameterValue::MakeScalar(0.5f);
	const FGuid ParameterId = FGuid::NewGuid();
	FMaterialProgram Program;
	Program.Nodes = {
		{.Id = ParameterId, .Opcode = EMaterialProgramOpcode::Parameter,
			.ResultType = EMaterialProgramValueType::Float, .ParameterId = Definition.Id},
		{.Id = FGuid::NewGuid(), .Opcode = EMaterialProgramOpcode::Saturate,
			.ResultType = EMaterialProgramValueType::Float, .Inputs = {{ParameterId, 0}}}};
	ASSERT_TRUE(Material->SetMaterialDefinitionsAndProgram({Definition}, Program));
	ASSERT_TRUE(FMaterialGraphOperations::Layout(*Material));
	FMaterialGraphCanvas Canvas;
	const auto& View = FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material);
	ASSERT_FALSE(View.Nodes.empty());
	FMaterialGraphCanvasTestAccess::PrepareVisuals(Canvas);
	const auto* Nodes = View.Nodes.data();
	std::vector<const FMaterialGraphPinView*> Pins;
	for (const auto& Node : View.Nodes) Pins.push_back(Node.Inputs.data());
	for (int Sample = 0; Sample < 20; ++Sample)
	{
		auto Presentation = Material->GetMaterialGraphPresentation();
		Presentation.Nodes.front().X += 7;
		Presentation.MaterialOutputY += 3;
		ASSERT_TRUE(Material->SetMaterialGraphPresentation(Presentation));
		FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material);
		EXPECT_EQ(View.Nodes.data(), Nodes);
		EXPECT_FALSE(FMaterialGraphCanvasTestAccess::TopologyStale(Canvas));
		EXPECT_EQ(FindViewNode(View, Presentation.Nodes.front().NodeId)->Presentation,
			Presentation.Nodes.front());
		EXPECT_EQ(View.MaterialOutputPosition.second, Presentation.MaterialOutputY);
		for (size_t Index = 0; Index < Pins.size(); ++Index)
			EXPECT_EQ(View.Nodes[Index].Inputs.data(), Pins[Index]);
	}
	const auto ParameterNode = std::ranges::find_if(View.Nodes,
		[](const auto& Node) { return Node.Node.ParameterId.IsValid(); });
	ASSERT_NE(ParameterNode, View.Nodes.end());
	ASSERT_TRUE(Material->RenameParameterDefinition(ParameterNode->Node.ParameterId,
		FName("RenamedCanvasParameter")));
	FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material);
	EXPECT_EQ(FindViewNode(View, ParameterId)->SecondaryLabel,
		Material->FindParameterDefinition(Definition.Id)->DisplayName);
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, FunctionClipboardRetainsDependenciesAndRemapsCompleteLinks)
{
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "ClipboardFunction");
	auto* Source = NewObject<DMaterial>(nullptr, "ClipboardFunctionSource");
	auto* Target = NewObject<DMaterial>(nullptr, "ClipboardFunctionTarget");
	FStrongObjectPtr RetainedTarget(Target);
	Source->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	Target->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument SourceDocument(*Source), TargetDocument(*Target);
	const auto Call = SourceDocument.InsertFunctionCall(*Function, 0, 0);
	ASSERT_TRUE(Call);
	const auto OutputId = Function->GetFunctionSignature().Outputs[0].Id;
	ASSERT_TRUE(SourceDocument.AssignMaterialOutput(std::nullopt, {Call.GeneratedNodeIds[0], 0, OutputId}));
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(SourceDocument.CopySelection(Call.GeneratedNodeIds, Payload));
	ASSERT_EQ(Payload.Calls.size(), 1u);
	TWeakObjectPtr<DMaterialFunction> WeakFunction(Function);
	MarkAsGarbage(Source);
	CollectGarbage();
	ASSERT_TRUE(WeakFunction.IsValid());
	EXPECT_FALSE(Payload.SourceRoot.IsValid());
	Durin::Tests::FTestTransactorOwner Transactions;
	const auto Before = *Target->GetMaterialProgram();
	const auto Pasted = TargetDocument.Paste(Payload, 100, 100, Transactions.Get());
	ASSERT_TRUE(Pasted) << Pasted.Message;
	EXPECT_NE(Pasted.GeneratedNodeIds[0], Call.GeneratedNodeIds[0]);
	EXPECT_EQ(Target->GetMaterialProgram()->Outputs.Surface.SourceOutputId, OutputId);
	EXPECT_EQ(Target->GetMaterialProgram()->Outputs.Surface.SourceNodeId, Pasted.GeneratedNodeIds[0]);
	EXPECT_EQ(Target->GetMaterialFunctionCalls()[0].Function.Get(), WeakFunction.Get());
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(*Target->GetMaterialProgram(), Before);
	EXPECT_TRUE(Target->GetMaterialFunctionCalls().empty());
	ASSERT_TRUE(Transactions->Redo());
	FMaterialGraphDocument FunctionDocument(*WeakFunction.Get());
	const auto FunctionBefore = WeakFunction.Get()->GetFunctionGraph();
	EXPECT_FALSE(FunctionDocument.Paste(Payload, 0, 0));
	EXPECT_EQ(WeakFunction.Get()->GetFunctionGraph(), FunctionBefore);
	Transactions->Reset();
	Payload = {};
	RetainedTarget.Reset();
	MarkAsGarbage(Target);
	MarkAsGarbage(WeakFunction.Get());
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, FunctionClipboardCopiesPortsAndSurfaceBindingsAtomically)
{
	InitializeDObjectSystem();
	auto* Source = NewObject<DMaterialFunction>(nullptr, "ClipboardPortSource");
	auto* Target = NewObject<DMaterialFunction>(nullptr, "ClipboardPortTarget");
	FMaterialGraphDocument Document(*Source), Destination(*Target);
	FMaterialFunctionPort Value{.Name = "Amount", .Default = {.Kind = EMaterialFunctionDefaultKind::Numeric}};
	const auto Input = Document.AddPort(false, Value);
	ASSERT_TRUE(Input);
	const auto OriginalInputId = Source->GetFunctionSignature().Inputs.back().Id;
	ASSERT_TRUE(Document.AddPort(false, {.Name = "Follow", .Default = {.Kind = EMaterialFunctionDefaultKind::Input, .InputId = OriginalInputId}}));
	const auto SurfaceId = Source->GetFunctionGraph().Nodes.front().Id;
	const auto Set = Document.CreateNode({.Node = {.Opcode = EMaterialProgramOpcode::SetSurfaceAttributes,
		.ResultType = EMaterialProgramValueType::Surface, .Inputs = {{SurfaceId}},
		.SurfaceAttributes = {{EMaterialSurfaceOutput::Metallic, {Input.GeneratedNodeIds[0]}}}}});
	ASSERT_TRUE(Set);
	ASSERT_TRUE(Document.AddPort(true, {.Type = EMaterialProgramValueType::Surface, .Name = "Modified"}, {Set.GeneratedNodeIds[0]}));
	std::vector<FGuid> Selection;
	for (const auto& Node : Source->GetFunctionGraph().Nodes) Selection.push_back(Node.Id);
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(Document.CopySelection(Selection, Payload));
	const auto Before = Target->GetFunctionGraph();
	Durin::Tests::FTestTransactorOwner Transactions;
	const auto Pasted = Destination.Paste(Payload, 400, 100, Transactions.Get());
	ASSERT_TRUE(Pasted) << Pasted.Message;
	const auto& Graph = Target->GetFunctionGraph();
	EXPECT_EQ(Graph.Signature.Inputs.size(), 4u);
	const auto Follow = std::ranges::find(Graph.Signature.Inputs, std::string("Follow"), &FMaterialFunctionPort::Name);
	ASSERT_NE(Follow, Graph.Signature.Inputs.end());
	EXPECT_NE(Follow->Default.InputId, OriginalInputId);
	EXPECT_NE(std::ranges::find(Graph.Signature.Inputs, Follow->Default.InputId, &FMaterialFunctionPort::Id), Graph.Signature.Inputs.end());
	EXPECT_EQ(Graph.Signature.Outputs.size(), 3u);
	std::unordered_set<FGuid> NewIds(Pasted.GeneratedNodeIds.begin(), Pasted.GeneratedNodeIds.end());
	for (const auto& Node : Graph.Nodes)
		if (NewIds.contains(Node.Id))
		{
			for (const auto& Link : Node.Inputs) EXPECT_TRUE(NewIds.contains(Link.SourceNodeId));
			for (const auto& Attribute : Node.SurfaceAttributes) EXPECT_TRUE(NewIds.contains(Attribute.Source.SourceNodeId));
		}
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Target->GetFunctionGraph(), Before);
	ASSERT_TRUE(Transactions->Redo());
	FMaterialGraphClipboardPayload Cut;
	ASSERT_TRUE(Destination.CutSelection(Pasted.GeneratedNodeIds, Cut, Transactions.Get()));
	EXPECT_EQ(Target->GetFunctionGraph(), Before);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Target->GetFunctionGraph().Signature.Outputs.size(), 3u);
	Transactions->Reset();
	MarkAsGarbage(Source); MarkAsGarbage(Target); CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, FunctionCanvasConnectsAndMovesNodesWithUndo)
{
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "InteractiveFunction");
	FMaterialGraphDocument Document(*Function);
	const auto Constant = Document.CreateNode({.Node = {.Literal = {.X = 0.5f}}, .X = 0, .Y = 300});
	ASSERT_TRUE(Constant);
	const auto Other = Document.CreateNode({.Node = {.Literal = {.X = 0.9f}}, .X = 0, .Y = 550});
	ASSERT_TRUE(Other);
	const auto Port = Document.AddPort(true, {.Name = "Amount"}, {Constant.GeneratedNodeIds[0]}, 350, 300);
	ASSERT_TRUE(Port);
	ImGuiContext* Context = ImGui::CreateContext();
	auto& IO = ImGui::GetIO();
	IO.DisplaySize = {1200, 1000}; IO.DeltaTime = 1.0f / 60.0f; IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault(); IO.Fonts->Build();
	Durin::Tests::FTestTransactorOwner Transactions;
	FMaterialGraphCanvas Canvas;
	Canvas.SetViewport(1, {40, 40});
	ImVec2 Origin;
	int Errors = 0;
	const auto Frame = [&](ImVec2 Mouse, bool Down) {
		IO.AddMousePosEvent(Mouse.x, Mouse.y); IO.AddMouseButtonEvent(ImGuiMouseButton_Left, Down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1200, 1000});
		ImGui::Begin("Function Canvas", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		Canvas.DrawFunction(*Function, *Transactions.Get(), 900, [&](std::string) { ++Errors; }, [](std::string_view) {});
		const auto* Child = ImGui::GetCurrentWindow()->DC.ChildWindows.back();
		Origin = {Child->Pos.x + Child->WindowPadding.x + 40, Child->Pos.y + Child->WindowPadding.y + 40};
		ImGui::End(); ImGui::Render();
	};
	Frame({1100, 950}, false); Frame({1100, 950}, false);
	const auto& Metrics = FMaterialGraphGeometry::GetMetrics();
	const float PinY = Metrics.HeaderHeight + Metrics.SecondaryHeight + Metrics.BodyPadding;
	const ImVec2 Source{Origin.x + Metrics.NodeWidth, Origin.y + 550 + PinY};
	const ImVec2 Destination{Origin.x + 350, Origin.y + 300 + PinY};
	IO.AddKeyEvent(ImGuiMod_Shift, true);
	Frame(Source, false); Frame(Source, true); Frame(Destination, true); Frame(Destination, false);
	EXPECT_EQ(Errors, 0);
	const auto& Nodes = Function->GetFunctionGraph().Nodes;
	const auto Output = std::ranges::find(Nodes, Port.GeneratedNodeIds[0], &FMaterialProgramNode::Id);
	ASSERT_NE(Output, Nodes.end());
	EXPECT_EQ(Output->Inputs[0].SourceNodeId, Other.GeneratedNodeIds[0]);
	ASSERT_TRUE(Transactions->Undo());
	const auto Before = Function->GetFunctionPresentation();
	const auto Revision = Function->GetFunctionRevision();
	const ImVec2 Header{Origin.x + 20, Origin.y + 300 + 12};
	Frame(Header, false); Frame(Header, true); Frame({Header.x + 60, Header.y + 40}, true); Frame({Header.x + 60, Header.y + 40}, false);
	EXPECT_NE(Function->GetFunctionPresentation(), Before);
	EXPECT_EQ(Function->GetFunctionRevision(), Revision);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Function->GetFunctionPresentation(), Before);
	ImGui::DestroyContext(Context);
	Transactions->Reset(); MarkAsGarbage(Function); CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, CanvasConnectsASecondFunctionOutputAndRefreshesItsInterface)
{
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "CanvasMultiOutputFunction");
	auto* Material = NewObject<DMaterial>(nullptr, "CanvasMultiOutputCaller");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument FunctionDocument(*Function), Document(*Material);
	FMaterialFunctionPort Input{.Id = FGuid::NewGuid(), .Name = "Amount",
		.Default = {.Kind = EMaterialFunctionDefaultKind::Numeric, .Numeric = {.X = 0.4f}}};
	const auto AddedInput = FunctionDocument.AddPort(false, Input);
	ASSERT_TRUE(AddedInput);
	FMaterialFunctionPort Output{.Id = FGuid::NewGuid(), .Name = "Amount Output", .DisplayOrder = 1};
	ASSERT_TRUE(FunctionDocument.AddPort(true, Output, {AddedInput.GeneratedNodeIds[0]}));
	const auto Call = Document.InsertFunctionCall(*Function, 0, 0);
	ASSERT_TRUE(Call);
	const auto Constant = Document.CreateNode({.Node = {}, .X = 0, .Y = 300});
	ASSERT_TRUE(Constant);
	const auto Surface = Document.CreateNode({.Node = {.Opcode = EMaterialProgramOpcode::SetSurfaceAttributes,
		.ResultType = EMaterialProgramValueType::Surface,
		.Inputs = {{Call.GeneratedNodeIds[0], 0, Function->GetFunctionSignature().Outputs[0].Id}},
		.SurfaceAttributes = {{EMaterialSurfaceOutput::Metallic, {Constant.GeneratedNodeIds[0]}}}}, .Y = 450});
	ASSERT_TRUE(Surface);
	const uint32 MetallicInput = static_cast<uint32>(EMaterialSurfaceOutput::Metallic) + 1;
	const FMaterialProgramLink AmountSource{Call.GeneratedNodeIds[0], 0, Output.Id};
	EXPECT_FALSE(Document.ConnectInput(Surface.GeneratedNodeIds[0], MetallicInput, AmountSource));
	ASSERT_TRUE(Document.ConnectInput(Surface.GeneratedNodeIds[0], MetallicInput, AmountSource, true));
	const auto SurfaceInspection = Document.Inspect();
	const auto* SurfaceView = FindViewNode(SurfaceInspection, Surface.GeneratedNodeIds[0]);
	ASSERT_NE(SurfaceView, nullptr);
	ASSERT_EQ(SurfaceView->Inputs.size(), 2u);
	EXPECT_EQ(SurfaceView->Inputs[0].AcceptedTypes, std::vector{EMaterialProgramValueType::Surface});
	EXPECT_EQ(SurfaceView->Inputs[1].Link, AmountSource);
	const auto Destination = Document.CreateNode({.Node = {.Opcode = EMaterialProgramOpcode::Saturate,
		.Inputs = {{Constant.GeneratedNodeIds[0]}}}, .X = 350});
	ASSERT_TRUE(Destination);
	auto Presentation = Material->GetMaterialGraphPresentation();
	Presentation.bHasMaterialOutputPosition = true;
	Presentation.MaterialOutputX = 700;
	Presentation.MaterialOutputY = 0;
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(Presentation));
	ImGuiContext* Context = ImGui::CreateContext();
	auto& IO = ImGui::GetIO();
	IO.DisplaySize = {1200, 720}; IO.DeltaTime = 1.0f / 60.0f; IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault(); IO.Fonts->Build();
	Durin::Tests::FTestTransactorOwner Transactions;
	FMaterialGraphCanvas Canvas;
	Canvas.SetViewport(1.0f, {40, 40});
	ImVec2 Origin;
	int Errors = 0;
	const auto Frame = [&](ImVec2 Mouse, bool Down) {
		IO.AddMousePosEvent(Mouse.x, Mouse.y); IO.AddMouseButtonEvent(ImGuiMouseButton_Left, Down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1200, 720});
		ImGui::Begin("Function Links", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		Canvas.Draw(*Material, *Transactions.Get(), 660, [&](std::string) { ++Errors; });
		const auto* Window = ImGui::GetCurrentWindow()->DC.ChildWindows.back();
		Origin = {Window->Pos.x + Window->WindowPadding.x + 40,
			Window->Pos.y + Window->WindowPadding.y + ImGui::GetFrameHeightWithSpacing() + 40};
		ImGui::End(); ImGui::Render();
	};
	Frame({1100, 600}, false); Frame({1100, 600}, false);
	const auto& Metrics = FMaterialGraphGeometry::GetMetrics();
	const float PinY = Metrics.HeaderHeight + Metrics.SecondaryHeight + Metrics.BodyPadding;
	const ImVec2 SecondOutput{Origin.x + Metrics.NodeWidth, Origin.y + PinY + Metrics.PinRowHeight};
	const ImVec2 Target{Origin.x + 350, Origin.y + PinY};
	IO.AddKeyEvent(ImGuiMod_Shift, true);
	Frame(SecondOutput, false); Frame(SecondOutput, true);
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::Linking(Canvas));
	Frame(Target, true); Frame(Target, false);
	EXPECT_EQ(Errors, 0);
	const auto Link = FindViewNode(FMaterialGraphOperations::Inspect(*Material), Destination.GeneratedNodeIds[0])->Node.Inputs[0];
	EXPECT_EQ(Link.SourceNodeId, Call.GeneratedNodeIds[0]);
	EXPECT_EQ(Link.SourceOutputId, Output.Id);
	auto Signature = Function->GetFunctionSignature();
	Signature.Outputs[1].Name = "Renamed Amount";
	ASSERT_TRUE(FunctionDocument.SetSignature(Signature));
	const auto& View = FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material);
	const auto* CallView = FindViewNode(View, Call.GeneratedNodeIds[0]);
	ASSERT_NE(CallView, nullptr);
	ASSERT_EQ(CallView->Outputs.size(), 2u);
	EXPECT_EQ(CallView->Outputs[1].Name, "Renamed Amount");
	const auto Pin = std::ranges::find(CallView->Inputs, Input.Id, &FMaterialGraphPinView::PortId);
	ASSERT_NE(Pin, CallView->Inputs.end());
	EXPECT_FLOAT_EQ(Pin->Default.Numeric.X, 0.4f);
	ImGui::DestroyContext(Context);
	MarkAsGarbage(Material); MarkAsGarbage(Function); CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, CanvasLinkReleaseEndsGestureAcrossFrames)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "CanvasLinkRelease");
	const FGuid Source = FGuid::NewGuid();
	const FGuid Destination = FGuid::NewGuid();
	const FGuid PreviousSource = FGuid::NewGuid();
	FMaterialProgram Program;
	Program.Nodes = {
		{.Id = PreviousSource, .Opcode = EMaterialProgramOpcode::Constant,
			.ResultType = EMaterialProgramValueType::Float},
		{.Id = Source, .Opcode = EMaterialProgramOpcode::Constant,
			.ResultType = EMaterialProgramValueType::Float},
		{.Id = Destination, .Opcode = EMaterialProgramOpcode::Saturate,
			.ResultType = EMaterialProgramValueType::Float, .Inputs = {{PreviousSource, 0}}}};
	ASSERT_TRUE(Material->SetMaterialProgram(Program));
	auto Presentation = Material->GetMaterialGraphPresentation();
	Presentation.Nodes = {{Source, 0, 0}, {Destination, 350, 0}, {PreviousSource, 0, 300}};
	Presentation.MaterialOutputX = 700;
	Presentation.MaterialOutputY = 0;
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(Presentation));
	ImGuiContext* Context = ImGui::CreateContext();
	auto& IO = ImGui::GetIO();
	IO.DisplaySize = {1200, 720};
	IO.DeltaTime = 1.0f / 60.0f;
	IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault();
	IO.Fonts->Build();
	Durin::Tests::FTestTransactorOwner Transactions;
	FMaterialGraphCanvas Canvas;
	Canvas.SetViewport(1.0f, {40, 40});
	ImVec2 Origin;
	int Errors = 0;
	const auto Frame = [&](ImVec2 Mouse, bool Down) {
		IO.AddMousePosEvent(Mouse.x, Mouse.y);
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, Down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0});
		ImGui::SetNextWindowSize({1200, 720});
		ImGui::Begin("Link Release", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		Canvas.Draw(*Material, *Transactions.Get(), 660,
			[&](std::string) { ++Errors; });
		const ImGuiWindow* GraphWindow = ImGui::GetCurrentWindow()->DC.ChildWindows.back();
		Origin = {GraphWindow->Pos.x + GraphWindow->WindowPadding.x + 40,
			GraphWindow->Pos.y + GraphWindow->WindowPadding.y
				+ ImGui::GetFrameHeightWithSpacing() + 40};
		ImGui::End();
		ImGui::Render();
	};
	Frame({1100, 600}, false);
	Frame({1100, 600}, false);
	const auto& Metrics = FMaterialGraphGeometry::GetMetrics();
	const float PinY = Metrics.HeaderHeight + Metrics.SecondaryHeight + Metrics.BodyPadding;
	const ImVec2 Output{Origin.x + Metrics.NodeWidth, Origin.y + PinY};
	const auto Drop = [&](ImVec2 Target, bool Replace) {
		IO.AddKeyEvent(ImGuiMod_Shift, Replace);
		Frame(Output, false);
		Frame(Output, true);
		EXPECT_TRUE(FMaterialGraphCanvasTestAccess::Linking(Canvas));
		Frame(Target, true);
		Frame(Target, false);
		EXPECT_TRUE(FMaterialGraphCanvasTestAccess::Idle(Canvas));
		Frame({1100, 600}, false);
		EXPECT_TRUE(FMaterialGraphCanvasTestAccess::Idle(Canvas));
	};
	Drop({Origin.x + 350, Origin.y + PinY}, false);
	EXPECT_EQ(Errors, 1);
	Drop({Origin.x + 350, Origin.y + PinY}, true);
	EXPECT_EQ(Errors, 1);
	EXPECT_EQ(FindViewNode(FMaterialGraphOperations::Inspect(*Material), Destination)
		->Node.Inputs.front().SourceNodeId, Source);
	Drop({Origin.x + 700, Origin.y + FMaterialGraphGeometry::GetSurfacePinOffset(3)}, false);
	EXPECT_EQ(Material->GetMaterialProgram()->Outputs.Roughness.SourceNodeId, Source);
	Drop({Origin.x + 700, Origin.y + FMaterialGraphGeometry::GetSurfacePinOffset(0)}, false);
	EXPECT_EQ(Errors, 2);
	Drop({Origin.x + 380, Origin.y + 10}, false);
	Frame(Output, false);
	Frame(Output, true);
	Frame({500, 500}, true);
	Frame({500, 500}, false);
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::Menu(Canvas));
	Canvas.CancelInteraction();
	Transactions->Reset();
	ImGui::DestroyContext(Context);
	MarkAsGarbage(Material);
	CollectGarbage();
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
	auto Validation = Material->SetMaterialProgram(DenseProgram);
	ASSERT_TRUE(Validation);
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
	ASSERT_TRUE((Validation = Material->SetMaterialProgram(MaximumProgram)));
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
	auto Validation = Material->SetMaterialProgram(std::move(SemanticEdit));
	ASSERT_TRUE(Validation);
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
	ASSERT_TRUE(Material->SetMaterialDefinitionsAndProgram(
		MakePBRMaterialParameterDefinitions(), MakeDefaultMaterialProgram()));
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
	ASSERT_EQ(Textured.GeneratedNodeIds.size(), 6u);
	const std::vector TextureDependencies = InspectMaterialParameterDependencies(
		*Material->GetMaterialProgram(), Material->GetParameterDefinitions());
	ASSERT_EQ(TextureDependencies.size(), 1u);
	EXPECT_EQ(TextureDependencies.front().ParameterId,
		MaterialParameters::GetBuiltinParameterIds(MaterialParameters::EMaterialBuiltinParameterRole::Normal).Texture);
	EXPECT_EQ(TextureDependencies.back().ParameterId,
		MaterialParameters::GetBuiltinParameterIds(MaterialParameters::EMaterialBuiltinParameterRole::Normal).Texture);
	const FMaterialNormalizationResult Normalized = Normalize(*Material);
	ASSERT_TRUE(Normalized);
	EXPECT_EQ(Normalized.IR.Nodes.size(), 6u);
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


TEST(FMaterialGraphOperationsTests, ForeignClipboardOwnsLocalDeclarationsAndRejectsConflictsAtomically)
{
	InitializeDObjectSystem();
	DMaterial* Source = NewObject<DMaterial>(nullptr, "ClipboardSource");
	DMaterial* Target = NewObject<DMaterial>(nullptr, "ClipboardTarget");
	FMaterialParameterDefinition Definition;
	Definition.Id = FGuid::NewGuid();
	Definition.Name = "LayerTint";
	Definition.DisplayName = "Layer Tint";
	Definition.Type = EMaterialParameterType::Vector4;
	Definition.Value = FMaterialParameterValue::MakeVector4({1, 2, 3, 4});
	FMaterialProgram Program;
	FMaterialProgramNode Node;
	Node.Id = FGuid::NewGuid();
	Node.ParameterId = Definition.Id;
	Node.Opcode = EMaterialProgramOpcode::Parameter;
	Node.ResultType = EMaterialProgramValueType::Float4;
	Program.Nodes.push_back(Node);
	ASSERT_TRUE(Source->SetMaterialDefinitionsAndProgram({Definition}, Program));
	ASSERT_TRUE(FMaterialGraphOperations::Layout(*Source));
	ASSERT_TRUE(Target->SetMaterialDefinitionsAndProgram({}, {}));
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(FMaterialGraphOperations::CopySelection(*Source, std::span(&Node.Id, 1), Payload));
	ASSERT_EQ(Payload.Definitions.size(), 1u);
	Durin::Tests::FTestTransactorOwner Transactions;
	const auto BeforeRevision = Target->GetMaterialCompileStatus().AuthoredRevision;
	const auto Pasted = FMaterialGraphOperations::Paste(*Target, Payload, 300, 200, Transactions.Get());
	ASSERT_TRUE(Pasted) << Pasted.Message;
	ASSERT_EQ(Target->GetParameterDefinitions().size(), 1u);
	const auto LocalId = Target->GetParameterDefinitions().front().Id;
	EXPECT_NE(LocalId, Definition.Id);
	EXPECT_EQ(Target->GetMaterialProgram()->Nodes.front().ParameterId, LocalId);
	EXPECT_EQ(Target->GetMaterialCompileStatus().AuthoredRevision, BeforeRevision + 1);
	const auto AfterPresentation = Target->GetMaterialGraphPresentation();
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_TRUE(Target->GetParameterDefinitions().empty());
	EXPECT_TRUE(Target->GetMaterialProgram()->Nodes.empty());
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Target->GetParameterDefinitions().front().Id, LocalId);
	EXPECT_EQ(Target->GetMaterialGraphPresentation(), AfterPresentation);
	ASSERT_TRUE(FMaterialGraphOperations::Paste(*Target, Payload, 600, 200));
	EXPECT_EQ(Target->GetParameterDefinitions().size(), 1u);
	EXPECT_EQ(Target->GetMaterialProgram()->Nodes.back().ParameterId, LocalId);

	const auto BeforeProgram = *Target->GetMaterialProgram();
	const auto BeforePresentation = Target->GetMaterialGraphPresentation();
	const auto Revision = Target->GetMaterialCompileStatus().AuthoredRevision;
	Payload.Definitions.front().Value.Vector4Value.x = 9;
	EXPECT_FALSE(FMaterialGraphOperations::Paste(*Target, Payload, 0, 0));
	EXPECT_EQ(*Target->GetMaterialProgram(), BeforeProgram);
	EXPECT_EQ(Target->GetMaterialGraphPresentation(), BeforePresentation);
	EXPECT_EQ(Target->GetMaterialCompileStatus().AuthoredRevision, Revision);
	Payload.Definitions.clear();
	EXPECT_FALSE(FMaterialGraphOperations::Paste(*Target, Payload, 0, 0));
	EXPECT_EQ(*Target->GetMaterialProgram(), BeforeProgram);
	MarkAsGarbage(Source);
	MarkAsGarbage(Target);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, SameRootClipboardUsesCurrentNamesAndDefaults)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "ClipboardRename");
	FMaterialParameterDefinition Definition;
	Definition.Id = FGuid::NewGuid();
	Definition.Name = "Amount";
	Definition.DisplayName = "Amount";
	FMaterialProgram Program;
	FMaterialProgramNode Node;
	Node.Id = FGuid::NewGuid();
	Node.ParameterId = Definition.Id;
	Node.Opcode = EMaterialProgramOpcode::Parameter;
	Program.Nodes.push_back(Node);
	ASSERT_TRUE(Material->SetMaterialDefinitionsAndProgram({Definition}, Program));
	ASSERT_TRUE(FMaterialGraphOperations::Layout(*Material));
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(FMaterialGraphOperations::CopySelection(*Material, std::span(&Node.Id, 1), Payload));
	ASSERT_TRUE(Material->RenameParameterDefinition(Definition.Id, "RenamedAmount"));
	ASSERT_TRUE(FMaterialGraphOperations::Paste(*Material, Payload, 400, 0));
	EXPECT_EQ(Material->GetMaterialProgram()->Nodes.back().ParameterId, Definition.Id);
	EXPECT_EQ(Material->GetMaterialProgram()->Nodes.back().DisplayName, "RenamedAmount");
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 1u);
	ASSERT_TRUE(Material->SetMaterialDefinitionsAndProgram({}, {}));
	EXPECT_FALSE(FMaterialGraphOperations::Paste(*Material, Payload, 400, 0));
	EXPECT_TRUE(Material->GetParameterDefinitions().empty());
	MarkAsGarbage(Material);
	CollectGarbage();
}


TEST(FMaterialGraphOperationsTests, DeclarationCommandsAndConstantPromotionShareAtomicHistory)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "DeclarationCommands");
	ASSERT_TRUE(Material->SetMaterialDefinitionsAndProgram({}, {}));
	Durin::Tests::FTestTransactorOwner Transactions;
	FMaterialParameterDefinition Definition;
	Definition.Name = "Amount";
	Definition.DisplayName = "Amount";
	Definition.Value = FMaterialParameterValue::MakeScalar(0.25f);
	const auto Created = FMaterialGraphOperations::CreateParameter(*Material, Definition, Transactions.Get());
	ASSERT_TRUE(Created);
	ASSERT_EQ(Created.AffectedParameterIds.size(), 1u);
	const auto Id = Created.AffectedParameterIds.front();
	ASSERT_TRUE(Id.IsValid());
	Definition.Value = FMaterialParameterValue::MakeScalar(0.75f);
	const auto Reused = FMaterialGraphOperations::CreateParameter(*Material, Definition, Transactions.Get());
	EXPECT_EQ(Reused.Status, EMaterialGraphCommandStatus::NoChange);
	EXPECT_EQ(Reused.AffectedParameterIds.front(), Id);
	EXPECT_EQ(Material->FindParameterDefinition(Id)->Value.ScalarValue, 0.25f);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_TRUE(Material->GetParameterDefinitions().empty());
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(FMaterialGraphOperations::RenameParameter(*Material, Id, "BlendAmount", Transactions.Get()));
	EXPECT_EQ(Material->FindParameterDefinition(Id)->Name, FName("BlendAmount"));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->FindParameterDefinition(Id)->Name, FName("Amount"));
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(FMaterialGraphOperations::DeleteParameter(*Material, Id, Transactions.Get()));
	EXPECT_TRUE(Material->GetParameterDefinitions().empty());
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_NE(Material->FindParameterDefinition(Id), nullptr);
	Transactions->Reset();

	FMaterialProgram Program;
	FMaterialProgramNode Constant;
	Constant.Id = FGuid::NewGuid();
	Constant.Opcode = EMaterialProgramOpcode::Constant;
	Constant.ResultType = EMaterialProgramValueType::Float4;
	Constant.Literal = {1, 2, 3, 4};
	Program.Nodes.push_back(Constant);
	ASSERT_TRUE(Material->SetMaterialProgram(Program));
	const auto Revision = Material->GetMaterialCompileStatus().AuthoredRevision;
	const auto Promoted = FMaterialGraphOperations::PromoteConstantToParameter(
		*Material, Constant.Id, "Tint", Transactions.Get());
	ASSERT_TRUE(Promoted) << Promoted.Message;
	ASSERT_EQ(Promoted.AffectedParameterIds.size(), 1u);
	const auto TintId = Promoted.AffectedParameterIds.front();
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision, Revision + 1);
	EXPECT_EQ(Material->FindParameterDefinition(TintId)->Value.Vector4Value, FVector4(1, 2, 3, 4));
	EXPECT_EQ(Material->GetMaterialProgram()->Nodes.front().Id, Constant.Id);
	EXPECT_EQ(Material->GetMaterialProgram()->Nodes.front().ParameterId, TintId);
	EXPECT_FALSE(FMaterialGraphOperations::DeleteParameter(*Material, TintId, Transactions.Get()));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->FindParameterDefinition(TintId), nullptr);
	EXPECT_EQ(*Material->GetMaterialProgram(), Program);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Material->GetMaterialProgram()->Nodes.front().ParameterId, TintId);
	Transactions->Reset();
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, ClipboardRetainsTextureDefaultsAfterSourceCollection)
{
	InitializeDObjectSystem();
	DMaterial* Source = NewObject<DMaterial>(nullptr, "ClipboardTextureSource");
	DTexture2D* Texture = NewObject<DTexture2D>(nullptr, "ClipboardTexture");
	TWeakObjectPtr<DTexture2D> WeakTexture(Texture);
	FMaterialParameterDefinition Definition;
	Definition.Id = FGuid::NewGuid();
	Definition.Name = "LayerTexture";
	Definition.Type = EMaterialParameterType::Texture;
	Definition.Value = FMaterialParameterValue::MakeTexture(Texture);
	FMaterialProgram Program;
	FMaterialProgramNode Node;
	Node.Id = FGuid::NewGuid();
	Node.Opcode = EMaterialProgramOpcode::TextureParameter;
	Node.ParameterId = Definition.Id;
	Node.ResultType = EMaterialProgramValueType::Texture2D;
	Program.Nodes.push_back(Node);
	ASSERT_TRUE(Source->SetMaterialDefinitionsAndProgram({Definition}, Program));
	ASSERT_TRUE(FMaterialGraphOperations::Layout(*Source));
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(FMaterialGraphOperations::CopySelection(*Source, std::span(&Node.Id, 1), Payload));
	MarkAsGarbage(Source);
	CollectGarbage();
	ASSERT_NE(WeakTexture.Get(), nullptr);
	EXPECT_EQ(Payload.SourceRoot.Get(), nullptr);
	DMaterial* Target = NewObject<DMaterial>(nullptr, "ClipboardTextureTarget");
	ASSERT_TRUE(Target->SetMaterialDefinitionsAndProgram({}, {}));
	ASSERT_TRUE(FMaterialGraphOperations::Paste(*Target, Payload, 0, 0));
	EXPECT_EQ(Target->GetParameterDefinitions().front().Value.TextureValue.Get(), WeakTexture.Get());
	MarkAsGarbage(Target);
	CollectGarbage();
	EXPECT_NE(WeakTexture.Get(), nullptr);
	Payload = {};
	CollectGarbage();
	EXPECT_EQ(WeakTexture.Get(), nullptr);
}

TEST(FMaterialGraphOperationsTests, DocumentDockLayoutsRemainIsolatedAndSurviveHiddenFrames)
{
	ImGuiContext* Context = ImGui::CreateContext();
	ImGuiIO& IO = ImGui::GetIO();
	IO.IniFilename = nullptr;
	IO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	IO.DisplaySize = {1600.0f, 1000.0f};
	IO.DeltaTime = 1.0f / 60.0f;
	IO.Fonts->Build();
	const FDocumentTab Wide{.DocumentKey = "/Test/Wide"};
	const FDocumentTab Narrow{.DocumentKey = "/Test/Narrow"};
	const auto WideType = Workspace::MakeDocumentDockType(Wide);
	const auto NarrowType = Workspace::MakeDocumentDockType(Narrow);
	const ImGuiID WideId = WorkspaceUI::MakeDockSpaceId(WideType, Workspace::LayoutVersion);
	const ImGuiID NarrowId = WorkspaceUI::MakeDockSpaceId(NarrowType, Workspace::LayoutVersion);
	EXPECT_NE(WideId, NarrowId);
	EXPECT_NE(WorkspaceUI::MakeDockClassId(WideType), WorkspaceUI::MakeDockClassId(NarrowType));
	ImGuiID GraphId = 0;
	ImGuiID PreviewId = 0;
	for (int Frame = 0; Frame < 5; ++Frame)
	{
		ImGui::NewFrame();
		for (const auto* Document : {&Wide, &Narrow})
		{
			const auto DockType = Workspace::MakeDocumentDockType(*Document);
			const bool bWide = Document == &Wide;
			const ImVec2 Size = bWide ? ImVec2(1400.0f, 800.0f) : ImVec2(600.0f, 500.0f);
			ImGui::SetNextWindowSize(Size);
			ImGui::Begin(bWide ? "WideHost" : "NarrowHost");
			if (Frame == 0) Workspace::BuildDefaultLayout(*Document, Size);
			WorkspaceUI::SubmitDockSpace(DockType, Workspace::LayoutVersion, Size,
				Frame == 2 ? ImGuiDockNodeFlags_KeepAliveOnly : ImGuiDockNodeFlags_None);
			if (Frame != 2)
			{
				for (const char* Key : {"Graph", "Preview", "Details", "Diagnostics"})
				{
					if (Frame == 0 && std::string_view(Key) == "Diagnostics") continue;
					WorkspaceUI::BeginDockablePanel(DockType, Key, Key);
					const ImGuiWindow* Window = ImGui::GetCurrentWindow();
					EXPECT_NE(Window->DockId, 0u);
					if (!bWide) EXPECT_EQ(Window->DockId, NarrowId);
					else if (std::string_view(Key) == "Graph")
					{
						if (Frame == 0) GraphId = Window->DockId;
						EXPECT_EQ(Window->DockId, GraphId);
					}
					else if (std::string_view(Key) == "Preview")
					{
						if (Frame == 0) PreviewId = Window->DockId;
						EXPECT_EQ(Window->DockId, PreviewId);
						EXPECT_NE(Window->DockId, GraphId);
					}
					else if (std::string_view(Key) == "Diagnostics")
						EXPECT_NE(Window->DockId, GraphId);
					ImGui::End();
				}
			}
			ImGui::End();
		}
		ImGui::Render();
	}
	ImGui::DestroyContext(Context);
}
