#include "NativePropertyControlTestSupport.h"
#include "MaterialGraphPinTestSupport.h"
#include "Graph/MaterialGraphCommands.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "Graph/MaterialGraphInteraction.h"
#include "Graph/MaterialEditorPreviewParameter.h"
#include "PropertyEditor/PropertyEditing.h"
#include "Graph/MaterialGraphCatalog.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionArithmetic.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionMaterialAttributes.h"
#include "Materials/MaterialExpressionNumeric.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustomData.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSwitch.h"
#include "Materials/MaterialExpressionVector.h"
#include "Materials/MaterialExpressionTextureCoordinates.h"
#include "Materials/MaterialExpressionTextureParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionVector4Parameter.h"
#include "Graph/MaterialEditingOperations.h"
#include "MaterialGraph.h"
#include "MaterialGraphTestSupport.h"
#include "Graph/MaterialExpressionParameters.h"
#include "Graph/MaterialGraphEditInternals.h"
#include "Materials/MaterialExpressionDescription.h"

#if DURIN_MATERIAL_GRAPH_WATER_TESTS
#include "MaterialExpressionGerstnerWaves.h"
#include "Misc/MountPathTestSupport.h"
#include "Misc/Paths.h"

namespace
{
	class FMaterialGraphWaterMountEnvironment final : public ::testing::Environment
	{
		auto SetUp() -> void override
		{
			const auto Existing = FMountPaths::GetRegisteredMountPoints();
			std::vector<FMountPoint> Definitions(Existing.begin(), Existing.end());
			// The Water component's class default validates its asset paths during DObject startup.
			Definitions.push_back({.VirtualRoot = "/Plugins/Water/", .Owner = EMountOwner::Extension,
				.Root = std::filesystem::path(FPaths::RootDir()) / "Engine/Plugins/Water/Content"});
			Mounts = std::make_unique<Testing::FScopedMountRegistryFixture>(Definitions);
			ASSERT_TRUE(Mounts->IsValid()) << Mounts->GetError();
		}

		auto TearDown() -> void override { Mounts.reset(); }

		std::unique_ptr<Testing::FScopedMountRegistryFixture> Mounts;
	};

	[[maybe_unused]] const auto* WaterMountEnvironment =
		::testing::AddGlobalTestEnvironment(new FMaterialGraphWaterMountEnvironment);
}
#endif

TEST(FMaterialGraphOperationsTests, ValidCastPermitsDirectEmissiveWire)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
	const auto Wide = Testing::CreateGraphCatalogNode(*Graph, EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float4);
	const auto Narrow = Testing::CreateGraphCatalogNode(*Graph, EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float2);
	ASSERT_TRUE(Wide);
	ASSERT_TRUE(Narrow);
	const auto Emissive = EMaterialProperty::MP_EmissiveColor;
	const auto WideOutput = FMaterialProgramLink{Wide->front(), 0};
	const auto NarrowOutput = FMaterialProgramLink{Narrow->front(), 0};
	EXPECT_FALSE(Graph->GetSchema().CanCreateConnection(Testing::ResolvePin(*Graph, NarrowOutput), Testing::ResolvePin(*Graph, Emissive)).Allows(true));
	EXPECT_TRUE(Testing::TryCreateConnection(*Graph, WideOutput, Emissive, false, false));
	EXPECT_EQ(Material->GetExpressionInputDescription(EMaterialProperty::MP_EmissiveColor)->Connection->GetExpressionId(), Wide->front());
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 2u);
}

TEST(FMaterialGraphOperationsTests, SamplingOutputsRejectRetiredSelectorsWithoutMutatingGraph)
{
	InitializeDObjectSystem();
	using Op = EMaterialProgramOpcode;
	using Type = EMaterialProgramValueType;
	using Mode = EMaterialTextureReadMode;
	for (const auto Opcode : {Op::TextureSample2D, Op::TextureSampleCube, Op::TextureSampleParameter2D, Op::TextureSampleParameterCube})
	{
		const bool bCube = Opcode == Op::TextureSampleCube || Opcode == Op::TextureSampleParameterCube;
		const bool bParameter = Opcode == Op::TextureSampleParameter2D || Opcode == Op::TextureSampleParameterCube;
		TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

		TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
		FMaterialProgramLink Resource;
		if (!bParameter)
		{
			EXPECT_FALSE(Testing::CreateGraphCatalogNode(*AuthoringGraph, Opcode, Type::Float4));
			const auto Texture = Testing::CreateGraphCatalogNode(*AuthoringGraph, Op::TextureParameter, bCube ? Type::TextureCube : Type::Texture2D);
			ASSERT_TRUE(Texture) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Texture);
			Resource = {Texture->front()};
		}
		const auto Sample = Testing::CreateGraphCatalogNode(*AuthoringGraph, Opcode, EMaterialProgramValueType::Float4, Resource);
		const auto Surface = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::MakeMaterialAttributes, EMaterialProgramValueType::MaterialAttributes);
		ASSERT_TRUE(Sample) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Sample);
		ASSERT_TRUE(Surface) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Surface);
		const auto Id = Sample->front();
		const auto Target = Surface->front();
		const auto View = Testing::CaptureGraphView(*AuthoringGraph);
		const auto* Node = FindViewNode(View, Id);
		ASSERT_NE(Node, nullptr);
		auto* Expression = AuthoringGraph->FindNode(Id)->GetMaterialExpression();
		const auto Duplicated = DuplicateObject(Expression, nullptr, NAME_None);
		ASSERT_TRUE(Duplicated);
		TStrongObjectPtr<DMaterialExpression> Draft(*Duplicated);
		auto* ReadMode = Draft->GetTextureSampleReadMode();
		ASSERT_NE(ReadMode, nullptr);
		const int32 Coordinate = bParameter ? 0 : 1;
		const int32 Mip = Coordinate + 1;
		const auto CoordinateType = bCube ? Type::Float3 : Type::Float2;
		ASSERT_EQ(Node->Inputs.size(), static_cast<size_t>(Mip + 3));
		EXPECT_EQ(Expression->GetInputName(Coordinate), bCube ? "Direction" : "UV");
		EXPECT_EQ(Expression->GetInputValueType(Coordinate), CoordinateType);
		EXPECT_EQ(Expression->IsInputConnectionRequired(0), !bParameter);
		EXPECT_FALSE(Expression->IsInputConnectionRequired(Coordinate));
		const auto* CoordinateInput = Expression->GetInput(Coordinate);
		ASSERT_NE(CoordinateInput, nullptr);
		EXPECT_FALSE(CoordinateInput->IsConnected());
		EXPECT_NE(Expression->GetInput(Mip), nullptr);
		EXPECT_FALSE(Node->Inputs[Mip].bActive);
		if (bCube)
		{
			const auto Direction = Testing::ExpressionInput(Id, Coordinate);
			EXPECT_FLOAT_EQ(Node->Inputs[Coordinate].InlineDefault.Literal.X, 1.f);
			ASSERT_TRUE(Testing::WriteInputDefault(Material.Get(), Direction, Node->Inputs[Coordinate].InlineDefault, false));
			const auto* Pin = Testing::InputView(*AuthoringGraph, Direction);
			ASSERT_NE(Pin, nullptr);
			EXPECT_TRUE(Pin->bUseConstant);
			EXPECT_EQ(Pin->RetainedConstant.Type, CoordinateType);
			EXPECT_FLOAT_EQ(Pin->RetainedConstant.Literal.X, 1.f);
			ASSERT_TRUE(Testing::WriteInputDefault(Material.Get(), Direction, {}, false));
			Pin = Testing::InputView(*AuthoringGraph, Direction);
			ASSERT_NE(Pin, nullptr);
			EXPECT_FALSE(Pin->bUseConstant);
			EXPECT_FLOAT_EQ(Pin->InlineDefault.Literal.X, 1.f);
		}
		for (const auto ModeValue : {Mode::Implicit, Mode::Level, Mode::Bias, Mode::Grad})
		{
			*ReadMode = ModeValue;
			EXPECT_TRUE(Draft->GetInputInfo(Coordinate).bActive);
			EXPECT_EQ(Draft->GetInputInfo(Mip).bActive, ModeValue == Mode::Level || ModeValue == Mode::Bias);
			for (int32 Index : {Mip + 1, Mip + 2})
			{
				EXPECT_EQ(Draft->GetInputValueType(Index), CoordinateType);
				EXPECT_EQ(Draft->GetInputInfo(Index).bActive, ModeValue == Mode::Grad);
			}
		}
		std::vector<uint8> Indices;
		for (const auto& Output : Node->Outputs) Indices.push_back(Output.OutputIndex);
		std::vector<uint8> Expected{1, 2, 3, 4, 5, 0};
		if (bParameter) Expected.push_back(7);
		EXPECT_EQ(Indices, Expected);
		for (size_t Ordinal = 0; Ordinal < Expected.size(); ++Ordinal)
		{
			const uint8 Index = Expected[Ordinal];
			const auto Output = Expression->GetOutput(static_cast<int32>(Ordinal));
			ASSERT_TRUE(Output);
			EXPECT_EQ(Output->StoredIndex, Index);
			EXPECT_EQ(Output->Name, Node->Outputs[Ordinal].Name);
			ASSERT_TRUE(Testing::WriteConnection(Material.Get(), Testing::ExpressionInput(Target, 0), FMaterialProgramLink{Id, Index}, true, false));
			const auto Connected = Testing::CaptureGraphView(*AuthoringGraph);
			EXPECT_EQ(FindViewNode(Connected, Target)->Inputs[0].SourceType,
				FindMaterialSampleOutput(Opcode, Index)->Type);
		}
		EXPECT_FALSE(Expression->GetOutput(static_cast<int32>(Expected.size())));
		const auto Before = CaptureExpressions(*Material);
		const auto Revision = Material->GetMaterialCompileDebugInfo().AuthoredRevision;
		for (const uint8 Index : {6, 8, 255})
		{
			EXPECT_FALSE(Testing::WriteConnection(Material.Get(), Testing::ExpressionInput(Target, 0), FMaterialProgramLink{Id, Index}, true, false));
			EXPECT_FALSE(Testing::WriteConnection(Material.Get(), EMaterialProperty::MP_BaseColor, FMaterialProgramLink{Id, Index}, true, false));
		}
		if (!bParameter)
			EXPECT_FALSE(Testing::WriteConnection(Material.Get(), Testing::ExpressionInput(Target, 0), FMaterialProgramLink{Id, 7}, true, false));
		EXPECT_EQ(CaptureExpressions(*Material), Before);
		EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision);
	}
}

#if DURIN_MATERIAL_GRAPH_WATER_TESTS
TEST(FMaterialGraphOperationsTests, GerstnerWavesCreatesFromReflectionWithoutAnOpcode)
{
	InitializeDObjectSystem();
	using Op = EMaterialProgramOpcode;
	using Type = EMaterialProgramValueType;
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
	const auto* Class = DMaterialExpressionGerstnerWaves::StaticClass();
	const auto* Description = FindMaterialExpressionDescription(Class);
	ASSERT_NE(Description, nullptr);
	EXPECT_FALSE(Description->SemanticOpcode);
	ASSERT_EQ(Description->Shapes.size(), 1u);
	EXPECT_EQ(Description->Shapes.front().ResultType, Type::Float3);

	const auto Rows = SearchCatalog("gerstner waves");
	ASSERT_EQ(Rows.size(), 1u);
	EXPECT_EQ(Rows.front().ExpressionClass, Class);
	EXPECT_FALSE(Rows.front().Opcode);
	EXPECT_TRUE(Rows.front().AcceptedInputTypes.empty());
	const auto Menu = Graph->GetSchema().GetGraphContextActions(*Graph);
	const auto Id = MakeCreationAction(Rows.front()).Id;
	const auto Action = std::ranges::find(Menu, Id, &FMaterialGraphCreationAction::Id);
	ASSERT_NE(Action, Menu.end());
	const auto Created = Action->PerformAction(*Graph, 0, 0, nullptr, false);
	ASSERT_TRUE(Created) << FormatMaterialGraphCommandResult(Created);
	const auto* Waves = (*Created)->GetMaterialExpression();
	ASSERT_NE(Waves, nullptr);
	const auto WavesId = Waves->Id;
	EXPECT_EQ(Waves->GetClass(), Class);
	EXPECT_FALSE((*Created)->GetView().Node.Opcode);
	ASSERT_EQ((*Created)->GetView().Outputs.size(), 2u);

	auto Forged = Rows.front();
	Forged.Opcode = Op::Constant;
	EXPECT_FALSE(Testing::CreateGraphCatalogNode(*Graph, Forged, 0, 0, {}, false));
	const auto Add = Testing::CreateGraphCatalogNode(*Graph, Op::Add, Type::Float);
	ASSERT_TRUE(Add);
	const auto Target = Testing::ExpressionInput(Add->front(), 0);
	const FMaterialProgramLink TangentNormal{WavesId, 1};
	const auto* Pin = Testing::ResolvePin(*Graph, TangentNormal);
	ASSERT_NE(Pin, nullptr);
	EXPECT_EQ(Pin->SourceIndex, 1);
	const auto& Output = Pin->GetOwningNode()->GetView().Outputs[Pin->DisplayIndex];
	EXPECT_EQ(Output.Name, "Tangent Normal");
	EXPECT_EQ(Output.Type, Type::Float3);
	EXPECT_FALSE(Output.PortId.IsValid());

	ASSERT_TRUE(Testing::WriteConnection(Material.Get(), Target, TangentNormal, false, false));
	const auto* Input = Testing::InputView(*Graph, Target);
	ASSERT_NE(Input, nullptr);
	EXPECT_EQ(Input->Link, TangentNormal);
	EXPECT_EQ(Input->SourceType, Type::Float3);
	const auto* Expression = Testing::FindExpression(Material.Get(), Add->front());
	ASSERT_NE(Expression, nullptr);
	EXPECT_EQ(Expression->GetOutputValueType(0), Type::Float3);
	EXPECT_FALSE(Testing::WriteConnection(Material.Get(), Target, {WavesId, 2}, true, false));

	const auto Normal = EMaterialProperty::MP_Normal;
	ASSERT_TRUE(Testing::TryCreateConnection(*Graph, TangentNormal, Normal, false, false));
	EXPECT_EQ(Material->GetExpressionInputForProperty(Normal)->OutputIndex, 1);
}
#endif

TEST(FMaterialGraphOperationsTests, DeclarationAndFloat4ReferenceUndoTogether)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "DeclarationTransaction");
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	FMaterialParameterDefinition Definition;
	Definition.Id = FGuid::NewGuid();
	Definition.Name = "LayerTint";
	Definition.Type = EMaterialParameterType::Vector4;
	Definition.Value = FMaterialParameterValue::MakeVector4({0.1, 0.2, 0.3, 0.4});
	auto Parameter = Testing::MakeGraphExpression<DMaterialExpressionVector4Parameter>();
	Parameter->Metadata.Id = Definition.Id;
	Parameter->Metadata.Name = Definition.Name;
	Parameter->DefaultValue = Definition.Value.GetVector4();
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	const auto Original = CaptureExpressions(*Material);
	const uint64 Revision = Material->GetMaterialCompileDebugInfo().AuthoredRevision;
	ASSERT_TRUE(AuthoringGraph->CreateExpression(*Parameter, 0, 0));
	const auto Program = CaptureExpressions(*Material);
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision + 1);
	ASSERT_NE(Material->FindParameterDefinition(Definition.Id), nullptr);
	EXPECT_EQ(Material->FindParameterDefinition(Definition.Id)->Value.GetVector4(),
		Definition.Value.GetVector4());
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->FindParameterDefinition(Definition.Id), nullptr);
	EXPECT_EQ(CaptureExpressions(*Material), Original);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(CaptureExpressions(*Material), Program);
	ASSERT_NE(Material->FindParameterDefinition(Definition.Id), nullptr);
	EXPECT_EQ(Material->FindParameterDefinition(Definition.Id)->Type, EMaterialParameterType::Vector4);
	const auto NoChange = AuthoringGraph->ReplaceExpression(*Parameter);
	EXPECT_EQ(NoChange.GetStatus(), EMaterialGraphCommandStatus::NoChange);
	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, PresentationWritesReportChangesAndPreserveSemanticState)
{
	InitializeDObjectSystem();
	DMaterial* Material = MakeExpandedGraphMaterial("PresentationResults");
	ASSERT_NE(Material, nullptr);
	const auto ProgramRevision = Material->GetMaterialProgramRevision();
	const auto AuthoredRevision = Material->GetMaterialCompileDebugInfo().AuthoredRevision;
	const auto Definitions = Material->GetParameterDefinitions();
	const std::vector<FMaterialParameterDefinition> BeforeDefinitions(Definitions.begin(), Definitions.end());
	int Notifications = 0;
	const auto Observer = Material->GetGraphChanges().Subscribe(*Material,
		[&](const FMaterialGraphChangeSet&) { ++Notifications; });

	FMaterialGraphPresentation Presentation = Material->GetMaterialGraphPresentation();
	ASSERT_FALSE(Presentation.Nodes.empty());
	auto& Output = Presentation.Nodes.front();
	Output.X += 64;
	const auto Position = Output;
	EXPECT_EQ(Material->SetMaterialGraphPresentation(Presentation), EMaterialGraphPresentationResult::Changed);
	EXPECT_EQ(Notifications, 1);
	EXPECT_EQ(Material->SetMaterialGraphPresentation(Presentation), EMaterialGraphPresentationResult::NoChange);
	EXPECT_EQ(Material->ApplyMaterialGraphNodePositions({&Position, 1}, AuthoredRevision),
		EMaterialGraphPresentationResult::NoChange);
	EXPECT_EQ(Notifications, 1);
	auto Unsanitized = Presentation;
	Unsanitized.Nodes.push_back({FGuid::NewGuid(), 10, 20});
	EXPECT_EQ(Material->SetMaterialGraphPresentation(Unsanitized), EMaterialGraphPresentationResult::NoChange);
	EXPECT_EQ(Material->ApplyMaterialGraphNodePositions({}, AuthoredRevision),
		EMaterialGraphPresentationResult::NoChange);
	EXPECT_EQ(Notifications, 1);

	auto Moved = Position;
	Moved.X += 32;
	EXPECT_EQ(Material->ApplyMaterialGraphNodePositions({&Moved, 1}, AuthoredRevision),
		EMaterialGraphPresentationResult::Changed);
	EXPECT_EQ(Notifications, 2);
	const auto AfterMove = Material->GetMaterialGraphPresentation();
	EXPECT_EQ(Material->ApplyMaterialGraphNodePositions({&Position, 1}, AuthoredRevision + 1),
		EMaterialGraphPresentationResult::Rejected);
	const std::array Duplicate{Position, Position};
	EXPECT_EQ(Material->ApplyMaterialGraphNodePositions(Duplicate, AuthoredRevision),
		EMaterialGraphPresentationResult::Rejected);
	auto Unknown = Position;
	Unknown.NodeId = FGuid::NewGuid();
	const std::array InvalidBatch{Position, Unknown};
	EXPECT_EQ(Material->ApplyMaterialGraphNodePositions(InvalidBatch, AuthoredRevision),
		EMaterialGraphPresentationResult::Rejected);
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), AfterMove);
	EXPECT_EQ(Notifications, 2);
	EXPECT_EQ(Material->GetMaterialProgramRevision(), ProgramRevision);
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, AuthoredRevision);
	EXPECT_TRUE(std::ranges::equal(Material->GetParameterDefinitions(), BeforeDefinitions));
	Material->GetGraphChanges().Unsubscribe(Observer);
	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
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
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 0u);
	EXPECT_TRUE(Material->GetParameterDefinitions().empty());
	EXPECT_TRUE(DMaterialGraph::Create(*Material)->GetRootNode() != nullptr);
	EXPECT_EQ(ReadMaterialNumericDefault(*Material->GetNumericInputForProperty(EMaterialProperty::MP_BaseColor), EMaterialProperty::MP_BaseColor),
		(std::vector<float>{0.f, 0.f, 0.f}));

	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests,
	MakeSurfaceCatalogAndAggregateCommandsAreAtomic)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "AggregateSurfaceCommands");
	ASSERT_NE(Material, nullptr);
	// This exercises authored graph commands and normalization, not shader publication.
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto Catalog = EnumerateCatalog();
	const auto Entry = std::ranges::find(Catalog,
		EMaterialProgramOpcode::MakeMaterialAttributes,
		[](const FMaterialGraphCatalogEntry& Value) {
			return Value.Opcode;
		});
	ASSERT_NE(Entry, Catalog.end());
	EXPECT_EQ(Entry->ResultType, EMaterialProgramValueType::MaterialAttributes);
	EXPECT_TRUE(std::ranges::none_of(Catalog,
		[](const FMaterialGraphCatalogEntry& Value) {
			return Value.Opcode
				== static_cast<EMaterialProgramOpcode>(30)
				|| Value.Opcode
					== static_cast<EMaterialProgramOpcode>(3);
		}));
	auto AggregateGraph = Testing::MakePBRMaterialExpressionsForTest();
	auto Surface = Testing::MakeGraphExpression<DMaterialExpressionMakeMaterialAttributes>();
	const auto& Outputs = AggregateGraph.Outputs;
	for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions())
		*Surface->GetExpressionInputForProperty(Field.Property) = Outputs.Get(Field.Property).Connection;
	AggregateGraph.Expressions.emplace_back(Surface.Get());
	AggregateGraph.Outputs = {};
	AggregateGraph.Outputs.Attributes = {Surface.Get()}; AggregateGraph.Outputs.bUseMaterialAttributes = true;
	ASSERT_TRUE(AggregateGraph.Apply(*Material));
	const FGuid SurfaceId = Material->GetExpressionInputForProperty(EMaterialProperty::MP_MaterialAttributes)->GetExpressionId();
	ASSERT_TRUE(SurfaceId.IsValid());
	FMaterialGraphPresentation AggregatePresentation;
	AggregatePresentation.Nodes.push_back({SurfaceId, 100, 100});
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(AggregatePresentation) != Durin::EMaterialGraphPresentationResult::Rejected);
	ASSERT_TRUE(Testing::WriteConnection(&*Material, EMaterialProperty::MP_MaterialAttributes, FMaterialProgramLink{}, true, false));
	ASSERT_TRUE(Testing::WriteConnection(&*Material, EMaterialProperty::MP_MaterialAttributes, FMaterialProgramLink{SurfaceId}, true, false));
	EXPECT_EQ(Material->GetExpressionInputForProperty(EMaterialProperty::MP_MaterialAttributes)->GetExpressionId(),
		SurfaceId);
	EXPECT_FALSE(Material->GetNumericInputForProperty(EMaterialProperty::MP_BaseColor)->Connection.IsConnected());
	const auto Normalized = Normalize(*Material);
	ASSERT_TRUE(Normalized);
	EXPECT_FALSE(Normalized.IR.bAggregateAttributes);
	// Both authoring modes normalize to final per-property output roots.
	EXPECT_TRUE(Normalized.IR.GetAttribute(EMaterialProperty::MP_BaseColor).bExpression);
	FMaterialGraphClipboardPayload Payload;
	TStrongObjectPtr<DMaterialGraph> MaterialEditorGraph(DMaterialGraph::Create(*Material));
	ASSERT_TRUE(MaterialEditorGraph->CopySelection(std::array{SurfaceId}, Payload));
	EXPECT_TRUE(Payload.AggregateSurface.IsConnected());
	ASSERT_TRUE(MaterialEditorGraph->Paste(Payload, 300, 100, false));
	EXPECT_NE(Material->GetExpressionInputForProperty(EMaterialProperty::MP_MaterialAttributes)->GetExpressionId(),
		SurfaceId);
	ASSERT_TRUE(Testing::WriteConnection(&*Material, EMaterialProperty::MP_MaterialAttributes, FMaterialProgramLink{}, true, false));
	EXPECT_FALSE(Material->GetExpressionInputForProperty(EMaterialProperty::MP_MaterialAttributes)->IsConnected());
	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, PresentationSanitizationIsIndependentAndBounded)
{
	const std::array Ids{FGuid::NewGuid(), FGuid::NewGuid()};
	FMaterialGraphPresentation Presentation;
	Presentation.SchemaVersion = 99;
	Presentation.Nodes = {
		{Ids[1], 20, 40},
		{Ids[1], 60, 80},
		{FGuid::NewGuid(), 10, 10},
		{Ids[0], MaterialGraphPresentationCoordinateLimit + 1, 0},
	};

	const FMaterialGraphPresentation Sanitized =
		SanitizeMaterialGraphPresentation(Presentation, Ids);
	EXPECT_EQ(Sanitized.SchemaVersion,
		CurrentMaterialGraphPresentationSchemaVersion);
	ASSERT_EQ(Sanitized.Nodes.size(), 1u);
	EXPECT_EQ(Sanitized.Nodes.front().NodeId, Ids[1]);
	EXPECT_EQ(Sanitized.Nodes.front().X, 20);
	EXPECT_EQ(Sanitized.Nodes.front().Y, 40);
}

TEST(FMaterialGraphOperationsTests, PresentationReachesMaximumNodeBoundAndDuplicatesByReflection)
{
	std::vector<FGuid> MaximumIds;
	FMaterialGraphPresentation MaximumPresentation;
	for (uint32 Index = 0; Index < MaterialProgramMaxNodeCount; ++Index)
	{
		const FGuid Id(Index + 1, 0, 0, 1);
		MaximumIds.push_back(Id);
		MaximumPresentation.Nodes.push_back(
			{Id, static_cast<int32>(Index * 10), static_cast<int32>(Index * -5)});
	}
	const FMaterialGraphPresentation Sanitized =
		SanitizeMaterialGraphPresentation(MaximumPresentation, MaximumIds);
	EXPECT_EQ(Sanitized.Nodes.size(), MaterialProgramMaxNodeCount);

	InitializeDObjectSystem();
	DMaterial* Material = MakeExpandedGraphMaterial("PresentationSource");
	ASSERT_NE(Material, nullptr);
	Material->PostLoad();
	const FGuid NodeId = Material->GetExpressionCollection().Expressions.front()->Id;
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(
		{.Nodes = {{NodeId, 100, -200}}}) != Durin::EMaterialGraphPresentationResult::Rejected);
	Material->SetRootPosition(420, -30);
	DMaterial* Duplicate = Cast<DMaterial>(DuplicateObject(
		Material, nullptr, "PresentationDuplicate").value());
	ASSERT_NE(Duplicate, nullptr);
	EXPECT_EQ(Duplicate->GetMaterialGraphPresentation(),
		Material->GetMaterialGraphPresentation());

	MarkAsGarbage(Duplicate);
	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, GroupedConfigurationAndOrdinaryOwnerEditsReplayInBothOrders)
{
	InitializeDObjectSystem();
	for (const bool bConfigurationFirst : {false, true})
	{
		SCOPED_TRACE(bConfigurationFirst);
		TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, bConfigurationFirst ? "ConfigFirst" : "RenameFirst", EObjectFlags::Transactional));
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		const auto OriginalName = Material->GetFName();
		const auto OriginalProperties = Material->GetStaticProperties();
		const auto OriginalRoughness = Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->Capture();
		FMaterialConfiguration Configuration{Material->GetDomain(), OriginalProperties};
		Configuration.Properties.ShadingModel = EMaterialShadingModel::Unlit;
		TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
		Tests::FTestEditorTransactorOwner History;
		const auto Pin = EMaterialProperty::MP_Roughness;
		const FName EditedName(bConfigurationFirst ? "RenamedAfterConfiguration" : "RenamedBeforeConfiguration");
		{
			FScopedTransaction Scope("Grouped material edit");
			if (bConfigurationFirst) ASSERT_TRUE(MaterialEditing::SetConfiguration(*Material, Configuration));
			ASSERT_TRUE(Material->Modify(false));
			Material->Rename(EditedName);
			ASSERT_TRUE(Graph->GetRootNode()->PinDefaultValueChanged(Testing::ResolvePin(*Graph->GetRootNode(), Pin), {.Kind = EMaterialInputDefaultKind::Literal, .Literal = {.X = .375f}}));
			if (!bConfigurationFirst) ASSERT_TRUE(MaterialEditing::SetConfiguration(*Material, Configuration));
		}
		ASSERT_EQ(History->GetQueueLength(), 1u);
		EXPECT_EQ(History->GetTransaction(0)->GetRecordCount(), 1u);
		const auto Revision = Material->GetMaterialCompileDebugInfo().AuthoredRevision;
		ASSERT_TRUE(History->Undo());
		EXPECT_EQ(Material->GetFName(), OriginalName);
		EXPECT_EQ(Material->GetStaticProperties(), OriginalProperties);
		EXPECT_EQ(*Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness), OriginalRoughness);
		EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision + 1);
		ASSERT_TRUE(History->Redo());
		EXPECT_EQ(Material->GetFName(), EditedName);
		EXPECT_EQ(Material->GetStaticProperties(), Configuration.Properties);
		EXPECT_FLOAT_EQ(Testing::InputView(*Graph, Pin)->InlineDefault.Literal.X, .375f);
		EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision + 2);
	}
}

TEST(FMaterialGraphOperationsTests, RejectedConfigurationPreservesTransactionStartedEdit)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "RejectedConfiguration"));
	TStrongObjectPtr<DMaterial> Other(NewObject<DMaterial>(nullptr, "OtherMaterial", EObjectFlags::Transactional));
	const auto OriginalName = Other->GetFName();
	const auto OriginalDomain = Material->GetDomain();
	FMaterialConfiguration Configuration{EMaterialDomain::PostProcess, {.ShadingModel = EMaterialShadingModel::Unlit}};
	Tests::FTestEditorTransactorOwner History;
	const auto Listener = History->OnTransactionStateChanged().AddLambda(
		[&](const FTransactionContext&, ETransactionStateEventType Event) {
			if (Event != ETransactionStateEventType::TransactionStarted) return;
			Other->Modify(false);
			Other->Rename("EditedMaterial");
			Configuration.Properties.bIsSky = true; // Sky is incompatible with PostProcess.
		});
	const auto Result = MaterialEditing::SetConfiguration(*Material, Configuration);
	History->OnTransactionStateChanged().Remove(Listener);
	EXPECT_FALSE(Result);
	EXPECT_EQ(Material->GetDomain(), OriginalDomain);
	EXPECT_EQ(Other->GetFName(), FName("EditedMaterial"));
	ASSERT_EQ(History->GetQueueLength(), 1u);
	ASSERT_TRUE(History->Undo());
	EXPECT_EQ(Other->GetFName(), OriginalName);
}

TEST(FMaterialGraphOperationsTests, AnonymousConfigurationRestoresDomainAndSchedulesOneGenerationPerReplay)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const FMaterialConfiguration Original{Material->GetDomain(), Material->GetStaticProperties()};
	const FMaterialConfiguration Configuration{EMaterialDomain::PostProcess, {.ShadingModel = EMaterialShadingModel::Unlit}};
	const auto Revision = Material->GetMaterialCompileDebugInfo().AuthoredRevision;
	Tests::FTestEditorTransactorOwner History;
	ASSERT_TRUE(MaterialEditing::SetConfiguration(*Material, Configuration));
	ASSERT_EQ(History->GetQueueLength(), 1u);
	EXPECT_EQ(History->GetTransaction(0)->GetRecordCount(), 1u);
	EXPECT_EQ(Material->GetDomain(), Configuration.Domain);
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision + 1);
	ASSERT_TRUE(History->Undo());
	EXPECT_EQ(Material->GetDomain(), Original.Domain);
	EXPECT_EQ(Material->GetStaticProperties(), Original.Properties);
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision + 2);
	ASSERT_TRUE(History->Redo());
	EXPECT_EQ(Material->GetDomain(), Configuration.Domain);
	EXPECT_EQ(Material->GetStaticProperties(), Configuration.Properties);
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision + 3);
}

TEST(FMaterialGraphOperationsTests, MaterialOutputMovementIsPresentationOnlyAndTransactional)
{
	InitializeDObjectSystem();
	DMaterial* Material = MakeExpandedGraphMaterial("MaterialOutputMovement");
	ASSERT_NE(Material, nullptr);
	const uint64 Revision = Material->GetMaterialCompileDebugInfo().AuthoredRevision;
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	const FMaterialGraphPresentation OriginalPresentation =
		Material->GetMaterialGraphPresentation();
	const auto OriginalRoot = Material->GetRootPosition();

	TStrongObjectPtr<DMaterialGraph> EditorGraph(DMaterialGraph::Create(*Material));
	{
		FScopedTransaction Scope("Grouped graph layout");
		ASSERT_TRUE(EditorGraph->MoveNodes(std::array{FMaterialGraphNodePresentation{EditorGraph->GetRootNode()->GetView().Node.Id, 520, -80}}));
	}
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision);
	EXPECT_TRUE(DMaterialGraph::Create(*Material)->GetRootNode() != nullptr);
	EXPECT_EQ(Material->GetRootPosition().first, 520);
	EXPECT_EQ(Material->GetRootPosition().second, -80);
	FMaterialGraphPresentation UnrelatedPresentation =
		Material->GetMaterialGraphPresentation();
	ASSERT_FALSE(UnrelatedPresentation.Nodes.empty());
	auto Unrelated = std::ranges::find_if(UnrelatedPresentation.Nodes, [&](const auto& P) { return P.NodeId != EditorGraph->GetRootNode()->GetView().Node.Id; });
	ASSERT_NE(Unrelated, UnrelatedPresentation.Nodes.end());
	Unrelated->X += 37;
	const FMaterialGraphNodePresentation UnrelatedPosition = *Unrelated;
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(UnrelatedPresentation) != Durin::EMaterialGraphPresentationResult::Rejected);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->GetRootPosition().first,
		OriginalRoot.first);
	const auto CurrentPresentation = Material->GetMaterialGraphPresentation();
	const auto PreservedNode = std::ranges::find(
		CurrentPresentation.Nodes,
		UnrelatedPosition.NodeId, &FMaterialGraphNodePresentation::NodeId);
	ASSERT_NE(PreservedNode, CurrentPresentation.Nodes.end());
	EXPECT_EQ(*PreservedNode, UnrelatedPosition);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Material->GetRootPosition().first, 520);
	Transactions->Reset();
	FMaterialGraphMoveSession Move;
	ASSERT_TRUE(Move.Begin(*EditorGraph, std::array{EditorGraph->GetRootNode()->GetView().Node.Id}, Transactions.Get()));
	ASSERT_TRUE(Move.Apply(std::array{FMaterialGraphNodePresentation{EditorGraph->GetRootNode()->GetView().Node.Id, 600, 40}}));
	Move.Cancel();
	EXPECT_EQ(Material->GetRootPosition().first, 520);
	EXPECT_FALSE(Transactions->CanUndo());
	ASSERT_TRUE(Move.Begin(*EditorGraph, std::array{EditorGraph->GetRootNode()->GetView().Node.Id}, Transactions.Get()));
	ASSERT_TRUE(Move.Apply(std::array{FMaterialGraphNodePresentation{EditorGraph->GetRootNode()->GetView().Node.Id, 600, 40}}));
	ASSERT_TRUE(Move.Commit());
	EXPECT_EQ(Material->GetRootPosition().first, 600);
	EXPECT_TRUE(Transactions->CanUndo());
	Transactions->Reset();
	const auto Pin = EMaterialProperty::MP_Roughness;
	ASSERT_TRUE(EditorGraph->GetRootNode()->PinDefaultValueChanged(Testing::ResolvePin(*EditorGraph->GetRootNode(), Pin), {.Kind = EMaterialInputDefaultKind::Literal, .Literal = {.X = .3125f}}));
	RecordProperty("RootSnapshotHistoryBytes", std::to_string(Transactions->GetUndoSize()));
	RecordProperty("ExpressionCount", std::to_string(Material->GetExpressionCollection().Expressions.size()));
	Transactions->Reset();
	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, UVSearchAliasesResolveToOneFixedWidthExpression)
{
	InitializeDObjectSystem();
	const auto Catalog = EnumerateCatalog();
	EXPECT_EQ(std::ranges::count(Catalog, EMaterialProgramOpcode::UVChannel,
		&FMaterialGraphCatalogEntry::Opcode), 0);
	for (const auto Query : {"texture coordinates", "UV Channel", "UVChannel"})
	{
		const auto Matches = SearchCatalog(Catalog, Query);
		ASSERT_EQ(Matches.size(), 1u) << Query;
		EXPECT_EQ(Matches.front().Opcode, EMaterialProgramOpcode::TextureCoordinates);
		EXPECT_EQ(Matches.front().ResultType, EMaterialProgramValueType::Float2);
		EXPECT_EQ(Matches.front().ExpressionClass, DMaterialExpressionTextureCoordinates::StaticClass());
	}
}

TEST(FMaterialGraphOperationsTests, ExpressionMenuPreservesCategoriesAndExcludesHiddenAndAssetBoundNodes)
{
	InitializeDObjectSystem();
	const auto Rows = SearchCatalog("");
	for (const auto& Row : Rows)
	{
		const auto* Description = FindMaterialExpressionDescription(Row.ExpressionClass);
		ASSERT_NE(Description, nullptr);
		EXPECT_EQ(MakeCreationAction(Row).Category, Description->Category) << Row.OperationName;
	}
	for (const auto Opcode : {EMaterialProgramOpcode::PrimitiveCustomData,
		EMaterialProgramOpcode::InstanceCustomData, EMaterialProgramOpcode::DecodeNormalRG})
		EXPECT_FALSE(std::ranges::contains(Rows, Opcode, &FMaterialGraphCatalogEntry::Opcode));
	const auto Catalog = EnumerateCatalog();
	for (auto* Class : {DMaterialExpressionPrimitiveCustomData::StaticClass(), DMaterialExpressionInstanceCustomData::StaticClass()})
	{
		const auto* Description = FindMaterialExpressionDescription(Class);
		ASSERT_NE(Description, nullptr);
		EXPECT_FALSE(Description->bRegisterForEditorCreation);
		EXPECT_FALSE(std::ranges::contains(Catalog, Class, &FMaterialGraphCatalogEntry::ExpressionClass));
		TStrongObjectPtr<DMaterialExpression> Expression(NewObject<DMaterialExpression>(Class, nullptr, NAME_None));
		ASSERT_TRUE(Expression);
		EXPECT_EQ(Expression->GetCreationInfo().Opcode, Description->SemanticOpcode);
		auto* Type = Expression->GetAuthoredResultType();
		ASSERT_NE(Type, nullptr);
		*Type = EMaterialProgramValueType::Float3;
		EXPECT_EQ(Expression->GetOutputValueType(0), EMaterialProgramValueType::Float3);
		EXPECT_FALSE(Expression->GetOutput(1));
		EXPECT_EQ(Expression->GetInput(0), nullptr);
	}
	for (const auto* Class : {DMaterialExpressionCollectionParameter::StaticClass(),
		DMaterialExpressionFunctionCall::StaticClass(), DMaterialExpressionFunctionInput::StaticClass(),
		DMaterialExpressionFunctionOutput::StaticClass()})
		EXPECT_FALSE(std::ranges::contains(Rows, Class, &FMaterialGraphCatalogEntry::ExpressionClass));

	const auto Matches = SearchCatalog("MuLtIpLy");
	ASSERT_FALSE(Matches.empty());
	EXPECT_EQ(Matches.front().OperationName, "Multiply");
	EXPECT_EQ(MakeCreationAction(Matches.front()).Category, "Math");
}

TEST(FMaterialGraphOperationsTests, ParameterAndChannelPaletteGroupsWidths)
{
	InitializeDObjectSystem();
	using Op = EMaterialProgramOpcode;
	using Type = EMaterialProgramValueType;
	const auto Rows = SearchCatalog("");
	EXPECT_EQ(std::ranges::count(Rows, Op::Parameter, &FMaterialGraphCatalogEntry::Opcode), 2);
	for (const auto Opcode : {Op::AppendVector, Op::Swizzle})
		EXPECT_EQ(std::ranges::count(Rows, Opcode, &FMaterialGraphCatalogEntry::Opcode), 1);
	for (const auto Opcode : {Op::MakeFloat2, Op::MakeFloat3, Op::MakeFloat4, Op::Splat2, Op::Splat3, Op::Splat4})
		EXPECT_EQ(std::ranges::count(Rows, Opcode, &FMaterialGraphCatalogEntry::Opcode), 0);
	const auto Mask = SearchCatalog("component mask", Type::Float4);
	ASSERT_EQ(Mask.size(), 1u);
	EXPECT_EQ(Mask.front().Opcode, Op::Swizzle);
	EXPECT_EQ(SearchCatalog("truncate", Type::Float3).size(), 1u);
	const auto Channels = SearchCatalog("channels", Type::Texture2D);
	const auto Swizzle = std::ranges::find(Channels, Op::Swizzle, &FMaterialGraphCatalogEntry::Opcode);
	ASSERT_NE(Swizzle, Channels.end());
	const FMaterialSchema Schema;
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	EXPECT_FALSE(Schema.CanCreate(*Material, MakeCreationAction(*Swizzle), Type::Texture2D));
	const auto Samples = SearchCatalog("sample", Type::Texture2D);
	const auto Sample = std::ranges::find(Samples, Op::TextureSample2D, &FMaterialGraphCatalogEntry::Opcode);
	ASSERT_NE(Sample, Samples.end());
	EXPECT_TRUE(Schema.CanCreate(*Material, MakeCreationAction(*Sample), Type::Texture2D));
}

TEST(FMaterialGraphOperationsTests, NodeCreationPolicyPreservesHostMenusAndOptionalSwitchInputs)
{
	InitializeDObjectSystem();
	using Op = EMaterialProgramOpcode;
	using Type = EMaterialProgramValueType;
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialGraph> MaterialGraph(DMaterialGraph::Create(*Material));
	TStrongObjectPtr<DMaterialGraph> FunctionGraph(DMaterialGraph::Create(*Function));
	const auto MenuEntry = [](const FMaterialGraphCreationAction& Action) -> std::optional<Op> {
		const auto* Entry = std::get_if<FMaterialGraphCatalogEntry>(&Action.Payload);
		return Entry ? Entry->Opcode : std::nullopt;
	};
	const auto MaterialMenu = MaterialGraph->GetSchema().GetGraphContextActions(*MaterialGraph);
	const auto FunctionMenu = FunctionGraph->GetSchema().GetGraphContextActions(*FunctionGraph);
	EXPECT_TRUE(std::ranges::contains(MaterialMenu, Op::TextureSampleParameter2D, MenuEntry));
	EXPECT_FALSE(std::ranges::contains(FunctionMenu, Op::TextureSampleParameter2D, MenuEntry));
	EXPECT_TRUE(std::ranges::contains(FunctionMenu, Op::TextureSample2D, MenuEntry));

	const auto Parameter = Testing::MakeGraphExpression<DMaterialExpressionTextureSampleParameter2D>();
	const auto FunctionInput = Testing::MakeGraphExpression<DMaterialExpressionFunctionInput>();
	EXPECT_TRUE(Parameter->IsAllowedIn(Material.Get()));
	EXPECT_FALSE(Parameter->IsAllowedIn(Function.Get()));
	EXPECT_FALSE(FunctionInput->IsAllowedIn(Material.Get()));
	EXPECT_TRUE(FunctionInput->IsAllowedIn(Function.Get()));
	EXPECT_FALSE(FunctionGraph->CreateExpression(*Parameter, 0, 0, false));
	EXPECT_FALSE(MaterialGraph->CreateExpression(*FunctionInput, 0, 0, false));

	for (auto* Graph : {MaterialGraph.Get(), FunctionGraph.Get()})
	{
		const auto& Menu = Graph == MaterialGraph.Get() ? MaterialMenu : FunctionMenu;
		for (const auto Opcode : {Op::QualitySwitch, Op::FeatureLevelSwitch})
		{
			const auto Action = std::ranges::find(Menu, Opcode, MenuEntry);
			ASSERT_NE(Action, Menu.end());
			const auto Created = Action->PerformAction(*Graph, 0, 0, nullptr, false);
			ASSERT_TRUE(Created) << FormatMaterialGraphCommandResult(Created);
			const auto* Expression = (*Created)->GetMaterialExpression();
			ASSERT_NE(Expression, nullptr);
			const auto& Inputs = (*Created)->GetView().Inputs;
			ASSERT_EQ(Inputs.size(), Opcode == Op::QualitySwitch ? 3u : 4u);
			EXPECT_EQ(Inputs.front().Name, "Default");
			EXPECT_EQ(Inputs.front().InlineDefault.Kind, EMaterialInputDefaultKind::Literal);
			EXPECT_EQ(Inputs.front().InlineDefault.Type, Type::Float);
			EXPECT_FLOAT_EQ(Inputs.front().InlineDefault.Literal.X, 0.f);
			for (const auto& Input : Inputs)
			{
				EXPECT_FALSE(Expression->IsInputConnectionRequired(static_cast<int32>(Input.InputIndex)));
				EXPECT_FALSE(Input.Link.SourceNodeId.IsValid());
			}
		}
	}
}

TEST(FMaterialGraphOperationsTests, AppendInfersWidthsAndKeepsUndoableOverflowDiagnostics)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, NAME_None);
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	using Op = EMaterialProgramOpcode;
	using Type = EMaterialProgramValueType;
	const auto Vector = Testing::CreateGraphCatalogNode(*AuthoringGraph, Op::Parameter, Type::Float4);
	ASSERT_TRUE(Vector);
	const auto Mask = Testing::CreateGraphCatalogNode(*AuthoringGraph, Op::Swizzle, Type::Float2, {Vector->front()});
	ASSERT_TRUE(Mask);
	const auto MaskId = Mask->front();
	const auto Append = Testing::CreateGraphCatalogNode(*AuthoringGraph, Op::AppendVector, Type::Float2, {MaskId});
	ASSERT_TRUE(Append) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Append);
	const auto Id = Append->front();
	EXPECT_EQ(AuthoringGraph->FindNode(Id)->GetView().Node.ResultType, Type::Float3);
	const auto Before = CaptureExpressions(*Material);
	ASSERT_TRUE(AuthoringGraph->FindNode(MaskId)->SetSwizzleComponents(std::array<uint8, 3>{0, 1, 2}));
	EXPECT_EQ(AuthoringGraph->FindNode(Id)->GetView().Node.ResultType, Type::Float4);
	const auto After = CaptureExpressions(*Material);
	ASSERT_TRUE(AuthoringGraph->FindNode(MaskId)->SetSwizzleComponents(std::array<uint8, 4>{0, 1, 2, 3}));
	EXPECT_TRUE(HasGraphDiagnostics(*Material));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), After);
	EXPECT_EQ(AuthoringGraph->FindNode(Id)->GetView().Node.ResultType, Type::Float4);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_EQ(AuthoringGraph->FindNode(Id)->GetView().Node.ResultType, Type::Float3);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(CaptureExpressions(*Material), After);
	EXPECT_EQ(AuthoringGraph->FindNode(Id)->GetView().Node.ResultType, Type::Float4);
}

TEST(FMaterialGraphOperationsTests, PromoteOrdinaryAndFunctionInputsReplaysConnectionsAndCompileRefresh)
{
	InitializeDObjectSystem();
	using Type = EMaterialProgramValueType;
	constexpr auto Setup = false;
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "PromoteInputMaterial"));
	TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
	TStrongObjectPtr<DMaterialFunction> Callee(NewObject<DMaterialFunction>(nullptr, "PromoteInputFunction"));
	TStrongObjectPtr<DMaterialGraph> CalleeGraph(DMaterialGraph::Create(*Callee));
	const FMaterialFunctionPort Port{.Id = FGuid::NewGuid(), .Type = Type::Float3, .Name = "Tint",
		.Default = {.Kind = EMaterialFunctionDefaultKind::Numeric, .Numeric = {.X = .25f, .Y = .5f, .Z = .75f}}};
	ASSERT_TRUE(CalleeGraph->AddPort(false, Port, {}, 0, 0, Setup));
	const auto CallResult = Graph->InsertFunctionCall(*Callee, 0, 0, Setup).transform(Testing::GraphNodeId);
	ASSERT_TRUE(CallResult) << FormatMaterialGraphCommandResult(CallResult);
	auto* Call = Cast<DMaterialExpressionFunctionCall>(Graph->FindNode(CallResult.value())->GetMaterialExpression());
	ASSERT_TRUE(Call);
	auto Draft = Testing::MakeGraphExpression<DMaterialExpressionAdd>();
	Draft->A.SetConstant({.625f});
	const auto Added = Graph->CreateExpression(*Draft, 0, 0, Setup).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Added);
	auto* Add = Cast<DMaterialExpressionAdd>(Graph->FindNode(Added.value())->GetMaterialExpression());
	ASSERT_TRUE(Add);

	for (bool IsCall : {false, true})
	{
		SCOPED_TRACE(IsCall ? "function call" : "ordinary expression");
		const auto Address = IsCall ? Testing::ExpressionInput(Call->Id, 0, Port.Id)
			: Testing::ExpressionInput(Add->Id, 0);
		const auto Before = CaptureExpressions(*Material);
		const auto Revision = Material->GetMaterialCompileDebugInfo().AuthoredRevision;
		const auto Requests = Material->GetMaterialCompileDebugInfo().RequestId;
		Durin::Tests::FTestEditorTransactorOwner History;
		const auto Promotion = Graph->PromoteInputToParameter(Testing::ResolvePin(*Graph, Address)).transform(Testing::NodeIds);
		ASSERT_TRUE(Promotion) << FormatMaterialGraphCommandResult(Promotion);
		ASSERT_EQ(Promotion->size(), IsCall ? 2u : 1u);

		const auto* PromotedParameter = FindExpression<DMaterialExpressionParameter>(*Material, Promotion->front());
		ASSERT_NE(PromotedParameter, nullptr);
		const auto* Definition = Material->FindParameterDefinition(PromotedParameter->Metadata.Id);
		ASSERT_TRUE(Definition);
		if (IsCall) EXPECT_EQ(Definition->Value.GetVector4(), FVector4(.25, .5, .75, 0));
		else EXPECT_FLOAT_EQ(Definition->Value.GetScalar(), .625f);
		const auto CheckConnection = [&]() {
			const auto* Pin = Testing::ResolvePin(*Graph, Address);
			ASSERT_TRUE(Pin);
			ASSERT_EQ(Pin->LinkedTo.size(), 1u);
			const auto* Output = Pin->LinkedTo.front();
			EXPECT_EQ(Output->SourceIndex, 0);
			auto* Source = Output->GetOwningNode()->GetMaterialExpression();
			EXPECT_EQ(Source->Id, Promotion->back());
			if (IsCall)
			{
				const auto Binding = std::ranges::find(Call->FunctionInputs, Port.Id, &FMaterialExpressionFunctionInputBinding::InputId);
				ASSERT_NE(Binding, Call->FunctionInputs.end());
				EXPECT_EQ(Binding->Input, (FMaterialExpressionInput{Source, 0}));
				auto* Mask = Cast<DMaterialExpressionSwizzle>(Source);
				ASSERT_TRUE(Mask);
				EXPECT_EQ(Mask->Components, (std::vector<uint8>{0, 1, 2}));
				ASSERT_TRUE(Mask->Input.Connection.IsConnected());
				EXPECT_EQ(Mask->Input.Connection.Expression->Id, Promotion->front());
			}
			else EXPECT_EQ(Add->A.Connection, (FMaterialExpressionInput{Source, 0}));
		};
		CheckConnection();
		EXPECT_FALSE(Graph->PromoteInputToParameter(Testing::ResolvePin(*Graph, Address)));
		EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision + 1);
		EXPECT_EQ(Material->GetMaterialCompileDebugInfo().RequestId, Requests + 1);
		ASSERT_TRUE(History->Undo());
		EXPECT_EQ(CaptureExpressions(*Material), Before);
		EXPECT_FALSE(History->CanUndo());
		if (IsCall) EXPECT_TRUE(std::ranges::none_of(Call->FunctionInputs, [](const auto& Binding) { return Binding.Input.IsConnected() || !Binding.InputDefault.empty(); }));
		EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision + 2);
		EXPECT_EQ(Material->GetMaterialCompileDebugInfo().RequestId, Requests + 2);
		ASSERT_TRUE(History->Redo());
		CheckConnection();
		EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision + 3);
		EXPECT_EQ(Material->GetMaterialCompileDebugInfo().RequestId, Requests + 3);
	}
}

TEST(FMaterialGraphOperationsTests, CreationAndReplacementReplayConnectedExpressionsOnBothOwners)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterialFunction> Callee(NewObject<DMaterialFunction>(nullptr, "ReplayCallee"));
	TStrongObjectPtr<DMaterialGraph> CalleeGraph(DMaterialGraph::Create(*Callee));
	FMaterialFunctionPort Port{.Id = FGuid::NewGuid(), .Type = EMaterialProgramValueType::Float, .Name = "Weight"};
	ASSERT_TRUE(CalleeGraph->AddPort(true, Port, {}, 0, 0, false));
	for (bool FunctionOwner : {false, true})
	{
		SCOPED_TRACE(FunctionOwner ? "function" : "material");
		TStrongObjectPtr<DObject> Owner(FunctionOwner ? static_cast<DObject*>(NewObject<DMaterialFunction>(nullptr, "ReplayFunction"))
			: static_cast<DObject*>(NewObject<DMaterial>(nullptr, "ReplayMaterial")));
		TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Owner));
		Durin::Tests::FTestEditorTransactorOwner Transactions;
		const auto Call = Graph->InsertFunctionCall(*Callee, 0, 0).transform(Testing::GraphNodeId);
		ASSERT_TRUE(Call) << FormatMaterialGraphCommandResult(Call);
		auto* CallNode = Graph->FindNode(Call.value());
		ASSERT_TRUE(CallNode);
		auto* FunctionCall = Cast<DMaterialExpressionFunctionCall>(CallNode->GetMaterialExpression());
		ASSERT_TRUE(FunctionCall);
		ASSERT_EQ(FunctionCall->FunctionOutputs.size(), 2u);
		ASSERT_EQ(FunctionCall->FunctionOutputs[1].OutputId, Port.Id);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_FALSE(Graph->FindNode(Call.value()));
		ASSERT_TRUE(Transactions->Redo());
		const auto MakeConsumer = [&](FMaterialProgramLink Source) {
			return Testing::CreateGraphCatalogNode(*Graph, EMaterialProgramOpcode::Saturate,
				EMaterialProgramValueType::Float, Source, 240, 80, true);
		};
		const auto Middle = MakeConsumer({FunctionCall->Id, 0, Port.Id});
		ASSERT_TRUE(Middle) << FormatMaterialGraphCommandResult(Middle);
		const auto MiddleId = Middle->front();
		auto* Original = Cast<DMaterialExpressionSaturate>(Graph->FindNode(MiddleId)->GetMaterialExpression());
		ASSERT_TRUE(Original);
		const FMaterialExpressionInput SecondOutput{FunctionCall, 1};
		EXPECT_EQ(Original->Input.Connection, SecondOutput);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_FALSE(Graph->FindNode(MiddleId));
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(Original->Input.Connection, SecondOutput);
		const auto Sink = MakeConsumer({MiddleId});
		ASSERT_TRUE(Sink);
		auto* Downstream = Cast<DMaterialExpressionSaturate>(Graph->FindNode(Sink->front())->GetMaterialExpression());
		ASSERT_TRUE(Downstream);
		auto Add = Testing::MakeGraphExpression<DMaterialExpressionAdd>(MiddleId);
		Add->A.Connection = SecondOutput;
		ASSERT_TRUE(Graph->ReplaceExpression(*Add));
		auto* Replacement = Cast<DMaterialExpressionAdd>(Graph->FindNode(MiddleId)->GetMaterialExpression());
		ASSERT_TRUE(Replacement);
		EXPECT_EQ(Downstream->Input.Connection.Expression.Get(), Replacement);
		EXPECT_EQ(Replacement->A.Connection, SecondOutput);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(Downstream->Input.Connection.Expression.Get(), Original);
		EXPECT_EQ(Original->Input.Connection, SecondOutput);
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(Downstream->Input.Connection.Expression.Get(), Replacement);
		EXPECT_EQ(Replacement->A.Connection, SecondOutput);
	}
}

TEST(FMaterialGraphOperationsTests, FunctionNodeEditsRestoreDownstreamWidthsInOneHistoryStep)
{
	InitializeDObjectSystem();
	using Op = EMaterialProgramOpcode;
	using Type = EMaterialProgramValueType;
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Function));
	const auto Constant = Testing::CreateGraphConstant(*AuthoringGraph, .25f);
	ASSERT_TRUE(Constant);
	const auto Add = Testing::CreateGraphCatalogNode(*AuthoringGraph, Op::Add, Type::Float, {Constant.value()});
	ASSERT_TRUE(Add);
	const auto Mask = Testing::CreateGraphCatalogNode(*AuthoringGraph, Op::Swizzle, Type::Float, {Add->front()});
	ASSERT_TRUE(Mask);
	const auto Append = Testing::CreateGraphCatalogNode(*AuthoringGraph, Op::AppendVector, Type::Float2, {Mask->front()});
	ASSERT_TRUE(Append);
	TStrongObjectPtr<DMaterialGraphNode> ConstantNode(AuthoringGraph->FindNode(Constant.value()));
	TStrongObjectPtr<DMaterialGraphNode> MaskNode(AuthoringGraph->FindNode(Mask->front()));
	ASSERT_TRUE(ConstantNode && MaskNode);
	const auto FirstOutput = FMaterialProgramLink{Add->front()};
	const auto LastOutput = FMaterialProgramLink{Append->front()};
	const auto Widths = [&](Type First, Type Last) {
		const auto* A = AuthoringGraph->FindOutputPin(FirstOutput);
		const auto* B = AuthoringGraph->FindOutputPin(LastOutput);
		ASSERT_TRUE(A && B);
		EXPECT_EQ(A->Type, First);
		EXPECT_EQ(B->Type, Last);
	};
	Tests::FTestEditorTransactorOwner History;
	const auto Vector = FMaterialParameterValue::MakeVector({.25f, .5f, .75f});
	ASSERT_TRUE(ConstantNode->SetConstantValue(Vector));
	Widths(Type::Float3, Type::Float2);
	EXPECT_EQ(ConstantNode->SetConstantValue(Vector).GetStatus(), EMaterialGraphCommandStatus::NoChange);
	const std::array<uint8, 2> Channels{2, 0};
	{
		FScopedTransaction Outer("Grouped swizzle");
		ASSERT_TRUE(MaskNode->SetSwizzleComponents(Channels));
		EXPECT_FALSE(MaskNode->SetSwizzleComponents(std::array<uint8, 1>{4}));
		EXPECT_EQ(MaskNode->SetSwizzleComponents(Channels).GetStatus(), EMaterialGraphCommandStatus::NoChange);
	}
	ASSERT_EQ(History->GetQueueLength(), 2u);
	EXPECT_EQ(History->GetTransaction(1)->GetRecordCount(), 2u);
	Widths(Type::Float3, Type::Float3);
	ASSERT_TRUE(History->Undo());
	Widths(Type::Float3, Type::Float2);
	ASSERT_TRUE(History->Undo());
	Widths(Type::Float, Type::Float2);
	ASSERT_TRUE(History->Redo());
	Widths(Type::Float3, Type::Float2);
	ASSERT_TRUE(History->Redo());
	Widths(Type::Float3, Type::Float3);
	EXPECT_EQ(Cast<DMaterialExpressionSwizzle>(MaskNode->GetMaterialExpression())->Components, (std::vector<uint8>{2, 0}));

	AuthoringGraph->RebuildGraph();
	EXPECT_FALSE(ConstantNode->SetConstantValue(FMaterialParameterValue::MakeScalar(.5f)));
	EXPECT_FALSE(MaskNode->SetSwizzleComponents(std::array<uint8, 1>{0}));
	EXPECT_EQ(History->GetQueueLength(), 2u);
	Widths(Type::Float3, Type::Float3);
}

TEST(FMaterialGraphOperationsTests, SwizzleWidthChangesRepeatChannelsAndUndo)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, NAME_None);
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	const auto Source = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float);
	ASSERT_TRUE(Source);
	const auto Swizzle = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::Swizzle,
		EMaterialProgramValueType::Float, {Source->front()});
	ASSERT_TRUE(Swizzle);
	const auto Id = Swizzle->front();
	const auto Before = CaptureExpressions(*Material);
	ASSERT_TRUE(AuthoringGraph->FindNode(Id)->SetSwizzleComponents(std::array<uint8, 4>{0, 0, 0, 0}));
	EXPECT_EQ(AuthoringGraph->FindNode(Id)->GetView().Node.ResultType, EMaterialProgramValueType::Float4);
	const auto After = CaptureExpressions(*Material);
	ASSERT_TRUE(AuthoringGraph->FindNode(Id)->SetSwizzleComponents(std::array<uint8, 2>{0, 1}));
	EXPECT_TRUE(HasGraphDiagnostics(*Material));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), After);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(CaptureExpressions(*Material), After);
}

TEST(FMaterialGraphOperationsTests, MathPaletteHasOneEntryPerOperationAndSourceWidth)
{
	InitializeDObjectSystem();
	using Type = EMaterialProgramValueType;
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	const auto Catalog = EnumerateCatalog();
	for (const auto Source : {std::optional<Type>{}, std::optional{Type::Float}, std::optional{Type::Float2},
		std::optional{Type::Float3}, std::optional{Type::Float4}, std::optional{Type::Texture2D}})
	{
		auto Rows = SearchCatalog(Catalog, {}, Source);
		const FMaterialSchema Schema;
		std::erase_if(Rows, [&](const auto& Entry) { return !Schema.CanCreate(*Material, MakeCreationAction(Entry), Source); });
		for (const auto& Entry : Catalog)
		{
			if (!Entry.IsAdaptiveNumeric()) continue;
			const bool bAllowed = Source != Type::Texture2D
				&& !(Source == Type::Float && (Entry.Opcode == EMaterialProgramOpcode::Normalize
					|| Entry.Opcode == EMaterialProgramOpcode::Reflect));
			EXPECT_EQ(std::ranges::count(Rows, Entry.Opcode, &FMaterialGraphCatalogEntry::Opcode), bAllowed ? 1 : 0);
		}
	}
	const auto Rows = SearchCatalog(Catalog, {});
	using Op = EMaterialProgramOpcode;
	const auto ExpectWidth = [&](Op Opcode, Type Width) {
		ASSERT_EQ(std::ranges::count(Rows, Opcode, &FMaterialGraphCatalogEntry::Opcode), 1);
		EXPECT_EQ(std::ranges::find(Rows, Opcode, &FMaterialGraphCatalogEntry::Opcode)->ResultType, Width);
	};
	ExpectWidth(Op::AppendVector, Type::Float2);
	ExpectWidth(Op::Swizzle, Type::Float);
	for (const auto Opcode : {Op::MakeFloat2, Op::MakeFloat3, Op::MakeFloat4, Op::Splat2, Op::Splat3, Op::Splat4})
	{
		EXPECT_TRUE(std::ranges::contains(Catalog, Opcode, &FMaterialGraphCatalogEntry::Opcode));
		EXPECT_FALSE(std::ranges::contains(Rows, Opcode, &FMaterialGraphCatalogEntry::Opcode));
	}
}

TEST(FMaterialGraphOperationsTests, DragFromVectorPinCreatesAndConnectsAdaptiveMath)
{
	InitializeDObjectSystem();
	using Type = EMaterialProgramValueType;
	using Op = EMaterialProgramOpcode;
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, NAME_None));
	for (DObject* Owner : {static_cast<DObject*>(Material.Get()), static_cast<DObject*>(Function.Get())})
	{
		TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Owner));
		for (const auto Width : {Type::Float2, Type::Float3})
		{
			const auto Source = Testing::CreateGraphCatalogNode(*Graph, Op::Constant, Width);
			ASSERT_TRUE(Source);
			const auto SourceId = Source->front();
			const auto Address = FMaterialProgramLink{SourceId};
			const auto Actions = Graph->GetSchema().GetGraphContextActions(*Graph, Testing::ResolvePin(*Graph, Address));
			for (const auto Opcode : {Op::Add, Op::Normalize})
			{
				const auto Matches = [&](const FMaterialGraphCreationAction& Action) {
					const auto* Entry = std::get_if<FMaterialGraphCatalogEntry>(&Action.Payload);
					return Entry && Entry->Opcode == Opcode;
				};
				ASSERT_EQ(std::ranges::count_if(Actions, Matches), 1);
				const auto Action = std::ranges::find_if(Actions, Matches);
				const auto Created = Action->PerformAction(*Graph, 120, 200, Testing::ResolvePin(*Graph, Address), false).transform(Testing::GraphNodeId);
				ASSERT_TRUE(Created) << FormatMaterialGraphCommandResult(Created);
				const auto* Node = Graph->FindNode(Created.value());
				ASSERT_NE(Node, nullptr);
				const auto* Input = Node->GetInputPin(0);
				const auto* Output = Graph->FindNode(SourceId)->GetOutputPin(0);
				ASSERT_NE(Input, nullptr);
				ASSERT_NE(Output, nullptr);
				ASSERT_EQ(Input->LinkedTo.size(), 1u);
				EXPECT_EQ(Input->LinkedTo.front(), Output);
				EXPECT_TRUE(std::ranges::contains(Output->LinkedTo, Input));
				EXPECT_EQ(Node->GetView().Inputs.front().SourceType, Width);
				EXPECT_EQ(Node->GetView().Outputs.front().Type, Width);
				if (Opcode == Op::Add)
				{
					const DMaterialExpression* Expression = Node->GetMaterialExpression();
					const auto* Add = Cast<DMaterialExpressionAdd>(Expression);
					ASSERT_NE(Add, nullptr);
					EXPECT_EQ(Expression->GetInput(0), &Add->A.Connection);
					EXPECT_EQ(Expression->GetInput(1), &Add->B.Connection);
					EXPECT_EQ(Expression->GetInput(2), nullptr);
					EXPECT_EQ(Expression->GetInputName(0), "A");
					EXPECT_EQ(Expression->GetInputName(1), "B");
					EXPECT_EQ(Expression->GetInputValueType(0), Width);
					EXPECT_EQ(Expression->GetOutputValueType(0), Width);
				}
			}
		}
	}
}

TEST(FMaterialGraphOperationsTests, MathWidthsPropagateBroadcastAndUndoAtomically)
{
	InitializeDObjectSystem();
	using Type = EMaterialProgramValueType;
	for (const auto Width : {Type::Float2, Type::Float3, Type::Float4})
	{
		auto* Material = NewObject<DMaterial>(nullptr, NAME_None);
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

		TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
		Durin::Tests::FTestEditorTransactorOwner Transactions;
		const auto Catalog = EnumerateCatalog();
		const auto Create = [&](EMaterialProgramOpcode Opcode, Type T) {
			const auto Entry = std::ranges::find_if(Catalog, [&](const auto& E) { return E.Opcode == Opcode && E.ResultType == T; });
			return Testing::CreateGraphCatalogNode(*AuthoringGraph, *Entry, 0, 0, {}, false);
		};
		const auto Vector = Create(EMaterialProgramOpcode::Constant, Width);
		const auto Scalar = Create(EMaterialProgramOpcode::Constant, Type::Float);
		const auto Other = Create(EMaterialProgramOpcode::Constant, Width == Type::Float2 ? Type::Float3 : Type::Float2);
		const auto Multiply = Create(EMaterialProgramOpcode::Multiply, Type::Float);
		const auto Add = Create(EMaterialProgramOpcode::Add, Type::Float);
		ASSERT_TRUE(Vector); ASSERT_TRUE(Scalar); ASSERT_TRUE(Other); ASSERT_TRUE(Multiply); ASSERT_TRUE(Add);
		const auto M = Multiply->front(), A = Add->front();
		ASSERT_TRUE(Testing::WriteConnection(Material, Testing::ExpressionInput(A, 0), FMaterialProgramLink{M}, false, false));
		ASSERT_TRUE(Testing::WriteConnection(Material, Testing::ExpressionInput(M, 1), FMaterialProgramLink{Scalar->front()}, false, false));
		const auto Before = CaptureExpressions(*Material);
		const auto Connect = [&](FGuid From, bool Replace, bool Mode = true) {
			return AuthoringGraph->GetSchema().TryCreateConnection(AuthoringGraph->FindNode(From)->GetOutputPin(0), AuthoringGraph->FindNode(M)->GetInputPin(0), Replace, Mode);
		};
		ASSERT_TRUE(Connect(Vector->front(), false));
		auto View = Testing::CaptureGraphView(*AuthoringGraph);
		EXPECT_EQ(FindViewNode(View, M)->Node.ResultType, Width);
		EXPECT_EQ(FindViewNode(View, A)->Node.ResultType, Width);
		std::vector<DMaterialExpression*> Expressions;
		for (const auto& E : Material->GetExpressionCollection().Expressions) Expressions.push_back(E.Get());
		const std::array Roots{FMaterialExpressionInput{FindMaterialExpression(Material->GetExpressionCollection().Expressions, A)}};
		const auto Built = MIR::BuildGraph(Expressions, Roots);
		ASSERT_TRUE(Built);
		EXPECT_EQ(Built.IR.Nodes[Built.Roots.front()].ResultType, Width);
		EXPECT_TRUE(std::ranges::any_of(Built.IR.Nodes, [&](const auto& N) {
			return N.ResultType == Width && N.Opcode >= EMaterialProgramOpcode::Splat2 && N.Opcode <= EMaterialProgramOpcode::Splat4;
		}));
		const auto After = CaptureExpressions(*Material);
		ASSERT_TRUE(Testing::WriteConnection(Material, Testing::ExpressionInput(M, 1), FMaterialProgramLink{Other->front()}, true));
		EXPECT_TRUE(HasGraphDiagnostics(*Material));
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(CaptureExpressions(*Material), After);
		ASSERT_TRUE(Transactions->Undo()); EXPECT_EQ(CaptureExpressions(*Material), Before);
		ASSERT_TRUE(Transactions->Redo()); EXPECT_EQ(CaptureExpressions(*Material), After);
		ASSERT_TRUE(Connect(Scalar->front(), true, false));
		View = Testing::CaptureGraphView(*AuthoringGraph);
		EXPECT_EQ(FindViewNode(View, M)->Node.ResultType, Type::Float);
		EXPECT_EQ(FindViewNode(View, A)->Node.ResultType, Type::Float);
		const auto MakeVector = Create(EMaterialProgramOpcode::MakeFloat2, Type::Float2);
		ASSERT_TRUE(MakeVector);
		ASSERT_TRUE(Testing::WriteConnection(Material, Testing::ExpressionInput(MakeVector->front(), 0), FMaterialProgramLink{A}, false, false));
		const auto Fixed = CaptureExpressions(*Material);
		ASSERT_TRUE(Connect(Vector->front(), true));
		EXPECT_FALSE(HasGraphDiagnostics(*Material));
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(CaptureExpressions(*Material), Fixed);
	}
}

TEST(FMaterialGraphOperationsTests, PromoteOpenMakePinUsesAttributeDefault)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
	const auto Make = Testing::CreateGraphCatalogNode(*Graph, EMaterialProgramOpcode::MakeMaterialAttributes,
		EMaterialProgramValueType::MaterialAttributes);
	ASSERT_TRUE(Make);
	const auto Pin = Testing::ExpressionInput(Make->front(), uint32(EMaterialProperty::MP_Roughness));
	ASSERT_TRUE(Testing::InputView(*Graph, Pin));
	EXPECT_FALSE(Testing::InputView(*Graph, Pin)->bSupportsConstant);
	const auto Promotion = Graph->PromoteInputToParameter(Testing::ResolvePin(*Graph, Pin), false).transform(Testing::NodeIds);
	ASSERT_TRUE(Promotion) << FormatMaterialGraphCommandResult(Promotion);
	const auto* Parameter = FindExpression<DMaterialExpressionParameter>(*Material, Promotion->front());
	ASSERT_TRUE(Parameter);
	const auto* Definition = Material->FindParameterDefinition(Parameter->Metadata.Id);
	ASSERT_TRUE(Definition);
	EXPECT_EQ(Definition->Value.GetScalar(), .5f);
}

TEST(FMaterialGraphOperationsTests, FixedInputsBroadcastScalarsAndUndoWithoutAuthoredConversionNodes)
{
	InitializeDObjectSystem();
	using Type = EMaterialProgramValueType;
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	const auto Catalog = EnumerateCatalog();
	const auto Create = [&](EMaterialProgramOpcode Opcode, Type T) {
		const auto Entry = std::ranges::find_if(Catalog, [&](const auto& E) { return E.Opcode == Opcode && E.ResultType == T; });
		return Testing::CreateGraphCatalogNode(*AuthoringGraph, *Entry, 0, 0, {}, false);
	};
	const auto Scalar = Create(EMaterialProgramOpcode::Constant, Type::Float);
	const auto Vector = Create(EMaterialProgramOpcode::Constant, Type::Float2);
	const auto Surface = Create(EMaterialProgramOpcode::MakeMaterialAttributes, Type::MaterialAttributes);
	ASSERT_TRUE(Scalar); ASSERT_TRUE(Vector); ASSERT_TRUE(Surface);
	const auto S = Scalar->front(), V = Vector->front(), M = Surface->front();
	const auto Before = CaptureExpressions(*Material);
	ASSERT_TRUE(Testing::WriteConnection(Material.Get(), Testing::ExpressionInput(M, 0), FMaterialProgramLink{S}, false));
	const auto After = CaptureExpressions(*Material);
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 3u);
	const auto View = Testing::CaptureGraphView(*AuthoringGraph);
	const auto& Types = FindViewNode(View, M)->Inputs[0].AcceptedTypes;
	EXPECT_NE(std::ranges::find(Types, Type::Float), Types.end());
	ASSERT_TRUE(Testing::WriteConnection(Material.Get(), Testing::ExpressionInput(M, 0), FMaterialProgramLink{V}, true));
	EXPECT_TRUE(HasGraphDiagnostics(*Material));
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Testing::WriteConnection(Material.Get(), Testing::ExpressionInput(M, 2), FMaterialProgramLink{V}, false));
	EXPECT_FALSE(HasGraphDiagnostics(*Material));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), After);
	ASSERT_TRUE(Transactions->Undo()); EXPECT_EQ(CaptureExpressions(*Material), Before);
	ASSERT_TRUE(Transactions->Redo()); EXPECT_EQ(CaptureExpressions(*Material), After);
	ASSERT_TRUE(Testing::WriteConnection(Material.Get(), EMaterialProperty::MP_BaseColor, FMaterialProgramLink{S}, true));
	EXPECT_EQ(Material->GetNumericInputForProperty(EMaterialProperty::MP_BaseColor)->Connection.GetExpressionId(), S);
	std::vector<DMaterialExpression*> Expressions;
	for (const auto& E : Material->GetExpressionCollection().Expressions) Expressions.push_back(E.Get());
	const auto Built = MIR::FGraphBuilder(Expressions).FinishSurface(Material->CaptureRootInputs());
	ASSERT_TRUE(Built);
	EXPECT_EQ(Built.IR.Nodes[Built.IR.GetAttribute(EMaterialProperty::MP_BaseColor).ExpressionIndex].Opcode, EMaterialProgramOpcode::Splat3);
	ASSERT_TRUE(Transactions->Undo()); EXPECT_FALSE(Material->GetNumericInputForProperty(EMaterialProperty::MP_BaseColor)->Connection.IsConnected());
	ASSERT_TRUE(Transactions->Redo()); EXPECT_EQ(Material->GetNumericInputForProperty(EMaterialProperty::MP_BaseColor)->Connection.GetExpressionId(), S);
}

TEST(FMaterialGraphOperationsTests, FunctionMathAdaptsAndReportsNormalizeAndLerpDiagnostics)
{
	InitializeDObjectSystem();
	using Type = EMaterialProgramValueType;
	auto* Function = NewObject<DMaterialFunction>(nullptr, NAME_None);

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Function));
	const auto Catalog = EnumerateCatalog();
	const auto Create = [&](EMaterialProgramOpcode Opcode, Type T, FMaterialProgramLink Source = {}) {
		const auto Entry = std::ranges::find_if(Catalog, [&](const auto& E) { return E.Opcode == Opcode && E.ResultType == T; });
		return Testing::CreateGraphCatalogNode(*AuthoringGraph, *Entry, 0, 0, Source, false);
	};
	const auto Vector = Create(EMaterialProgramOpcode::Constant, Type::Float3);
	const auto Scalar = Create(EMaterialProgramOpcode::Constant, Type::Float);
	ASSERT_TRUE(Vector); ASSERT_TRUE(Scalar);
	const auto V = Vector->front(), S = Scalar->front();
	const auto Normalize = Create(EMaterialProgramOpcode::Normalize, Type::Float2, {V});
	ASSERT_TRUE(Normalize) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Normalize);
	const auto N = Normalize->front();
	const auto Lerp = Create(EMaterialProgramOpcode::Lerp, Type::Float, {N});
	ASSERT_TRUE(Lerp) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Lerp);
	const auto L = Lerp->front();
	const auto View = Testing::CaptureGraphView(*AuthoringGraph);
	EXPECT_EQ(FindViewNode(View, N)->Node.ResultType, Type::Float3);
	EXPECT_EQ(FindViewNode(View, L)->Node.ResultType, Type::Float3);
	ASSERT_TRUE(Testing::WriteConnection(Function, Testing::ExpressionInput(N, 0), FMaterialProgramLink{S}, true, false));
	ASSERT_TRUE(Testing::WriteConnection(Function, Testing::ExpressionInput(L, 2), FMaterialProgramLink{V}, false, false));
	EXPECT_TRUE(HasGraphDiagnostics(*Function));
	ASSERT_TRUE(Testing::WriteConnection(Function, Testing::ExpressionInput(L, 2), FMaterialProgramLink{S}, true, false));
	ASSERT_TRUE(Testing::WriteConnection(Function, Testing::ExpressionInput(N, 0), FMaterialProgramLink{}, true, false));
	ASSERT_TRUE(Testing::WriteConnection(Function, Testing::ExpressionInput(N, 0), FMaterialProgramLink{V}, false, false));
}

namespace
{
	void ExpectCatalogNodeView(const FMaterialGraphCatalogEntry& Entry, DMaterial& Material,
		DMaterialGraph& AuthoringGraph, DMaterialExpression& Created)
	{
		const bool NarrowParameter = Entry.Opcode == EMaterialProgramOpcode::Parameter
			&& (Entry.ResultType == EMaterialProgramValueType::Float2 || Entry.ResultType == EMaterialProgramValueType::Float3);
		EXPECT_EQ(Created.GetClass(), NarrowParameter ? DMaterialExpressionSwizzle::StaticClass() : Entry.ExpressionClass);
		EXPECT_EQ(Created.GetOuter(), &Material);
		const auto View = Testing::CaptureGraphView(AuthoringGraph);
		EXPECT_EQ(View.Nodes.size(), Material.GetExpressionCollection().Expressions.size() + 1);
		for (const FMaterialGraphNodeView& Node : View.Nodes)
		{
			EXPECT_FALSE(Node.PrimaryLabel.empty());
			if (Node.Node.bMaterialOutput) { EXPECT_EQ(Node.Inputs.size(), FMaterialAttributeDefinitionMap::GetDefinitions().size() + 1); continue; }
			const auto Expression = std::ranges::find_if(Material.GetExpressionCollection().Expressions,
				[&](const auto& Value) { return Value->Id == Node.Node.Id; });
			ASSERT_NE(Expression, Material.GetExpressionCollection().Expressions.end());
			EXPECT_EQ(Node.Inputs.size(), (*Expression)->GetAuthoredInputCount());
			for (const FMaterialGraphPinView& Input : Node.Inputs)
			{
				EXPECT_FALSE(Input.Name.empty());
				EXPECT_FALSE(Input.AcceptedTypes.empty());
			}
		}
		const auto* Viewed = FindViewNode(View, Created.Id);
		ASSERT_NE(Viewed, nullptr);
		EXPECT_EQ(Viewed->Node.Opcode, NarrowParameter ? EMaterialProgramOpcode::Swizzle : Entry.Opcode);
		EXPECT_EQ(Viewed->Node.ResultType, Entry.Opcode == EMaterialProgramOpcode::AppendVector ? EMaterialProgramValueType::Float2 : Entry.ResultType);
		EXPECT_EQ(Viewed->Inputs.size(), Created.GetAuthoredInputCount());
		const auto Info = Created.GetCreationInfo();
		if (Info.Opcode && (Info.FixedOutputType || Cast<DMaterialExpressionNumeric>(&Created)))
		{
			EXPECT_EQ(Info.Opcode, Viewed->Node.Opcode);
			EXPECT_EQ(Created.GetOutputValueType(0), Viewed->Node.ResultType);
			EXPECT_FALSE(Created.GetOutput(1));
			for (const auto& Input : Viewed->Inputs)
			{
				const auto Index = static_cast<int32>(Input.InputIndex);
				EXPECT_EQ(Created.GetInputName(Index), Input.Name);
				EXPECT_NE(Created.GetInput(Index), nullptr);
				if (const auto Type = Created.GetInputValueType(Index))
					EXPECT_TRUE(std::ranges::contains(Input.AcceptedTypes, *Type));
			}
		}
		if (Entry.ExpressionClass == DMaterialExpressionVector3Constant::StaticClass())
		{
			EXPECT_EQ(Viewed->PrimaryLabel, "Constant");
			ASSERT_EQ(Viewed->Outputs.size(), 1u);
			EXPECT_EQ(Viewed->Outputs.front().Type, EMaterialProgramValueType::Float3);
		}
		VisitMaterialExpressionInputs(Created, [&](uint32 Index, FMaterialExpressionInput& Input) {
			ASSERT_LT(Index, Viewed->Inputs.size());
			const auto& Link = Viewed->Inputs[Index].Link;
			EXPECT_EQ(Link.SourceNodeId, Input.GetExpressionId());
			EXPECT_EQ(Link.SourceOutputId, Input.GetOutputId());
			if (!Link.SourceOutputId.IsValid()) EXPECT_EQ(Link.SourceOutputIndex, Input.OutputIndex);
			const auto* Numeric = FindMaterialNumericInput(Created, Input);
			if (!Numeric) return;
			EXPECT_EQ(Viewed->Inputs[Index].bUseConstant, Numeric->UseConstant);
			const auto& Retained = Viewed->Inputs[Index].RetainedConstant;
			const std::array Components{Retained.Literal.X, Retained.Literal.Y, Retained.Literal.Z, Retained.Literal.W};
			ASSERT_LE(Numeric->Constant.size(), Components.size());
			for (size_t C = 0; C < Numeric->Constant.size(); ++C) EXPECT_EQ(Numeric->Constant[C], Components[C]);
		});
	}
}

TEST(FMaterialGraphOperationsTests, CatalogShapesCreateExpectedNodesAndAgreeWithRuntimeValidation)
{
	InitializeDObjectSystem();
	using Type = EMaterialProgramValueType;
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	const auto Catalog = EnumerateCatalog();
	ASSERT_TRUE(std::ranges::contains(Catalog, DMaterialExpressionVector3Constant::StaticClass(),
		&FMaterialGraphCatalogEntry::ExpressionClass));
	// Build the immutable source expressions once for the pin compatibility matrix.
	Testing::FTestMaterialExpressionGraph Graph;
	FMaterialParameterDefinition Texture;
	Texture.Id = FGuid::NewGuid(); Texture.Name = "SignatureTexture";
	Texture.Type = EMaterialParameterType::Texture; Texture.Value = FMaterialParameterValue::MakeTexture(nullptr);
	auto Cube = Texture; Cube.Id = FGuid::NewGuid(); Cube.Name = "SignatureCube"; Cube.Value = FMaterialParameterValue::MakeTextureCube(nullptr);
	const std::array Definitions{Texture, Cube};
	std::function<FMaterialExpressionInput(Type)> AddSource = [&](Type ValueType) {
		if (ValueType == Type::Texture2D || ValueType == Type::TextureCube)
			return Testing::MakeLink(Graph.Add(EMaterialProgramOpcode::TextureParameter, ValueType, {}, ValueType == Type::TextureCube ? Cube.Id : Texture.Id, {}, Definitions));
		std::vector<FMaterialExpressionInput> Inputs;
			if (ValueType == Type::MaterialAttributes)
				for (uint32 Index = 0; Index < FMaterialAttributeDefinitionMap::GetDefinitions().size(); ++Index)
				{
					auto Source = AddSource(GetMaterialSurfaceOutputType(FMaterialAttributeDefinitionMap::GetDefinitions()[Index].Property));
					if (FMaterialAttributeDefinitionMap::GetDefinitions()[Index].Property == EMaterialProperty::MP_Normal)
					{
						auto& Annotation = Graph.Add(EMaterialProgramOpcode::Swizzle,
							Type::Float3, {Source}, {}, {});
						auto* Swizzle = Cast<DMaterialExpressionSwizzle>(&Annotation);
						Swizzle->Components = {0, 1, 2};
						Swizzle->OutputSpatialKind = EMaterialSpatialKind::Normal;
						Swizzle->OutputCoordinateSpace = EMaterialCoordinateSpace::Tangent;
						Source = {&Annotation};
					}
					Inputs.push_back(Source);
				}
		return Testing::MakeLink(Graph.Add(ValueType == Type::MaterialAttributes ? EMaterialProgramOpcode::MakeMaterialAttributes : EMaterialProgramOpcode::Constant,
			ValueType, std::move(Inputs), {}, ValueType == Type::Float3 ? FMaterialProgramLiteral{1,0,0} : FMaterialProgramLiteral{}));
	};
	std::array<FMaterialExpressionInput, static_cast<size_t>(Type::MaterialAttributes) + 1> Sources;
	std::array<std::vector<DMaterialExpression*>, static_cast<size_t>(Type::MaterialAttributes) + 1> SourceExpressions;
		for (uint32 I = 0; I < Sources.size(); ++I)
	{
		const auto First = Graph.Expressions.size();
		Sources[I] = AddSource(static_cast<Type>(I));
			for (size_t J = First; J < Graph.Expressions.size(); ++J) SourceExpressions[I].push_back(Graph.Expressions[J].Get());
		}
		const auto NormalFirst = Graph.Expressions.size();
		auto& NormalAnnotation = Graph.Add(EMaterialProgramOpcode::Swizzle, Type::Float3,
			{Sources[static_cast<size_t>(Type::Float3)]}, {}, {});
		auto* NormalSwizzle = Cast<DMaterialExpressionSwizzle>(&NormalAnnotation);
		NormalSwizzle->Components = {0, 1, 2};
		NormalSwizzle->OutputSpatialKind = EMaterialSpatialKind::Normal;
		NormalSwizzle->OutputCoordinateSpace = EMaterialCoordinateSpace::Tangent;
		const FMaterialExpressionInput NormalSource{&NormalAnnotation};
		std::vector<DMaterialExpression*> NormalExpressions;
		for (size_t J = NormalFirst; J < Graph.Expressions.size(); ++J)
			NormalExpressions.push_back(Graph.Expressions[J].Get());
	std::vector<DMaterialExpression*> Expressions;
	for (const auto& Entry : Catalog)
	{
		SCOPED_TRACE(std::format("{} ({})", Entry.OperationName, static_cast<uint8>(Entry.ResultType)));
		ASSERT_TRUE(Material->SetMaterialExpressions({}, {}));
		FMaterialProgramLink Prerequisite;
		std::optional<Type> FirstType;
		if (!Entry.AcceptedInputTypes.empty())
		{
			ASSERT_FALSE(Entry.AcceptedInputTypes.front().empty());
			FirstType = Entry.AcceptedInputTypes.front().front();
		}
		const bool bWaterOutput = Entry.Opcode == EMaterialProgramOpcode::SingleLayerWaterOutput;
		if (bWaterOutput)
		{
			const auto Blank = Testing::CreateGraphCatalogNode(*AuthoringGraph, Entry, 0, 0, {}, false);
			ASSERT_TRUE(Blank) << FormatMaterialGraphCommandResult(Blank);
			ASSERT_EQ(Blank->size(), 1u);
			const auto* Node = AuthoringGraph->FindNode(Blank->front());
			ASSERT_TRUE(Node);
			ASSERT_EQ(Node->GetView().Inputs.size(), Entry.AcceptedInputTypes.size());
			for (const auto& Input : Node->GetView().Inputs)
				EXPECT_FALSE(Input.Link.SourceNodeId.IsValid());
			ASSERT_TRUE(Material->SetMaterialExpressions({}, {}));
		}
		if (bWaterOutput || FirstType == Type::MaterialAttributes || FirstType == Type::Texture2D || FirstType == Type::TextureCube)
		{
			const auto Opcode = bWaterOutput ? EMaterialProgramOpcode::Constant
				: FirstType == Type::MaterialAttributes ? EMaterialProgramOpcode::MakeMaterialAttributes : EMaterialProgramOpcode::TextureParameter;
			const auto Found = std::ranges::find_if(Catalog, [&](const auto& Entry) { return Entry.Opcode == Opcode && Entry.ResultType == FirstType; });
			ASSERT_NE(Found, Catalog.end());
			const auto Created = Testing::CreateGraphCatalogNode(*AuthoringGraph, *Found, 0, 0, {}, false);
			ASSERT_TRUE(Created); Prerequisite = {Created->front()};
		}
		const auto Created = Testing::CreateGraphCatalogNode(*AuthoringGraph, Entry, 0, 0, Prerequisite, false);
		ASSERT_TRUE(Created) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Created);
		ASSERT_EQ(Created->size(), Entry.Opcode == EMaterialProgramOpcode::StaticSwitch ? 2u : 1u);
		const auto* Node = AuthoringGraph->FindNode(Created->front());
		ASSERT_NE(Node, nullptr);
		auto* Original = Node->GetMaterialExpression();
		ASSERT_NE(Original, nullptr);
		ASSERT_NO_FATAL_FAILURE(ExpectCatalogNodeView(Entry, *Material, *AuthoringGraph, *Original));
		if (bWaterOutput)
		{
			ASSERT_FALSE(Node->GetView().Inputs.empty());
			EXPECT_EQ(Node->GetView().Inputs.front().Link, Prerequisite);
			for (const auto& Input : Node->GetView().Inputs | std::views::drop(1))
				EXPECT_FALSE(Input.Link.SourceNodeId.IsValid());
		}
		if (Entry.AcceptedInputTypes.empty()) continue;
		// Static selectors have an authoring-only condition or optional exact
		// branches with Default fallback; their semantic matrix is covered by
		// dedicated selector tests rather than this all-pins-required shape test.
		if (Entry.Opcode == EMaterialProgramOpcode::StaticSwitch
			|| Entry.Opcode == EMaterialProgramOpcode::QualitySwitch
			|| Entry.Opcode == EMaterialProgramOpcode::FeatureLevelSwitch) continue;
		ASSERT_EQ(Original->GetAuthoredInputCount(), Entry.AcceptedInputTypes.size());
		TStrongObjectPtr<DMaterialExpression> Target(DuplicateObject(Original, nullptr, NAME_None).value());
		if (auto* Swizzle = Cast<DMaterialExpressionSwizzle>(Target.Get()))
			std::ranges::fill(Swizzle->Components, 0);
		for (uint32 Pin = 0; Pin < Entry.AcceptedInputTypes.size(); ++Pin)
			for (Type SourceType : {Type::Float, Type::Float2, Type::Float3, Type::Float4,
				Type::Texture2D, Type::TextureCube, Type::MaterialAttributes})
			{
				SCOPED_TRACE(std::format("{} result {} pin {} source {}", Entry.OperationName,
					static_cast<uint8>(Entry.ResultType), Pin, static_cast<uint8>(SourceType)));
					Expressions.clear();
					std::array<bool, static_cast<size_t>(Type::MaterialAttributes) + 1> Included{};
					bool bNormalIncluded = false;
			VisitMaterialExpressionInputs(*Target, [&](uint32 Index, FMaterialExpressionInput& Input) {
					const bool bEqualWidthGeometry = Entry.Opcode == EMaterialProgramOpcode::Dot
						|| Entry.Opcode == EMaterialProgramOpcode::Distance;
					const auto TypeIndex = static_cast<size_t>(Index == Pin || bEqualWidthGeometry
						? SourceType : Entry.AcceptedInputTypes[Index].front());
					const bool bNormalInput = Entry.Opcode == EMaterialProgramOpcode::BlendNormalsRNM
						|| (Entry.Opcode == EMaterialProgramOpcode::MakeMaterialAttributes
							&& Index == static_cast<uint32>(EMaterialProperty::MP_Normal));
					Input = bNormalInput && static_cast<Type>(TypeIndex) == Type::Float3
						? NormalSource : Sources[TypeIndex];
					if (bNormalInput && static_cast<Type>(TypeIndex) == Type::Float3)
					{
						if (!Included[TypeIndex])
						{
							Included[TypeIndex] = true;
							Expressions.insert(Expressions.end(), SourceExpressions[TypeIndex].begin(), SourceExpressions[TypeIndex].end());
						}
						if (!bNormalIncluded)
						{
							bNormalIncluded = true;
							Expressions.insert(Expressions.end(), NormalExpressions.begin(), NormalExpressions.end());
						}
					}
					else if (!Included[TypeIndex])
					{
						Included[TypeIndex] = true;
						Expressions.insert(Expressions.end(), SourceExpressions[TypeIndex].begin(), SourceExpressions[TypeIndex].end());
					}
				});
				const auto ActivateSampleOperand = [&](auto* Sample, uint32 FirstOptional) {
					if (Sample) Sample->ReadMode = Pin < FirstOptional ? EMaterialTextureReadMode::Implicit
						: Pin == FirstOptional ? EMaterialTextureReadMode::Level : EMaterialTextureReadMode::Grad;
				};
				ActivateSampleOperand(Cast<DMaterialExpressionTextureSampleCube>(Target.Get()), 2);
				ActivateSampleOperand(Cast<DMaterialExpressionTextureSampleParameterCube>(Target.Get()), 1);
				ActivateSampleOperand(Cast<DMaterialExpressionTextureSample2D>(Target.Get()), 2);
				ActivateSampleOperand(Cast<DMaterialExpressionTextureSampleParameter2D>(Target.Get()), 1);
				Expressions.push_back(Target.Get());
				const auto& Accepted = Entry.AcceptedInputTypes[Pin];
				const bool bSpatialTransform = Entry.Opcode == EMaterialProgramOpcode::TransformPosition
					|| Entry.Opcode == EMaterialProgramOpcode::TransformDirection
					|| Entry.Opcode == EMaterialProgramOpcode::TransformNormal;
				// Transform inputs are additionally constrained by spatial kind and source
				// coordinate space. The generic constants in this shape matrix are
				// intentionally non-spatial, so even Float3 must fail semantic validation.
				const bool bAccepted = std::ranges::find(Accepted, SourceType) != Accepted.end()
					&& !bSpatialTransform;
				const auto Validation = MIR::FGraphBuilder::ValidateSurface(Expressions, {});
				EXPECT_EQ(static_cast<bool>(Validation), bAccepted)
					<< (Validation.Diagnostics.empty() ? std::string{}
						: Validation.Diagnostics.front().Message);
			}
	}
}

TEST(FMaterialGraphOperationsTests, SignaturesRejectInvalidResultsAndKeepSwizzlePayloadValidation)
{
	InitializeDObjectSystem();
	using Type = EMaterialProgramValueType;
	auto Source = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
	for (DClass* Class : {DMaterialExpressionAdd::StaticClass(), DMaterialExpressionNegate::StaticClass(), DMaterialExpressionClamp::StaticClass()})
		for (Type ResultType : {Type::Texture2D, Type::MaterialAttributes})
		{
			TStrongObjectPtr<DMaterialExpression> Target(NewObject<DMaterialExpression>(Class, nullptr, NAME_None));
			Target->Id = FGuid::NewGuid();
			auto* Property = Class->FindPropertyByName("ResultType");
			ASSERT_NE(Property, nullptr);
			*static_cast<Type*>(Property->GetValuePtr(Target.Get())) = ResultType;
			VisitMaterialExpressionInputs(*Target, [&](uint32, FMaterialExpressionInput& Input) { Input = {Source.Get()}; });
			EXPECT_FALSE(MIR::FGraphBuilder::ValidateSurface(std::array<DMaterialExpression*, 2>{Source.Get(), Target.Get()}, {}));
		}
	EXPECT_FALSE(GetMaterialProgramNodeSignature(static_cast<EMaterialProgramOpcode>(3), Type::Float));
	EXPECT_FALSE(GetMaterialProgramNodeSignature(static_cast<EMaterialProgramOpcode>(255), Type::Float));
	EXPECT_FALSE(GetMaterialProgramNodeSignature(EMaterialProgramOpcode::Constant, static_cast<Type>(255)));
	EXPECT_FALSE(GetMaterialProgramNodeSignature(EMaterialProgramOpcode::Normalize, Type::Float));
	auto Swizzle = Testing::MakeGraphExpression<DMaterialExpressionSwizzle>();
	Swizzle->Input = {Source.Get()}; Swizzle->Components = {0};
	const std::array<DMaterialExpression*, 2> Expressions{Source.Get(), Swizzle.Get()};
	ASSERT_TRUE(MIR::FGraphBuilder::ValidateSurface(Expressions, {}));
	// A valid source must also belong to the graph being validated.
	EXPECT_FALSE(MIR::FGraphBuilder::ValidateSurface(std::array<DMaterialExpression*, 1>{Swizzle.Get()}, {}));
	Swizzle->Components = {1};
	EXPECT_FALSE(MIR::FGraphBuilder::ValidateSurface(Expressions, {}));
	Swizzle->Components = {};
	EXPECT_FALSE(MIR::FGraphBuilder::ValidateSurface(Expressions, {}));
}

TEST(FMaterialGraphOperationsTests, PaletteCreationAddsVisibleDefaultsInOneTransaction)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "PaletteCreationMaterial");
	ASSERT_NE(Material, nullptr);
	const std::vector<FMaterialGraphCatalogEntry> Catalog =
		SearchCatalog("multiply", EMaterialProgramValueType::Float3);
	const auto Multiply = std::ranges::find_if(Catalog,
		[](const FMaterialGraphCatalogEntry& Entry) {
			return Entry.OperationName == "Multiply"
				&& Entry.ResultType == EMaterialProgramValueType::Float3;
		});
	ASSERT_NE(Multiply, Catalog.end());

	const auto Before = CaptureExpressions(*Material);
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	const auto Created = Testing::CreateGraphCatalogNode(*DMaterialGraph::Create(*Material), *Multiply, 400, 200, {});
	ASSERT_TRUE(Created) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Created);
	ASSERT_EQ(Created->size(), 1u);
	TStrongObjectPtr<DMaterialGraph> InspectionGraph(DMaterialGraph::Create(*Material));
	const FMaterialGraphView View = Testing::CaptureGraphView(*InspectionGraph);
	const FMaterialGraphNodeView* Node = FindViewNode(View, Created->front());
	ASSERT_NE(Node, nullptr);
	ASSERT_EQ(Node->Inputs.size(), 2u);
	for (const auto& Pin : Node->Inputs)
	{
		EXPECT_FALSE(Pin.Link.SourceNodeId.IsValid());
		EXPECT_EQ(Pin.InlineDefault.Kind, EMaterialInputDefaultKind::Literal);
		EXPECT_EQ(Pin.InlineDefault.Type, EMaterialProgramValueType::Float3);
	}
	EXPECT_FLOAT_EQ(Node->Inputs[1].InlineDefault.Literal.X, 1.0f);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);

	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, StaticSwitchCreationOwnsUniqueDeclarationAndReplaysAtomically)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "StaticSwitchCreationMaterial");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	const auto Catalog = EnumerateCatalog();
	const auto Entry = std::ranges::find_if(Catalog, [](const auto& Candidate) {
		return Candidate.Opcode == EMaterialProgramOpcode::StaticSwitch
			&& Candidate.ResultType == EMaterialProgramValueType::Float;
	});
	ASSERT_NE(Entry, Catalog.end());
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	const auto Before = CaptureExpressions(*Material);
	{
		FScopedMaterialGraphChange Batch(*Material);
		std::vector<DMaterialGraphNode*> Auxiliary;
		const auto First = MakeCreationAction(*Entry).PerformAction(*AuthoringGraph, 400, 200, {}, true, &Auxiliary);
		ASSERT_TRUE(First) << FormatMaterialGraphCommandResult(First);
		ASSERT_EQ(Auxiliary.size(), 1u);
		EXPECT_EQ((*First)->GetView().Node.Opcode, EMaterialProgramOpcode::StaticSwitch);
		EXPECT_EQ(Auxiliary.front()->GetView().Node.Opcode, EMaterialProgramOpcode::StaticBool);
		const auto* Switch = (*First)->GetMaterialExpression();
		const auto* Declaration = Auxiliary.front()->GetMaterialExpression();
		EXPECT_EQ(Declaration->GetOutputValueType(0), EMaterialProgramValueType::StaticBool);
		EXPECT_FALSE(Declaration->GetOutput(1));
		EXPECT_EQ(Switch->GetInputName(0), "Condition");
		EXPECT_EQ(Switch->GetInputValueType(0), EMaterialProgramValueType::StaticBool);
		ASSERT_NE(Switch->GetInput(0), nullptr);
		EXPECT_EQ(Switch->GetInput(0)->Expression.Get(), Declaration);
		EXPECT_EQ(Switch->GetInput(0)->OutputIndex, 0);
	}
	const auto Second = Testing::CreateGraphCatalogNode(*AuthoringGraph, *Entry, 700, 200, {});
	ASSERT_TRUE(Second) << FormatMaterialGraphCommandResult(Second);
	ASSERT_EQ(Second->size(), 2u);
	std::vector<FName> DeclarationNames;
	std::vector<FGuid> DeclarationIds;
	for (const auto& Expression : Material->GetExpressionCollection().Expressions)
		if (const auto* Declaration = Cast<DMaterialExpressionStaticBool>(Expression.Get()))
		{
			DeclarationNames.push_back(Declaration->Name);
			DeclarationIds.push_back(Declaration->DeclarationId);
		}
	ASSERT_EQ(DeclarationNames.size(), 2u);
	EXPECT_NE(DeclarationNames[0], DeclarationNames[1]);
	EXPECT_TRUE(DeclarationIds[0].IsValid());
	EXPECT_TRUE(DeclarationIds[1].IsValid());
	EXPECT_NE(DeclarationIds[0], DeclarationIds[1]);
	const auto After = CaptureExpressions(*Material);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), Before.Expressions.size() + 2u);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(CaptureExpressions(*Material), After);
	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, CompactInputCommandsPreserveSharingFallbacksAndUndo)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "CompactInputCommands");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	const auto Created = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::Multiply, EMaterialProgramValueType::Float, {}, 400, 200, true);
	ASSERT_TRUE(Created) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Created);
	const auto Id = Created.value()[0];
	const auto Before = CaptureExpressions(*Material);
	const auto Extracted = AuthoringGraph->FindNode(Id)->ExtractInputDefault(Testing::ResolvePin(*AuthoringGraph->FindNode(Id), Testing::ExpressionInput(Id, 1))).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Extracted) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Extracted);
	const auto ExtractedId = Extracted.value();
	ASSERT_TRUE(Testing::WriteConnection(Material, Testing::ExpressionInput(Id, 0), FMaterialProgramLink{ExtractedId}, false));
	ASSERT_TRUE(AuthoringGraph->FindNode(Id)->InlineInputNode(Testing::ResolvePin(*AuthoringGraph->FindNode(Id), Testing::ExpressionInput(Id, 1))));
	EXPECT_TRUE(std::ranges::any_of(Material->GetExpressionCollection().Expressions, [&](const auto& Node) { return Node->Id == ExtractedId; }));
	ASSERT_TRUE(AuthoringGraph->FindNode(Id)->InlineInputNode(Testing::ResolvePin(*AuthoringGraph->FindNode(Id), Testing::ExpressionInput(Id, 0))));
	EXPECT_FALSE(std::ranges::any_of(Material->GetExpressionCollection().Expressions, [&](const auto& Node) { return Node->Id == ExtractedId; }));
	for (int Index = 0; Index < 4; ++Index) ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	for (int Index = 0; Index < 4; ++Index) ASSERT_TRUE(Transactions->Redo());
	const auto* Node = FindExpression<DMaterialExpressionMultiply>(*Material, Id);
	ASSERT_NE(Node, nullptr);
	EXPECT_EQ(Node->A.Constant, (std::vector<float>{1}));
	EXPECT_EQ(Node->B.Constant, (std::vector<float>{1}));
	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, RemovalAndInliningReplayDownstreamWidthsOnBothGraphOwners)
{
	using Type = EMaterialProgramValueType;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	for (DObject* Owner : std::array<DObject*, 2>{Material.Get(), Function.Get()})
	{
		SCOPED_TRACE(Owner == Material.Get() ? "material" : "material function");
		Durin::Tests::FTestEditorTransactorOwner Transactions;
		TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Owner));
		auto Vector = Testing::MakeGraphExpression<DMaterialExpressionVector2Constant>();
		Vector->Value = FVector2(2, 3);
		const auto Source = AuthoringGraph->CreateExpression(*Vector, 0, 0, false).transform(Testing::GraphNodeId);
		const auto Scalar = Testing::CreateGraphConstant(*AuthoringGraph, 4);
		ASSERT_TRUE(Source);
		ASSERT_TRUE(Scalar);
		const auto Add = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::Add, Type::Float, {Source.value()});
		ASSERT_TRUE(Add);
		const auto AddId = Add->front();
		const auto Multiply = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::Multiply, Type::Float, {AddId});
		ASSERT_TRUE(Multiply);
		const auto B = Testing::ExpressionInput(AddId, 1);
		ASSERT_TRUE(Testing::TryCreateConnection(*AuthoringGraph, B, FMaterialProgramLink{Scalar.value()},
			false, false));
		auto* Target = Cast<DMaterialExpressionAdd>(AuthoringGraph->FindNode(AddId)->GetMaterialExpression());
		auto* Downstream = Cast<DMaterialExpressionMultiply>(AuthoringGraph->FindNode(Multiply->front())->GetMaterialExpression());
		ASSERT_TRUE(Target && Downstream);
		auto Revision = [&] { return Owner == Material.Get() ? Material->GetMaterialCompileDebugInfo().AuthoredRevision : Function->GetFunctionRevision(); };
		auto PreviousRevision = Revision();
		const auto Check = [&](Type Width) {
			EXPECT_EQ(Target->ResultType, Width);
			EXPECT_EQ(Downstream->ResultType, Width);
			EXPECT_EQ(Downstream->A.Connection.Expression.Get(), Target);
			EXPECT_GT(Revision(), PreviousRevision);
			PreviousRevision = Revision();
		};
		auto* OriginalSource = Target->A.Connection.Expression.Get();
		ASSERT_NE(OriginalSource, nullptr);
		ASSERT_TRUE(AuthoringGraph->RemoveNodes(std::array{Source.value()}));
		EXPECT_FALSE(Target->A.Connection.IsConnected());
		Check(Type::Float);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(Target->A.Connection.Expression.Get(), OriginalSource);
		Check(Type::Float2);
		ASSERT_TRUE(Transactions->Redo());
		Check(Type::Float);
		auto* OriginalScalar = Target->B.Connection.Expression.Get();
		ASSERT_NE(OriginalScalar, nullptr);
		ASSERT_TRUE(AuthoringGraph->FindNode(AddId)->InlineInputNode(Testing::ResolvePin(*AuthoringGraph->FindNode(AddId), B)));
		EXPECT_FALSE(AuthoringGraph->FindNode(Scalar.value()));
		EXPECT_EQ(Target->B.Constant, (std::vector<float>{4}));
		Check(Type::Float);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(Target->B.Connection.Expression.Get(), OriginalScalar);
		Check(Type::Float);
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_FALSE(Target->B.Connection.IsConnected());
		Check(Type::Float);
		const auto Extracted = AuthoringGraph->FindNode(AddId)->ExtractInputDefault(Testing::ResolvePin(*AuthoringGraph->FindNode(AddId), B)).transform(Testing::GraphNodeId);
		ASSERT_TRUE(Extracted);

		const auto ConstantId = Extracted.value();
		auto* Constant = Target->B.Connection.Expression.Get();
		ASSERT_TRUE(AuthoringGraph->FindNode(ConstantId));
		EXPECT_EQ(AuthoringGraph->FindNode(ConstantId)->GetMaterialExpression(), Constant);
		const auto Position = AuthoringGraph->FindNode(ConstantId)->GetView().Presentation;
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_FALSE(AuthoringGraph->FindNode(ConstantId));
		EXPECT_FALSE(Target->B.Connection.IsConnected());
		EXPECT_EQ(Target->B.Constant, (std::vector<float>{4}));
		ASSERT_TRUE(Transactions->Redo());
		ASSERT_TRUE(AuthoringGraph->FindNode(ConstantId));
		EXPECT_EQ(AuthoringGraph->FindNode(ConstantId)->GetView().Presentation, Position);
		EXPECT_EQ(Target->B.Connection.Expression.Get(), Constant);
	}
}

TEST(FMaterialGraphOperationsTests, TypedInputDefaultsCoverWidthsCoordinatesAndFunctionBindings)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "TypedInputDefaults");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	for (uint32 Width = 1; Width <= 4; ++Width)
	{
		auto* Add = NewObject<DMaterialExpressionAdd>(nullptr, NAME_None);
		Add->Id = FGuid::NewGuid();
		Add->ResultType = static_cast<EMaterialProgramValueType>(Width - 1);
		Add->A.SetConstant(std::vector<float>(Width, 0.f));
		Add->B.SetConstant(std::vector<float>(Width, 1.f));
		const auto Id = Add->Id;
		ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 1>{Add}, {}));
		ASSERT_TRUE(Material->SetMaterialGraphPresentation({.Nodes = {{Id, 400, 100}}}) != Durin::EMaterialGraphPresentationResult::Rejected);
		const FMaterialInputDefault Value{.Kind = EMaterialInputDefaultKind::Literal,
			.Type = Add->ResultType, .Literal = {2, 3, 4, 5}};
		ASSERT_TRUE(Testing::WriteInputDefault(Material, Testing::ExpressionInput(Id, 1, {}), Value, false));
		EXPECT_EQ(Testing::WriteInputDefault(Material, Testing::ExpressionInput(Id, 1, {}), Value, false).GetStatus(), EMaterialGraphCommandStatus::NoChange);
		const auto Extracted = AuthoringGraph->FindNode(Id)->ExtractInputDefault(Testing::ResolvePin(*AuthoringGraph->FindNode(Id), Testing::ExpressionInput(Id, 1)), false);
		ASSERT_TRUE(Extracted) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Extracted);
		ASSERT_EQ(Material->GetExpressionCollection().Expressions.size(), 2u);
		EXPECT_EQ(Material->GetExpressionCollection().Expressions.back()->GetClass(),
			(std::array{DMaterialExpressionScalarConstant::StaticClass(), DMaterialExpressionVector2Constant::StaticClass(),
				DMaterialExpressionVector3Constant::StaticClass(), DMaterialExpressionVector4Constant::StaticClass()})[Width - 1]);
		ASSERT_TRUE(AuthoringGraph->FindNode(Id)->InlineInputNode(Testing::ResolvePin(*AuthoringGraph->FindNode(Id), Testing::ExpressionInput(Id, 1)), false));
		ASSERT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
		const auto* Result = Cast<DMaterialExpressionAdd>(Material->GetExpressionCollection().Expressions.front().Get());
		ASSERT_NE(Result, nullptr);
		ASSERT_EQ(Result->B.Constant.size(), Width);
		for (uint32 Index = 0; Index < Width; ++Index) EXPECT_FLOAT_EQ(Result->B.Constant[Index], 2.f + Index);
	}
	auto* Coordinates = NewObject<DMaterialExpressionTextureCoordinates>(nullptr, NAME_None);
	Coordinates->Id = FGuid::NewGuid();
	const auto CoordinatesId = Coordinates->Id;
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 1>{Coordinates}, {}));
	ASSERT_TRUE(Material->SetMaterialGraphPresentation({.Nodes = {{CoordinatesId, 400, 100}}}) != Durin::EMaterialGraphPresentationResult::Rejected);
	ASSERT_TRUE(Testing::WriteInputDefault(Material, Testing::ExpressionInput(CoordinatesId, 0, {}), {.Kind = EMaterialInputDefaultKind::Literal,
		.Type = EMaterialProgramValueType::Float3, .Literal = {2, 3, 4}}, false));
	EXPECT_TRUE(HasGraphDiagnostics(*Material));
	ASSERT_TRUE(Testing::WriteInputDefault(Material, Testing::ExpressionInput(CoordinatesId, 0, {}), {.Kind = EMaterialInputDefaultKind::Literal,
		.Type = EMaterialProgramValueType::Float, .Literal = {2}}));
	ASSERT_TRUE(AuthoringGraph->FindNode(CoordinatesId)->ExtractInputDefault(Testing::ResolvePin(*AuthoringGraph->FindNode(CoordinatesId), Testing::ExpressionInput(CoordinatesId, 0))));
	ASSERT_TRUE(AuthoringGraph->FindNode(CoordinatesId)->InlineInputNode(Testing::ResolvePin(*AuthoringGraph->FindNode(CoordinatesId), Testing::ExpressionInput(CoordinatesId, 0))));
	EXPECT_EQ(Cast<DMaterialExpressionTextureCoordinates>(Material->GetExpressionCollection().Expressions.front().Get())->Channel.Constant, (std::vector<float>{2}));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 2u);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);

	auto* Function = NewObject<DMaterialFunction>(nullptr, "TypedDefaultFunction");
	FMaterialFunctionSignature Signature;
	const auto InputId = FGuid::NewGuid(), OutputId = FGuid::NewGuid();
	Signature.Inputs.push_back({.Id = InputId, .Type = EMaterialProgramValueType::Float3, .Name = "Value",
		.Default = {.Kind = EMaterialFunctionDefaultKind::Numeric}});
	Signature.Outputs.push_back({.Id = OutputId, .Type = EMaterialProgramValueType::Float3, .Name = "Result"});
	auto* Input = NewObject<DMaterialExpressionFunctionInput>(nullptr, NAME_None);
	Input->Id = FGuid::NewGuid(); Input->Port.Id = InputId;
	auto* Output = NewObject<DMaterialExpressionFunctionOutput>(nullptr, NAME_None);
	Output->Id = FGuid::NewGuid(); Output->Port.Id = OutputId; Output->Source = {Input};
	ASSERT_TRUE(Function->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(Signature, std::array<DMaterialExpression*, 2>{Input, Output})));
	ASSERT_TRUE(Material->SetMaterialExpressions({}, {}));
	const auto Created = AuthoringGraph->InsertFunctionCall(*Function, 400, 100, false).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Created) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Created);
	const auto CallId = Created.value();
	ASSERT_TRUE(Testing::WriteInputDefault(Material, Testing::ExpressionInput(CallId, 0, InputId), {.Kind = EMaterialInputDefaultKind::Literal,
		.Type = EMaterialProgramValueType::Float3, .Literal = {4, 5, 6}}, false));
	Transactions->Reset();
	const auto FindCallInput = [&] { return Testing::ResolvePin(*AuthoringGraph, Testing::ExpressionInput(CallId, 0, InputId)); };
	const auto BeforeExtract = Material->GetMaterialCompileDebugInfo().AuthoredRevision;
	const auto Extracted = AuthoringGraph->FindNode(CallId)->ExtractInputDefault(FindCallInput()).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Extracted);
	EXPECT_EQ(Transactions->GetQueueLength(), 1u);
	EXPECT_GT(Material->GetMaterialCompileDebugInfo().AuthoredRevision, BeforeExtract);
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 2u);
	const auto BeforeInline = Material->GetMaterialCompileDebugInfo().AuthoredRevision;
	ASSERT_TRUE(AuthoringGraph->FindNode(CallId)->InlineInputNode(FindCallInput()));
	EXPECT_EQ(Transactions->GetQueueLength(), 2u);
	EXPECT_GT(Material->GetMaterialCompileDebugInfo().AuthoredRevision, BeforeInline);
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(AuthoringGraph->FindNode(*Extracted));
	EXPECT_EQ(FindCallInput()->LinkedTo.size(), 1u);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_FALSE(AuthoringGraph->FindNode(*Extracted));
	EXPECT_TRUE(FindCallInput()->LinkedTo.empty());
	const auto* Call = Cast<DMaterialExpressionFunctionCall>(Material->GetExpressionCollection().Expressions.front().Get());
	ASSERT_NE(Call, nullptr);
	ASSERT_EQ(Call->FunctionInputs.size(), 1u);
	EXPECT_EQ(Call->FunctionInputs.front().InputDefault, (std::vector<float>{4, 5, 6}));
	EXPECT_FALSE(Call->FunctionInputs.front().Input.IsConnected());
	MarkAsGarbage(Material);
	MarkAsGarbage(Function);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, SamplingClipboardRemapsSharedExplicitCoordinates)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "CompactSamplingCommands");
	auto* Target = NewObject<DMaterial>(nullptr, "CompactSamplingTarget");

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	const auto Created = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::TextureSampleParameter2D, EMaterialProgramValueType::Float4, {}, 400, 0, true);
	ASSERT_TRUE(Created) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Created);
	const auto Id = Created.value()[0];
	const auto CreatedUV = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::TextureCoordinates, EMaterialProgramValueType::Float2, {}, 0, 0, true);
	ASSERT_TRUE(CreatedUV) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(CreatedUV);
	const auto CoordinatesId = CreatedUV.value()[0];
	ASSERT_TRUE(Testing::WriteInputDefault(Material, Testing::ExpressionInput(CoordinatesId, 0, {}), {.Kind = EMaterialInputDefaultKind::Literal,
		.Type = EMaterialProgramValueType::Float, .Literal = {1}}));
	const auto Before = CaptureExpressions(*Material);
	ASSERT_TRUE(Testing::WriteConnection(Material, Testing::ExpressionInput(Id, 0), FMaterialProgramLink{CoordinatesId}, false));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	ASSERT_TRUE(Transactions->Redo());
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(AuthoringGraph->CopySelection(std::array{Id, CoordinatesId}, Payload));
	EXPECT_EQ(Payload.Nodes.size(), 2u);
	TStrongObjectPtr<DMaterialGraph> TargetEditorGraph(DMaterialGraph::Create(*Target));
	const auto Pasted = TargetEditorGraph->Paste(Payload, 0, 0, false);
	ASSERT_TRUE(Pasted) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Pasted);
	ASSERT_EQ(Target->GetParameterDefinitions().size(), 1u);
	EXPECT_NE(Target->GetParameterDefinitions().front().Id, Material->GetParameterDefinitions().front().Id);
	for (const auto& Node : Target->GetExpressionCollection().Expressions)
	{
		if (const auto* Coordinates = Cast<DMaterialExpressionTextureCoordinates>(Node.Get()))
			EXPECT_EQ(Coordinates->Channel.Constant, (std::vector<float>{1}));
		if (const auto* Sample = Cast<DMaterialExpressionTextureSampleParameter2D>(Node.Get()))
			EXPECT_TRUE(Sample->UV.Connection.IsConnected());
	}
	auto* Function = NewObject<DMaterialFunction>(nullptr, "RejectRootBindings");
	TStrongObjectPtr<DMaterialGraph> FunctionEditorGraph(DMaterialGraph::Create(*Function));
	EXPECT_FALSE(FunctionEditorGraph->Paste(Payload, 0, 0, false));
	MarkAsGarbage(Function);
	MarkAsGarbage(Target);
	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, ReplacementDraftCannotMutateOwnerOrHistoryAfterCommit)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material.Get()));
	const auto Created = Testing::CreateGraphConstant(*AuthoringGraph, 0.25f);
	ASSERT_TRUE(Created);
	auto Draft = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>(Created.value());
	auto* Constant = Draft.Get();
	ASSERT_NE(Constant, nullptr);
	EXPECT_NE(Constant, Material->GetExpressionCollection().Expressions.front().Get());
	Constant->Value = 0.5f;
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	ASSERT_TRUE(AuthoringGraph->ReplaceExpression(*Constant));
	Constant->Value = 0.9f;
	const auto Read = [&]() { return Cast<DMaterialExpressionScalarConstant>(Material->GetExpressionCollection().Expressions.front().Get())->Value; };
	EXPECT_FLOAT_EQ(Read(), 0.5f);
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FLOAT_EQ(Read(), 0.25f);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_FLOAT_EQ(Read(), 0.5f);
	const auto Revision = Material->GetMaterialCompileDebugInfo().AuthoredRevision;
	Constant->Value = std::numeric_limits<float>::quiet_NaN();
	EXPECT_FALSE(AuthoringGraph->FindNode(Constant->Id)->SetConstantValue(FMaterialParameterValue::MakeScalar(Constant->Value)));
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FLOAT_EQ(Read(), 0.25f);
}

TEST(FMaterialGraphOperationsTests, TypedReplacementRejectsRecursiveFunctionTargetsAtomically)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterialFunction> Caller(NewObject<DMaterialFunction>(nullptr, "TypedReplacementCaller"));
	TStrongObjectPtr<DMaterialFunction> Callee(NewObject<DMaterialFunction>(nullptr, "TypedReplacementCallee"));
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Caller.Get()));
	const auto Inserted = AuthoringGraph->InsertFunctionCall(*Callee.Get(), 0, 0).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Inserted) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Inserted);
	const auto Revision = Caller->GetFunctionRevision();
	TStrongObjectPtr<DMaterialExpressionFunctionCall> Replacement(NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None));
	Replacement->Id = Inserted.value();
	Replacement->Function = Caller.Get();
	for (const auto& Port : Caller->GetFunctionSignature().Outputs) Replacement->FunctionOutputs.push_back({Port.Id, Port.Type});
	const auto Rejected = AuthoringGraph->ReplaceExpression(*Replacement.Get());
	ASSERT_FALSE(Rejected);
	EXPECT_NE(FormatMaterialGraphCommandResult(Rejected).find("recursive"), std::string::npos);
	EXPECT_EQ(Caller->GetFunctionRevision(), Revision);
	EXPECT_EQ(Cast<DMaterialExpressionFunctionCall>(Caller->GetExpressionCollection().Expressions.back().Get())->Function.Get(), Callee.Get());
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Caller->GetExpressionCollection().Expressions.size(), 2u);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Caller->GetExpressionCollection().Expressions.size(), 3u);
}

TEST(FMaterialGraphOperationsTests, TypedReplacementKeepsIndependentHistoryAndSwizzleWidth)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialExpressionVector4Constant> Constant(NewObject<DMaterialExpressionVector4Constant>(nullptr, NAME_None));
	Constant->Id = FGuid::NewGuid();
	Constant->Value = {0.1, 0.2, 0.3, 0.4};
	TStrongObjectPtr<DMaterialExpressionSwizzle> Swizzle(NewObject<DMaterialExpressionSwizzle>(nullptr, NAME_None));
	Swizzle->Id = FGuid::NewGuid();
	Swizzle->Input = {Constant.Get()};
	Swizzle->Components = {0, 1};
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 2>{Constant.Get(), Swizzle.Get()}, {}));
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	const auto Id = Constant->Id;
	Constant->Value = {0.5, 0.6, 0.7, 0.8};
	ASSERT_TRUE(AuthoringGraph->ReplaceExpression(*Constant.Get()));
	Constant->Value = {9, 9, 9, 9};
	const auto ReadValue = [&]() { return Cast<DMaterialExpressionVector4Constant>(Material->GetExpressionCollection().Expressions[0].Get())->Value; };
	EXPECT_EQ(ReadValue(), FVector4(0.5, 0.6, 0.7, 0.8));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(ReadValue(), FVector4(0.1, 0.2, 0.3, 0.4));
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(ReadValue(), FVector4(0.5, 0.6, 0.7, 0.8));
	EXPECT_EQ(Material->GetExpressionCollection().Expressions[0]->GetOuter(), Material.Get());
	const auto ReadComponents = [&]() { return Cast<DMaterialExpressionSwizzle>(Material->GetExpressionCollection().Expressions[1].Get())->Components; };
	ASSERT_TRUE(AuthoringGraph->FindNode(Swizzle->Id)->SetSwizzleComponents(std::array<uint8, 1>{3}));
	EXPECT_EQ(ReadComponents(), (std::vector<uint8>{3}));
	const auto Revision = Material->GetMaterialCompileDebugInfo().AuthoredRevision;
	EXPECT_FALSE(AuthoringGraph->FindNode(Swizzle->Id)->SetSwizzleComponents(std::array<uint8, 1>{4}));
	EXPECT_FALSE(AuthoringGraph->FindNode(Swizzle->Id)->SetSwizzleComponents({}));
	EXPECT_FALSE(AuthoringGraph->FindNode(Id)->SetConstantValue(FMaterialParameterValue::MakeTexture(nullptr)));
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(ReadComponents(), (std::vector<uint8>{0, 1}));
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(ReadComponents(), (std::vector<uint8>{3}));
}

TEST(FMaterialGraphOperationsTests, ConstantPaletteUsesOneEntryAndTypeChangesPreserveValidGraphs)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "GenericConstantMaterial");
	ASSERT_NE(Material, nullptr);
	auto Entries = SearchCatalog("constant");
	std::erase_if(Entries, [](const FMaterialGraphCatalogEntry& Entry) {
		return Entry.Opcode != EMaterialProgramOpcode::Constant;
	});
	ASSERT_EQ(Entries.size(), 1u);
	EXPECT_EQ(Entries.front().ResultType, EMaterialProgramValueType::Float);
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	const auto Created = Testing::CreateGraphCatalogNode(*DMaterialGraph::Create(*Material), Entries.front(), 0, 0, {});
	ASSERT_TRUE(Created) << FormatMaterialGraphCommandResult(Created);
	const auto NodeId = Created->front();
	auto ResultType = EMaterialProgramValueType::Float;
	TStrongObjectPtr<DMaterialGraph> EditorGraph(DMaterialGraph::Create(*Material));
	ASSERT_NE(EditorGraph->FindNode(NodeId), nullptr);
	EXPECT_EQ(EditorGraph->FindNode(NodeId)->GetMaterialExpression()->GetOutputValueType(0), ResultType);
	const auto ApplyValue = [&]() {
		FMaterialParameterValue Value;
		switch (ResultType)
		{
		case EMaterialProgramValueType::Float2: Value = FMaterialParameterValue::MakeVector2({0.2f, 0.4f}); break;
		case EMaterialProgramValueType::Float3: Value = FMaterialParameterValue::MakeVector({0.2f, 0.4f, 0.6f}); break;
		case EMaterialProgramValueType::Float4: Value = FMaterialParameterValue::MakeVector4({0.2f, 0.4f, 0.6f, 0.8f}); break;
		default: Value = FMaterialParameterValue::MakeScalar(0.2f); break;
		}
		return EditorGraph->FindNode(NodeId)->SetConstantValue(Value);
	};
	ASSERT_TRUE(ApplyValue());
	for (auto Type : {EMaterialProgramValueType::Float2, EMaterialProgramValueType::Float3,
		EMaterialProgramValueType::Float4})
	{
		const auto Before = CaptureExpressions(*Material);
		ResultType = Type;
		ASSERT_TRUE(ApplyValue());
		const auto CheckValue = [&]() {
			const auto* Node = EditorGraph->FindNode(NodeId);
			ASSERT_NE(Node, nullptr);
			EXPECT_EQ(Node->GetMaterialExpression()->GetOutputValueType(0), Type);
			EXPECT_FALSE(Node->GetMaterialExpression()->GetOutput(1));
			if (Type == EMaterialProgramValueType::Float2)
			{
				const auto* Value = FindExpression<DMaterialExpressionVector2Constant>(*Material, NodeId);
				ASSERT_NE(Value, nullptr); EXPECT_EQ(Value->Value, FVector2(.2f, .4f));
			}
			else if (Type == EMaterialProgramValueType::Float3)
			{
				const auto* Value = FindExpression<DMaterialExpressionVector3Constant>(*Material, NodeId);
				ASSERT_NE(Value, nullptr); EXPECT_EQ(Value->Value, FVector3(.2f, .4f, .6f));
			}
			else
			{
				const auto* Value = FindExpression<DMaterialExpressionVector4Constant>(*Material, NodeId);
				ASSERT_NE(Value, nullptr); EXPECT_EQ(Value->Value, FVector4(.2f, .4f, .6f, .8f));
			}
		};
		CheckValue();
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(CaptureExpressions(*Material), Before);
		ASSERT_TRUE(Transactions->Redo());
		CheckValue();
	}
	ResultType = EMaterialProgramValueType::Float;
	ASSERT_TRUE(ApplyValue());
	ASSERT_TRUE(Testing::WriteConnection(&*Material, EMaterialProperty::MP_Roughness, FMaterialProgramLink{NodeId}, true));
	const auto Connected = CaptureExpressions(*Material);
	const auto Revision = Material->GetMaterialCompileDebugInfo().AuthoredRevision;
	ResultType = EMaterialProgramValueType::Float4;
	ASSERT_TRUE(ApplyValue());
	EXPECT_FALSE(HasGraphDiagnostics(*Material));
	EXPECT_GT(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Connected);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FALSE(Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->Connection.IsConnected());
	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, ParameterSharingUsesIndependentNodesAndRenamePreservesIdentity)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "SharedParameter");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialParameterDefinition Definition;
	Definition.Name = "SharedValue";
	Definition.Value = FMaterialParameterValue::MakeScalar(0.4f);
	const auto Created = MaterialEditing::CreateParameter(*Material, Definition, false).transform(Testing::ExpressionNodeId);
	ASSERT_TRUE(Created) << FormatMaterialGraphCommandResult(Created);

	const auto OwnerId = Created.value();
	const auto* CreatedParameter = FindExpression<DMaterialExpressionParameter>(*Material, Created.value());
	ASSERT_NE(CreatedParameter, nullptr);
	const auto ParameterId = CreatedParameter->Metadata.Id;
	Durin::Tests::FTestEditorTransactorOwner Transactions;

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	ASSERT_TRUE(Testing::WriteConnection(Material, EMaterialProperty::MP_Metallic, FMaterialProgramLink{OwnerId}, true));
	ASSERT_TRUE(Testing::WriteConnection(Material, EMaterialProperty::MP_Roughness, FMaterialProgramLink{OwnerId}, true));
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 1u);
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
	TStrongObjectPtr<DMaterialExpression> DuplicateOwner(DuplicateObject(Material->GetExpressionCollection().Expressions.front().Get(), nullptr, NAME_None).value());
	DuplicateOwner->Id = FGuid::NewGuid();
	ASSERT_TRUE(AuthoringGraph->CreateExpression(*DuplicateOwner.Get(), 0, 0, false));
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 1u);
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 2u);
	ASSERT_TRUE(MaterialEditing::RenameParameter(*Material, ParameterId, "RenamedShared"));
	EXPECT_EQ(Material->FindParameterDefinition("RenamedShared")->Id, ParameterId);
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.front()->Id, OwnerId);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_NE(Material->FindParameterDefinition("SharedValue"), nullptr);
	ASSERT_TRUE(Transactions->Redo());
	const auto View = Testing::CaptureGraphView(*AuthoringGraph);
	EXPECT_EQ(FindViewNode(View, OwnerId)->PrimaryLabel, Material->FindParameterDefinition(ParameterId)->DisplayName);
	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, GenericParametersCreateIndependentDeclarationsAndUndoAtomically)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "GenericParameterMaterial");
	ASSERT_NE(Material, nullptr);
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	auto Entries = EnumerateCatalog();
	const auto IsParameter = [](const FMaterialGraphCatalogEntry& Entry) {
		return Entry.Opcode == EMaterialProgramOpcode::Parameter
			|| Entry.Opcode == EMaterialProgramOpcode::TextureParameter;
	};
	std::erase_if(Entries, [&](const auto& Entry) { return !IsParameter(Entry); });
	ASSERT_EQ(Entries.size(), 4u);
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	for (const auto& Entry : Entries)
	{
		ASSERT_NE(Entry.ExpressionClass, nullptr);
		EXPECT_TRUE(Entry.ExpressionClass->IsChildOf(DMaterialExpressionParameter::StaticClass()));
		const auto Before = CaptureExpressions(*Material);
		const auto BeforePresentation = Material->GetMaterialGraphPresentation();
		const std::vector<FMaterialParameterDefinition> BeforeDefinitions(
			Material->GetParameterDefinitions().begin(), Material->GetParameterDefinitions().end());
		const auto Created = Testing::CreateGraphCatalogNode(*DMaterialGraph::Create(*Material), Entry, 400, 200, {});
		ASSERT_TRUE(Created) << FormatMaterialGraphCommandResult(Created);
		ASSERT_EQ(Created->size(), 1u);

		EXPECT_EQ(Material->GetParameterDefinitions().size(), BeforeDefinitions.size() + 1);
		const auto After = CaptureExpressions(*Material);
		const auto AfterPresentation = Material->GetMaterialGraphPresentation();
		const auto* Parameter = Cast<DMaterialExpressionParameter>(After.Expressions.back().Get());
		ASSERT_NE(Parameter, nullptr);
		EXPECT_EQ(Parameter->Id, Created->front());
		EXPECT_EQ(Parameter->GetOutputValueType(0), Entry.ResultType);
		EXPECT_FALSE(Parameter->GetOutput(1));
		EXPECT_EQ(Parameter->GetInput(0), nullptr);
		const auto FirstId = Parameter->Metadata.Id;
		EXPECT_TRUE(FirstId.IsValid());
		EXPECT_EQ(Parameter->GetClass(), Entry.ResultType == EMaterialProgramValueType::Float2 || Entry.ResultType == EMaterialProgramValueType::Float3
			? DMaterialExpressionVector4Parameter::StaticClass() : Entry.ExpressionClass);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(CaptureExpressions(*Material), Before);
		EXPECT_EQ(Material->GetMaterialGraphPresentation(), BeforePresentation);
		EXPECT_TRUE(std::ranges::equal(Material->GetParameterDefinitions(), BeforeDefinitions));
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(CaptureExpressions(*Material), After);
		EXPECT_EQ(Material->GetMaterialGraphPresentation(), AfterPresentation);

		const auto Second = Testing::CreateGraphCatalogNode(*DMaterialGraph::Create(*Material), Entry, 0, 0, {});
		ASSERT_TRUE(Second) << FormatMaterialGraphCommandResult(Second);
		ASSERT_EQ(Second->size(), 1u);
		const auto* SecondParameter = FindExpression<DMaterialExpressionParameter>(*Material, Second->front());
		ASSERT_NE(SecondParameter, nullptr);
		const auto SecondId = SecondParameter->Metadata.Id;
		EXPECT_NE(SecondId, FirstId);
		const auto* FirstDefinition = Material->FindParameterDefinition(FirstId);
		const auto* SecondDefinition = Material->FindParameterDefinition(SecondId);
		ASSERT_NE(FirstDefinition, nullptr);
		ASSERT_NE(SecondDefinition, nullptr);
		EXPECT_NE(FirstDefinition->Name, SecondDefinition->Name);
		TStrongObjectPtr<DMaterialExpression> SharedNode(DuplicateObject(Material->GetExpressionCollection().Expressions.back().Get(), nullptr, NAME_None).value());
		Cast<DMaterialExpressionParameter>(SharedNode.Get())->Metadata.Id = FirstDefinition->Id;
		EXPECT_FALSE(DMaterialGraph::Create(*Material)->ReplaceExpression(*SharedNode.Get()));
		EXPECT_EQ(Cast<DMaterialExpressionParameter>(Material->GetExpressionCollection().Expressions.back().Get())->Metadata.Id, SecondId);
		EXPECT_EQ(std::ranges::count_if(SearchCatalog("parameter"), IsParameter), 4);
	}
	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, TypedLayoutIncludesCallAndSurfaceDependenciesAndPreservesLabels)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "TypedLayout");
	auto* Function = NewObject<DMaterialFunction>(nullptr, "LayoutSurfaceFunction");
	auto* Base = NewObject<DMaterialExpressionMakeMaterialAttributes>(nullptr, NAME_None);
	Base->Id = FGuid::NewGuid();
	auto* Constant = NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None);
	Constant->Id = FGuid::NewGuid(); Constant->Value = .7f;
	auto* Override = NewObject<DMaterialExpressionSetMaterialAttributes>(nullptr, NAME_None);
	Override->Id = FGuid::NewGuid(); Override->Surface = {Base};
	Override->Attributes.push_back({EMaterialProperty::MP_Roughness, {Constant}});
	auto* Call = NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None);
	Call->Id = FGuid::NewGuid(); Call->Function = Function;
	const auto InputPort = Function->GetFunctionSignature().Inputs.front().Id;
	const auto OutputPort = Function->GetFunctionSignature().Outputs.front().Id;
	Call->FunctionInputs.push_back({InputPort, EMaterialProgramValueType::MaterialAttributes, {Override}});
	Call->FunctionOutputs.push_back({OutputPort, EMaterialProgramValueType::MaterialAttributes});
	const auto BaseId = Base->Id, ConstantId = Constant->Id, OverrideId = Override->Id, CallId = Call->Id;
	FMaterialRootInputs Outputs;
	Outputs.Attributes = {Call, 0, OutputPort}; Outputs.bUseMaterialAttributes = true;
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 4>{Call, Override, Base, Constant}, Outputs));
	ASSERT_TRUE(Material->SetMaterialGraphPresentation({.Nodes = {{ConstantId, 100, 100, "Retained label"}}}) != Durin::EMaterialGraphPresentationResult::Rejected);
	const auto Revision = Material->GetMaterialCompileDebugInfo().AuthoredRevision;
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	TStrongObjectPtr<DMaterialGraph> EditorGraph(DMaterialGraph::Create(*Material));
	ASSERT_TRUE(EditorGraph->Layout({}));
	const auto Layout = Material->GetMaterialGraphPresentation();
	const auto Position = [&](FGuid Id) -> const FMaterialGraphNodePresentation& {
		return *std::ranges::find(Layout.Nodes, Id, &FMaterialGraphNodePresentation::NodeId);
	};
	EXPECT_LT(Position(BaseId).X, Position(OverrideId).X);
	EXPECT_LT(Position(ConstantId).X, Position(OverrideId).X);
	EXPECT_LT(Position(OverrideId).X, Position(CallId).X);
	EXPECT_LT(Position(CallId).X, Material->GetRootPosition().first);
	EXPECT_EQ(Position(ConstantId).DisplayName, "Retained label");
	const auto HistoryLength = Transactions->GetQueueLength();
	ASSERT_TRUE(EditorGraph->Layout({}));
	EXPECT_EQ(Transactions->GetQueueLength(), HistoryLength);
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision);
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), Layout);
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision);
	Transactions->Reset();
	MarkAsGarbage(Material); MarkAsGarbage(Function); CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, MaximumGraphLayoutIsDeterministicAndPresentationOnly)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "MaximumLayoutMaterial");
	ASSERT_NE(Material, nullptr);
	Testing::FTestMaterialExpressionGraph Graph;
	while (Graph.Expressions.size() < MaterialProgramMaxNodeCount)
		Graph.Expressions.emplace_back(Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>().Get());
	ASSERT_TRUE(Graph.Apply(*Material));
	const auto MaximumGraph = CaptureExpressions(*Material);
	const uint64 SemanticRevision =
		Material->GetMaterialCompileDebugInfo().AuthoredRevision;

	TStrongObjectPtr<DMaterialGraph> EditorGraph(DMaterialGraph::Create(*Material));
	ASSERT_TRUE(EditorGraph->Layout({}, false));
	EXPECT_EQ(Material->GetMaterialGraphPresentation().Nodes.size(),
		MaterialProgramMaxNodeCount);
	EXPECT_EQ(CaptureExpressions(*Material), MaximumGraph);
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision,
		SemanticRevision);
	const FMaterialGraphPresentation FirstLayout =
		Material->GetMaterialGraphPresentation();
	const FMaterialGraphView LayoutView = Testing::CaptureGraphView(*EditorGraph);
	for (size_t A = 0; A < LayoutView.Nodes.size(); ++A)
		for (size_t B = A + 1; B < LayoutView.Nodes.size(); ++B)
		{
			const auto& PositionA = LayoutView.Nodes[A].Presentation;
			const auto& PositionB = LayoutView.Nodes[B].Presentation;
			const float HeightA = GraphNodeHeight(LayoutView.Nodes[A]);
			const float HeightB = GraphNodeHeight(LayoutView.Nodes[B]);
			EXPECT_FALSE(PositionA.X < PositionB.X + GraphNodeWidth(LayoutView.Nodes[B])
				&& PositionA.X + GraphNodeWidth(LayoutView.Nodes[A]) > PositionB.X
				&& PositionA.Y < PositionB.Y + HeightB
				&& PositionA.Y + HeightA > PositionB.Y);
		}
	EXPECT_TRUE(EditorGraph->Layout({}, false));
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), FirstLayout);

	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
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
	auto Validation = Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Material);
	ASSERT_TRUE(Validation);
	std::string Error;
	ASSERT_TRUE(PrepareNewMaterialForEditing(*Material, Error)) << Error;
	const FMaterialGraphPresentation& Presentation =
		Material->GetMaterialGraphPresentation();
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 0u);
	EXPECT_FALSE(Material->GetExpressionInputForProperty(EMaterialProperty::MP_MaterialAttributes)->IsConnected());
	EXPECT_EQ(Presentation.Nodes.size(),
		Material->GetExpressionCollection().Expressions.size());
	EXPECT_TRUE(Material->GetExpressionCollection().Expressions.empty());
	TStrongObjectPtr<DMaterialGraph> InspectionGraph(DMaterialGraph::Create(*Material));
	const FMaterialGraphView View = Testing::CaptureGraphView(*InspectionGraph);
	EXPECT_EQ(View.Nodes.size(), Material->GetExpressionCollection().Expressions.size() + 1);
	EXPECT_EQ(std::pair(InspectionGraph->GetRootNode()->GetView().Presentation.X, InspectionGraph->GetRootNode()->GetView().Presentation.Y), Material->GetRootPosition());

	MarkObjectHierarchyAsGarbage(Package);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, LayoutReducesDenseCrossingsAndAvoidsSelectedCollisions)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "DenseLayoutMaterial");
	ASSERT_NE(Material, nullptr);
	Testing::FTestMaterialExpressionGraph Graph;
	std::array<FGuid, 8> Sources;
	std::array<FGuid, 8> Consumers;
	for (uint32 Index = 0; Index < Sources.size(); ++Index)
	{
		Sources[Index] = FGuid(100 + Index, 0, 0, 1);
		Consumers[Index] = FGuid(200 + Index, 0, 0, 1);
	}
	for (uint32 Index = 0; Index < Sources.size(); ++Index)
	{
		Graph.Expressions.emplace_back(Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>(Sources[Index]).Get());
	}
	for (uint32 Index = 0; Index < Sources.size(); ++Index)
	{
		auto Consumer = Testing::MakeGraphExpression<DMaterialExpressionSaturate>(Consumers[Index]);
		Consumer->ResultType = EMaterialProgramValueType::Float;
		Consumer->Input = {Graph.Expressions[Sources.size() - Index - 1].Get()};
		Graph.Expressions.emplace_back(Consumer.Get());
	}
	auto Validation = Graph.Apply(*Material);
	ASSERT_TRUE(Validation);
	TStrongObjectPtr<DMaterialGraph> EditorGraph(DMaterialGraph::Create(*Material));
	ASSERT_TRUE(EditorGraph->Layout({}, false));
	const FMaterialGraphView View = Testing::CaptureGraphView(*EditorGraph);
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
	ASSERT_TRUE(EditorGraph->MoveNodes(std::span(&Occupied, 1), false));
	ASSERT_TRUE(EditorGraph->Layout(std::span(&Selected, 1), false));
	const FMaterialGraphView Relayout = Testing::CaptureGraphView(*EditorGraph);
	const auto* RelayoutSelected = FindViewNode(Relayout, Selected);
	const auto* RelayoutFixed = FindViewNode(Relayout, Fixed);
	ASSERT_NE(RelayoutSelected, nullptr);
	ASSERT_NE(RelayoutFixed, nullptr);
	const float Height = GraphNodeHeight(*RelayoutSelected);
	const float Width = FMaterialGraphGeometry::GetMetrics().NodeWidth;
	EXPECT_FALSE(RelayoutSelected->Presentation.X < RelayoutFixed->Presentation.X + Width
		&& RelayoutSelected->Presentation.X + Width > RelayoutFixed->Presentation.X
		&& RelayoutSelected->Presentation.Y < RelayoutFixed->Presentation.Y + Height
		&& RelayoutSelected->Presentation.Y + Height > RelayoutFixed->Presentation.Y);

	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests,
	ClipboardPasteRemapsIdentityPreservesExternalLinksAndRejectsAtomically)
{
	InitializeDObjectSystem();
	DMaterial* Material = MakeExpandedGraphMaterial("ClipboardMaterial");
	ASSERT_NE(Material, nullptr);
	auto Constant = Testing::MakeGraphExpression<DMaterialExpressionVector3Constant>();
	Constant->Value = {.2f, .4f, .6f};
	auto Saturate = Testing::MakeGraphExpression<DMaterialExpressionSaturate>();
	Saturate->ResultType = EMaterialProgramValueType::Float3;
	Saturate->Input = {Constant.Get()};
	FMaterialRootInputs Outputs;
	Outputs.Get(EMaterialProperty::MP_BaseColor) = {Saturate.Get()};
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 2>{Constant.Get(), Saturate.Get()}, Outputs));
	TStrongObjectPtr<DMaterialGraph> MaterialEditorGraph(DMaterialGraph::Create(*Material));
	ASSERT_TRUE(MaterialEditorGraph->Layout({}, false));
	std::vector<FGuid> AllNodes;
	for (const auto& Node : Material->GetExpressionCollection().Expressions) AllNodes.push_back(Node->Id);
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(MaterialEditorGraph->CopySelection(AllNodes, Payload));
	ASSERT_EQ(Payload.Nodes.size(), AllNodes.size());
	EXPECT_TRUE(std::ranges::any_of(Payload.Nodes,
		[](const FMaterialGraphClipboardNode& Node) {
			return Node.RelativeX == 0;
		}));
	EXPECT_TRUE(std::ranges::any_of(Payload.Nodes,
		[](const FMaterialGraphClipboardNode& Node) {
			return Node.RelativeY == 0;
		}));

	const auto BeforeProgram = CaptureExpressions(*Material);
	const FMaterialGraphPresentation BeforePresentation =
		Material->GetMaterialGraphPresentation();
	const MIR::FNormalizationResult BeforeIdentity = Normalize(*Material);
	ASSERT_TRUE(BeforeIdentity);
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	const auto Pasted = MaterialEditorGraph->Paste(Payload, 1200, 400).transform(Testing::NodeIds);
	ASSERT_TRUE(Pasted) << FormatMaterialGraphCommandResult(Pasted);
	ASSERT_EQ(Pasted->size(), Payload.Nodes.size());
	std::unordered_set<FGuid> OriginalIds(AllNodes.begin(), AllNodes.end());
	for (const FGuid& Id : Pasted.value())
		EXPECT_FALSE(OriginalIds.contains(Id));
	const MIR::FNormalizationResult AfterIdentity = Normalize(*Material);
	ASSERT_TRUE(AfterIdentity);
	EXPECT_EQ(AfterIdentity.Identity, BeforeIdentity.Identity);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), BeforeProgram);
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), BeforePresentation);
	ASSERT_TRUE(Transactions->Redo());

	FMaterialGraphClipboardPayload UnknownVersion = Payload;
	UnknownVersion.SchemaVersion = 99;
	const auto BeforeRejected = CaptureExpressions(*Material);
	const auto QueueLength = Transactions->GetQueueLength();
	ASSERT_GE(Transactions->Begin("Graph", "Move and reject paste"), 0);
	const auto Nested = Testing::CreateGraphConstant(*MaterialEditorGraph, .3f, 0, 0, true);
	ASSERT_TRUE(Nested);
	const auto Rejected = MaterialEditorGraph->Paste(UnknownVersion, 0, 0);
	ASSERT_EQ(Transactions->End(), 1);
	EXPECT_EQ(Transactions->GetQueueLength(), QueueLength + 1);
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_FALSE(Rejected);
	EXPECT_EQ(Rejected.error().GetStatus(), EMaterialGraphCommandStatus::Rejected);
	EXPECT_EQ(CaptureExpressions(*Material), BeforeRejected);

	const auto* Dependent = FindExpression<DMaterialExpressionSaturate>(*Material, Saturate->Id);
	ASSERT_NE(Dependent, nullptr);
	const FGuid RequiredSource = Dependent->Input.Connection.GetExpressionId();
	const FGuid DependentId = Dependent->Id;
	const auto ExternalInput = Dependent->Input;
	const FMaterialGraphCommandResult RequiredRemoval =
		DMaterialGraph::Create(*Material)->RemoveNodes(std::span(&RequiredSource, 1));
	ASSERT_TRUE(RequiredRemoval);
	// Numeric definitions now supply Saturate's inherited zero after disconnection.
	EXPECT_FALSE(HasGraphDiagnostics(*Material));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), BeforeRejected);
	FMaterialGraphClipboardPayload Partial;
	ASSERT_TRUE(MaterialEditorGraph->CopySelection(std::span(&DependentId, 1), Partial));
	ASSERT_EQ(Partial.Nodes.size(), 1u);
	const auto* CopiedExpression = Cast<DMaterialExpressionSaturate>(Partial.Nodes.front().Expression.Get());
	ASSERT_NE(CopiedExpression, nullptr);
	EXPECT_EQ(CopiedExpression->Input, ExternalInput);
	const auto PartialPaste = MaterialEditorGraph->Paste(Partial, 0, 0).transform(Testing::NodeIds);
	ASSERT_TRUE(PartialPaste) << FormatMaterialGraphCommandResult(PartialPaste);
	ASSERT_EQ(PartialPaste->size(), 1u);
	const auto* PastedDependent = FindExpression<DMaterialExpressionSaturate>(*Material, PartialPaste->front());
	ASSERT_NE(PastedDependent, nullptr);
	EXPECT_EQ(PastedDependent->Input, ExternalInput);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), BeforeRejected);

	const auto StandaloneCreated = Testing::CreateGraphConstant(*DMaterialGraph::Create(*Material), 0.75f, 80, 120, true);
	ASSERT_TRUE(StandaloneCreated);
	const FGuid StandaloneId = StandaloneCreated.value();
	const auto Duplicated = MaterialEditorGraph->DuplicateNodes(std::span(&StandaloneId, 1), 40, 40).transform(Testing::NodeIds);
	ASSERT_TRUE(Duplicated) << FormatMaterialGraphCommandResult(Duplicated);
	ASSERT_EQ(Duplicated->size(), 1u);
	EXPECT_NE(Duplicated->front(), StandaloneId);
	FMaterialGraphClipboardPayload CutPayload;
	const FMaterialGraphCommandResult Cut = MaterialEditorGraph->CutSelection(std::span(&StandaloneId, 1), CutPayload);
	ASSERT_TRUE(Cut) << FormatMaterialGraphCommandResult(Cut);
	ASSERT_EQ(CutPayload.Nodes.size(), 1u);
	EXPECT_EQ(CutPayload.Nodes.front().Expression->Id, StandaloneId);
	EXPECT_EQ(FindViewNode(Testing::CaptureGraphView(*MaterialEditorGraph), StandaloneId),
		nullptr);
	ASSERT_TRUE(Transactions->Undo());
	const FMaterialGraphView RestoredCut = Testing::CaptureGraphView(*MaterialEditorGraph);
	EXPECT_NE(FindViewNode(RestoredCut, StandaloneId), nullptr);
	Transactions->Reset();
	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, CrossGraphPasteUsesExpressionObjectsAndReplaysOutputSlots)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> MaterialGraph(DMaterialGraph::Create(*Material));
	TStrongObjectPtr<DMaterialGraph> FunctionGraph(DMaterialGraph::Create(*Function));
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	constexpr auto Slot = static_cast<int32>(EMaterialProperty::MP_Roughness);
	for (auto* From : {MaterialGraph.Get(), FunctionGraph.Get()})
	{
		auto* To = From == MaterialGraph.Get() ? FunctionGraph.Get() : MaterialGraph.Get();
		auto Attributes = Testing::MakeGraphExpression<DMaterialExpressionGetMaterialAttributes>();
		const auto Added = From->CreateExpression(*Attributes, 0, 0, false).transform(Testing::GraphNodeId);
		ASSERT_TRUE(Added);
		auto* Original = From->FindNode(Added.value())->GetMaterialExpression();
		auto Saturate = Testing::MakeGraphExpression<DMaterialExpressionSaturate>();
		Saturate->Input.Connection.Connect(Slot, Original);
		const auto Consumer = From->CreateExpression(*Saturate, 300, 0, false).transform(Testing::GraphNodeId);
		ASSERT_TRUE(Consumer);
		FMaterialGraphClipboardPayload Payload;
		const std::array Selection{Original->Id, Consumer.value()};
		ASSERT_TRUE(From->CopySelection(Selection, Payload));
		ASSERT_EQ(Payload.Nodes.size(), 2u);
		const size_t Upstream = Payload.Nodes[0].Expression->Id == Original->Id ? 0 : 1;
		const size_t Downstream = 1 - Upstream;
		auto* SnapshotConsumer = Cast<DMaterialExpressionSaturate>(Payload.Nodes[Downstream].Expression.Get());
		ASSERT_TRUE(SnapshotConsumer);
		auto& Link = SnapshotConsumer->Input.Connection;
		auto* Snapshot = Payload.Nodes[Upstream].Expression.Get();
		ASSERT_NE(Original, Snapshot);
		ASSERT_EQ(Link.Expression.Get(), Snapshot);
		ASSERT_EQ(Link.OutputIndex, Slot);
		const auto Before = To->GetNodes().size();
		// The source asset and its snapshot have the same node GUID but are different endpoints.
		Link.Expression = Original;
		EXPECT_FALSE(To->Paste(Payload, 600, 0));
		EXPECT_EQ(To->GetNodes().size(), Before);
		EXPECT_FALSE(Transactions->Undo());
		Link.Expression = Snapshot;
		ASSERT_TRUE(From->RemoveNodes(Selection, false));
		const auto Pasted = To->Paste(Payload, 600, 0).transform(Testing::NodeIds);
		ASSERT_TRUE(Pasted) << FormatMaterialGraphCommandResult(Pasted);
		ASSERT_EQ(Pasted->size(), 2u);
		const auto Check = [&] {
			const auto* SourceNode = To->FindNode(Pasted.value()[Upstream]);
			const auto* TargetNode = To->FindNode(Pasted.value()[Downstream]);
			ASSERT_TRUE(SourceNode && TargetNode);
			const auto* Target = Cast<DMaterialExpressionSaturate>(TargetNode->GetMaterialExpression());
			ASSERT_TRUE(Target);
			EXPECT_NE(Target->Input.Connection.Expression.Get(), Snapshot);
			EXPECT_EQ(Target->Input.Connection.Expression.Get(), SourceNode->GetMaterialExpression());
			EXPECT_EQ(Target->Input.Connection.OutputIndex, Slot);
			const auto* InputPin = TargetNode->GetInputPin(0);
			ASSERT_TRUE(InputPin);
			ASSERT_EQ(InputPin->LinkedTo.size(), 1u);
			EXPECT_EQ(InputPin->LinkedTo[0]->GetOwningNode(), SourceNode);
			EXPECT_EQ(InputPin->LinkedTo[0]->SourceIndex, Slot);
		};
		Check();
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(To->GetNodes().size(), Before);
		ASSERT_TRUE(Transactions->Redo());
		Check();
		EXPECT_EQ(Link.Expression.Get(), Snapshot);
		Transactions->Reset();
	}
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

	TStrongObjectPtr<DMaterialGraph> SourceDocumentAuthoringGraph(DMaterialGraph::Create(*Source));
	const auto Call = SourceDocumentAuthoringGraph->InsertFunctionCall(*Function, 0, 0, false).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Call);
	const auto OutputId = Function->GetFunctionSignature().Outputs[0].Id;
	ASSERT_TRUE(Testing::WriteConnection(Source, EMaterialProperty::MP_MaterialAttributes, FMaterialProgramLink{Call.value(), 0, OutputId}, true, false));
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(SourceDocumentAuthoringGraph->CopySelection(std::array{Call.value()}, Payload));
	ASSERT_EQ(Payload.Nodes.size(), 1u);
	ASSERT_NE(Cast<DMaterialExpressionFunctionCall>(Payload.Nodes.front().Expression.Get()), nullptr);
	TWeakObjectPtr<DMaterialFunction> WeakFunction(Function);
	MarkAsGarbage(Source);
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(WeakFunction.IsValid());
	EXPECT_FALSE(Payload.SourceRoot.IsValid());
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	const auto Before = CaptureExpressions(*Target);
	TStrongObjectPtr<DMaterialGraph> TargetEditorGraph(DMaterialGraph::Create(*Target));
	const auto Pasted = TargetEditorGraph->Paste(Payload, 100, 100, true).transform(Testing::NodeIds);
	ASSERT_TRUE(Pasted) << FormatMaterialGraphCommandResult(Pasted);
	EXPECT_NE(Pasted.value()[0], Call.value());
	EXPECT_EQ(Target->GetExpressionInputForProperty(EMaterialProperty::MP_MaterialAttributes)->GetOutputId(), OutputId);
	EXPECT_EQ(Target->GetExpressionInputForProperty(EMaterialProperty::MP_MaterialAttributes)->GetExpressionId(), Pasted.value()[0]);
	const auto* PastedCall = FindExpression<DMaterialExpressionFunctionCall>(*Target, Pasted.value()[0]);
	ASSERT_NE(PastedCall, nullptr); EXPECT_EQ(PastedCall->Function.Get(), WeakFunction.Get());
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Target), Before);
	EXPECT_EQ(Target->GetExpressionCollection().Expressions.size(), 0u);
	ASSERT_TRUE(Transactions->Redo());
	const auto FunctionBefore = CaptureExpressions(*WeakFunction.Get());
	TStrongObjectPtr<DMaterialGraph> WeakFunctionEditorGraph(DMaterialGraph::Create(*WeakFunction.Get()));
	EXPECT_FALSE(WeakFunctionEditorGraph->Paste(Payload, 0, 0, false));
	EXPECT_EQ(CaptureExpressions(*WeakFunction.Get()), FunctionBefore);
	Transactions->Reset();
	Payload = {};
	RetainedTarget.Reset();
	MarkAsGarbage(Target);
	MarkAsGarbage(WeakFunction.Get());
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, FunctionClipboardCopiesPortsAndSurfaceBindingsAtomically)
{
	InitializeDObjectSystem();
	auto* Source = NewObject<DMaterialFunction>(nullptr, "ClipboardPortSource");
	auto* Target = NewObject<DMaterialFunction>(nullptr, "ClipboardPortTarget");
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Source));
	FMaterialFunctionPort Value{.Name = "Amount", .Default = {.Kind = EMaterialFunctionDefaultKind::Numeric}};
	const auto Input = AuthoringGraph->AddPort(false, Value, {}, 0, 0, false).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Input);
	const auto OriginalInputId = Source->GetFunctionSignature().Inputs.back().Id;
	ASSERT_TRUE(AuthoringGraph->AddPort(false, {.Name = "Follow", .Default = {.Kind = EMaterialFunctionDefaultKind::Input, .InputId = OriginalInputId}}, {}, 0, 0, false));
	const auto SurfaceId = Source->GetExpressionCollection().Expressions.front()->Id;
	auto SetExpression = Testing::MakeGraphExpression<DMaterialExpressionSetMaterialAttributes>();
	SetExpression->Surface = {Source->GetExpressionCollection().Expressions.front().Get()};
	SetExpression->Attributes = {{EMaterialProperty::MP_Metallic, {FindMaterialExpression(Source->GetExpressionCollection().Expressions, Input.value())}}};
	const auto Set = AuthoringGraph->CreateExpression(*SetExpression.Get(), 0, 0, false).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Set);
	ASSERT_TRUE(AuthoringGraph->AddPort(true, {.Type = EMaterialProgramValueType::MaterialAttributes, .Name = "Modified"}, {Set.value()}, 0, 0, false));
	std::vector<FGuid> Selection;
	for (const auto& Node : Source->GetExpressionCollection().Expressions) Selection.push_back(Node->Id);
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(AuthoringGraph->CopySelection(Selection, Payload));
	const auto Before = CaptureExpressions(*Target);
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	TStrongObjectPtr<DMaterialGraph> TargetEditorGraph(DMaterialGraph::Create(*Target));
	const auto Pasted = TargetEditorGraph->Paste(Payload, 400, 100, true).transform(Testing::NodeIds);
	ASSERT_TRUE(Pasted) << FormatMaterialGraphCommandResult(Pasted);
	const auto& Graph = CaptureExpressions(*Target);
	EXPECT_EQ(Graph.Signature.Inputs.size(), 4u);
	const auto Follow = std::ranges::find(Graph.Signature.Inputs, std::string("Follow"), &FMaterialFunctionPort::Name);
	ASSERT_NE(Follow, Graph.Signature.Inputs.end());
	EXPECT_NE(Follow->Default.InputId, OriginalInputId);
	EXPECT_NE(std::ranges::find(Graph.Signature.Inputs, Follow->Default.InputId, &FMaterialFunctionPort::Id), Graph.Signature.Inputs.end());
	EXPECT_EQ(Graph.Signature.Outputs.size(), 3u);
	std::unordered_set<FGuid> NewIds(Pasted.value().begin(), Pasted.value().end());
	for (const auto& Node : Graph.Expressions)
		if (NewIds.contains(Node->Id))
			VisitMaterialExpressionInputs(*Node.Get(), [&](uint32, FMaterialExpressionInput& Link) {
				if (Link.IsConnected()) EXPECT_TRUE(NewIds.contains(Link.GetExpressionId()));
			});
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Target), Before);
	ASSERT_TRUE(Transactions->Redo());
	FMaterialGraphClipboardPayload Cut;
	ASSERT_TRUE(TargetEditorGraph->CutSelection(Pasted.value(), Cut, true));
	EXPECT_EQ(CaptureExpressions(*Target), Before);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Target->GetFunctionSignature().Outputs.size(), 3u);
	Transactions->Reset();
	MarkAsGarbage(Source); MarkAsGarbage(Target); CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, CommandsAreAtomicAndTransactionsRestoreSemanticAndPresentationState)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "GraphOperationsMaterial");
	ASSERT_NE(Material, nullptr);
	const auto OriginalGraph = CaptureExpressions(*Material);
	const uint64 OriginalRevision =
		Material->GetMaterialCompileDebugInfo().AuthoredRevision;
	Durin::Tests::FTestEditorTransactorOwner Transactions;

	const auto Created = Testing::CreateGraphConstant(*DMaterialGraph::Create(*Material), 0.25f, 120, -80, true);
	ASSERT_TRUE(Created) << FormatMaterialGraphCommandResult(Created);

	const FGuid CreatedId = Created.value();
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(),
		OriginalGraph.Expressions.size() + 1);
	EXPECT_GT(Material->GetMaterialCompileDebugInfo().AuthoredRevision,
		OriginalRevision);
	const uint64 SemanticRevision =
		Material->GetMaterialCompileDebugInfo().AuthoredRevision;

	const FMaterialGraphNodePresentation Moved{CreatedId, 320, 160};
	TStrongObjectPtr<DMaterialGraph> EditorGraph(DMaterialGraph::Create(*Material));
	ASSERT_TRUE(EditorGraph->MoveNodes(std::span(&Moved, 1)));
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision,
		SemanticRevision);
	const FMaterialGraphView MovedGraph = Testing::CaptureGraphView(*EditorGraph);
	const FMaterialGraphNodeView* MovedView = FindViewNode(MovedGraph, CreatedId);
	ASSERT_NE(MovedView, nullptr);
	EXPECT_EQ(MovedView->Presentation.X, 320);

	const auto BeforeRejected = CaptureExpressions(*Material);
	ASSERT_NE(FindExpression<DMaterialExpressionScalarConstant>(*Material, CreatedId), nullptr);
	const FMaterialGraphCommandResult Rejected = EditorGraph->FindNode(CreatedId)->SetConstantValue(FMaterialParameterValue::MakeScalar(std::numeric_limits<float>::quiet_NaN()));
	EXPECT_EQ(Rejected.GetStatus(), EMaterialGraphCommandStatus::Rejected);
	EXPECT_FALSE(Rejected.Diagnostics.empty());
	EXPECT_EQ(CaptureExpressions(*Material), BeforeRejected);
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision,
		SemanticRevision);

	ASSERT_TRUE(Transactions->Undo());
	const FMaterialGraphView UnmovedGraph = Testing::CaptureGraphView(*EditorGraph);
	const FMaterialGraphNodeView* UnmovedView = FindViewNode(UnmovedGraph, CreatedId);
	ASSERT_NE(UnmovedView, nullptr);
	EXPECT_EQ(UnmovedView->Presentation.X, 120);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), OriginalGraph);
	ASSERT_TRUE(Transactions->Redo());
	const FMaterialGraphView RecreatedGraph = Testing::CaptureGraphView(*EditorGraph);
	EXPECT_NE(FindViewNode(RecreatedGraph, CreatedId), nullptr);
	ASSERT_TRUE(Transactions->Redo());
	const FMaterialGraphView RemovedGraph = Testing::CaptureGraphView(*EditorGraph);
	const FMaterialGraphNodeView* RedoneView = FindViewNode(RemovedGraph, CreatedId);
	ASSERT_NE(RedoneView, nullptr);
	EXPECT_EQ(RedoneView->Presentation.X, 320);

	FMaterialGraphMoveSession MoveSession;
	ASSERT_TRUE(MoveSession.Begin(*EditorGraph, std::span(&CreatedId, 1), Transactions.Get()));
	const FMaterialGraphNodePresentation FirstPreview{CreatedId, 400, 200};
	const FMaterialGraphNodePresentation SecondPreview{CreatedId, 480, 240};
	ASSERT_TRUE(MoveSession.Apply(std::span(&FirstPreview, 1)));
	ASSERT_TRUE(MoveSession.Apply(std::span(&SecondPreview, 1)));
	ASSERT_TRUE(MoveSession.Commit());
	ASSERT_TRUE(Transactions->Undo());
	const FMaterialGraphView CoalescedUndoGraph = Testing::CaptureGraphView(*EditorGraph);
	const FMaterialGraphNodeView* CoalescedUndo = FindViewNode(
		CoalescedUndoGraph, CreatedId);
	ASSERT_NE(CoalescedUndo, nullptr);
	EXPECT_EQ(CoalescedUndo->Presentation.X, 320);
	Transactions->Reset();
	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests,
	MovePreviewRejectsSemanticChangesAndRestoresOnlyItsPositions)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "StaleGraphMove");
	ASSERT_NE(Material, nullptr);

	const auto Created = Testing::CreateGraphConstant(*DMaterialGraph::Create(*Material), 0.f, 40, 80);
	ASSERT_TRUE(Created) << FormatMaterialGraphCommandResult(Created);

	const FGuid NodeId = Created.value();

	FMaterialGraphMoveSession MoveSession;
	TStrongObjectPtr<DMaterialGraph> EditorGraph(DMaterialGraph::Create(*Material));
	ASSERT_TRUE(MoveSession.Begin(*EditorGraph, std::span(&NodeId, 1)));
	const FMaterialGraphNodePresentation Preview{NodeId, 400, 240};
	ASSERT_TRUE(MoveSession.Apply(std::span(&Preview, 1)));

	auto Validation = GraphEditInternals::WriteInputDefault(Material, EMaterialProperty::MP_Roughness, {.Kind = EMaterialInputDefaultKind::Literal, .Type = EMaterialProgramValueType::Float, .Literal = {.X = .37f}}, false);
	ASSERT_TRUE(Validation);
	const uint64 SemanticRevision =
		Material->GetMaterialCompileDebugInfo().AuthoredRevision;

	const FMaterialGraphNodePresentation StalePreview{NodeId, 640, 360};
	EXPECT_FALSE(MoveSession.Apply(std::span(&StalePreview, 1)));
	EXPECT_FALSE(MoveSession.IsActive());
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision,
		SemanticRevision);
	EXPECT_FLOAT_EQ(Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->Constant[0],
		0.37f);
	const FMaterialGraphView RestoredView =
		Testing::CaptureGraphView(*EditorGraph);
	const FMaterialGraphNodeView* Restored = FindViewNode(RestoredView, NodeId);
	ASSERT_NE(Restored, nullptr);
	EXPECT_EQ(Restored->Presentation.X, 40);
	EXPECT_EQ(Restored->Presentation.Y, 80);

	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, RootCommandsRespectExtendedPropertiesAndSpatialSemantics)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	using P = EMaterialProperty;
	EXPECT_FALSE(MaterialEditing::AddTextureToSurfaceOutput(*Material, {.Output = P::MP_WorldPositionOffset}, false));
	EXPECT_FALSE(MaterialEditing::PromoteSurfaceOutputToParameter(*Material, {.Output = P::MP_Normal}, false));
	EXPECT_TRUE(Material->GetExpressionCollection().Expressions.empty());
	for (const auto Property : {P::MP_WorldPositionOffset, P::MP_Specular, P::MP_ClearCoat, P::MP_ClearCoatRoughness})
	{
		SCOPED_TRACE(uint32(Property));
		ASSERT_TRUE(MaterialEditing::PromoteSurfaceOutputToParameter(*Material, {.Output = Property}, false));
		EXPECT_TRUE(Material->GetExpressionInputForProperty(Property)->IsConnected());
		EXPECT_FALSE(MaterialEditing::CanPromoteSurfaceOutput(*Material, Property));
	}
	EXPECT_TRUE(Normalize(*Material));
}

TEST(FMaterialGraphOperationsTests,
	MaterialOutputPromotionTextureAndDisconnectAreAtomic)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "MaterialOutputCommands");
	ASSERT_NE(Material, nullptr);
	ASSERT_TRUE(Material->SetMaterialExpressions({}, {}));
	ASSERT_EQ(Material->GetExpressionCollection().Expressions.size(), 0u);
	Durin::Tests::FTestEditorTransactorOwner Transactions;

	FMaterialProgramLiteral EditedBaseColor{0.2f, 0.3f, 0.4f, 0.0f};
	TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
	const auto Root = EMaterialProperty::MP_BaseColor;
	ASSERT_TRUE(Graph->GetRootNode()->PinDefaultValueChanged(Testing::ResolvePin(*Graph->GetRootNode(), Root), {.Kind = EMaterialInputDefaultKind::Literal, .Type = EMaterialProgramValueType::Float3, .Literal = EditedBaseColor}));
	const auto Promoted = MaterialEditing::PromoteSurfaceOutputToParameter(*Material, {
			.Output = EMaterialProperty::MP_BaseColor,
			.X = 100,
			.Y = 200}).transform(Testing::ExpressionNodeId);
	ASSERT_TRUE(Promoted) << FormatMaterialGraphCommandResult(Promoted);

	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 2u);
	EXPECT_TRUE(Material->GetNumericInputForProperty(EMaterialProperty::MP_BaseColor)->Connection.IsConnected());
	const auto* PromotedParameter = FindExpression<DMaterialExpressionParameter>(*Material, Promoted.value());
	ASSERT_NE(PromotedParameter, nullptr);
	const auto ParameterId = PromotedParameter->Metadata.Id;
	FVector4 BaseColor;
	ASSERT_TRUE(ReadVector4Parameter(*Material,
		Material->FindParameterDefinition(ParameterId)->Name, BaseColor));
	EXPECT_NEAR(BaseColor.x, 0.2, 1.e-6);
	EXPECT_NEAR(BaseColor.y, 0.3, 1.e-6);
	EXPECT_NEAR(BaseColor.z, 0.4, 1.e-6);
	const auto PromotedGraph = Normalize(*Material);
	ASSERT_TRUE(PromotedGraph);
	const auto& PromotedDependencies = PromotedGraph.ActiveParameters;
	ASSERT_EQ(PromotedDependencies.size(), 1u);
	EXPECT_EQ(PromotedDependencies.front().Id,
		Material->FindParameterDefinition(ParameterId)->Name.ToString());
	const uint64 CompileGeneration =
		Material->GetMaterialCompileDebugInfo().RequestId;
	ASSERT_TRUE(Testing::WritePreviewParameter(*Material, ParameterId, FVector4(0.7, 0.6, 0.5, 0)));
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().RequestId,
		CompileGeneration);
	ASSERT_TRUE(ReadVector4Parameter(*Material,
		Material->FindParameterDefinition(ParameterId)->Name, BaseColor));
	EXPECT_EQ(BaseColor, FVector4(0.7, 0.6, 0.5, 0));
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(ReadVector4Parameter(*Material,
		Material->FindParameterDefinition(ParameterId)->Name, BaseColor));
	EXPECT_NEAR(BaseColor.x, 0.2, 1.e-6);
	EXPECT_NEAR(BaseColor.y, 0.3, 1.e-6);
	EXPECT_NEAR(BaseColor.z, 0.4, 1.e-6);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Transactions->Undo());

	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 0u);
	EXPECT_TRUE(Material->GetParameterDefinitions().empty());
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Testing::WriteConnection(&*Material, EMaterialProperty::MP_BaseColor, FMaterialProgramLink{}, true));
	EXPECT_FALSE(Material->GetNumericInputForProperty(EMaterialProperty::MP_BaseColor)->Connection.IsConnected());
	EXPECT_EQ(ReadMaterialNumericDefault(*Material->GetNumericInputForProperty(EMaterialProperty::MP_BaseColor), EMaterialProperty::MP_BaseColor),
		(std::vector<float>{EditedBaseColor.X, EditedBaseColor.Y, EditedBaseColor.Z}));
	const auto DisconnectedGraph = Normalize(*Material);
	ASSERT_TRUE(DisconnectedGraph);
	EXPECT_TRUE(DisconnectedGraph.ActiveParameters.empty());

	ASSERT_TRUE(FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	const auto Revision = [&] { return Material->GetMaterialCompileDebugInfo().AuthoredRevision; };
	const auto BeforeTexture = Revision();
	const auto Textured = MaterialEditing::AddTextureToSurfaceOutput(*Material, {
			.Output = EMaterialProperty::MP_Normal,
			.X = 400,
			.Y = 200}).transform(Testing::ExpressionNodeId);
	ASSERT_TRUE(Textured) << FormatMaterialGraphCommandResult(Textured);

	EXPECT_EQ(Revision(), BeforeTexture + 1);
	EXPECT_EQ(Material->GetNumericInputForProperty(EMaterialProperty::MP_Normal)->Connection.OutputIndex, 1u);
	const MIR::FNormalizationResult Normalized = Normalize(*Material);
	ASSERT_TRUE(Normalized);
	EXPECT_EQ(std::ranges::count(Normalized.IR.Nodes, EMaterialProgramOpcode::TextureSample2D,
		&MIR::FNode::Opcode), 1);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FALSE(Material->GetNumericInputForProperty(EMaterialProperty::MP_Normal)->Connection.IsConnected());
	EXPECT_EQ(Revision(), BeforeTexture + 2);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Revision(), BeforeTexture + 3);
	const auto& Restored = Material->GetNumericInputForProperty(EMaterialProperty::MP_Normal)->Connection;
	ASSERT_TRUE(Restored.IsConnected());
	EXPECT_EQ(Restored.Expression->Id, Textured.value());
	EXPECT_EQ(Restored.OutputIndex, 1);
	Transactions->Reset();
	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, TexturePropertyHistoryRetainsOriginalAcrossCollection)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "TextureSessionMaterial"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	auto* Original = NewObject<DTexture2D>(nullptr, "TextureSessionOriginal");
	auto* Replacement = NewObject<DTexture2D>(nullptr, "TextureSessionReplacement");
	TWeakObjectPtr<DTexture2D> WeakOriginal(Original);
	const FGuid Id = FGuid::NewGuid();
	ASSERT_TRUE(MaterialEditing::CreateParameter(*Material, {.Id = Id,
		.Name = FName("Texture"), .Type = EMaterialParameterType::Texture,
		.Value = FMaterialParameterValue::MakeTexture(Original)}, false));
	Tests::FTestEditorTransactorOwner History;
	FMaterialEditorPreviewParameters Parameters(Material);
	auto Preview = Parameters.Find(Id);
	ASSERT_TRUE(Preview);
	const auto Handle = PropertyEditor::MakePropertyHandle(Preview->GetEditTarget());
	ASSERT_EQ(Handle->SetValue(FMaterialParameterValue::MakeTexture(Replacement).GetTexture()), PropertyEditor::FPropertyAccess::Success);
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(WeakOriginal.IsValid());
	ASSERT_TRUE(History->Undo());
	EXPECT_EQ(Material->FindParameterDefinition(Id)->Value.GetTexture().Texture.Get(), WeakOriginal.Get());
	Material.Reset();
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests,
	ParameterEditSessionPublishesOnlyCommittedValueWithoutCompiling)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "InteractiveParameterMaterial");
	ASSERT_NE(Material, nullptr);
	TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
	const auto Root = EMaterialProperty::MP_BaseColor;
	ASSERT_TRUE(Graph->GetRootNode()->PinDefaultValueChanged(Testing::ResolvePin(*Graph->GetRootNode(), Root), {.Kind = EMaterialInputDefaultKind::Literal, .Type = EMaterialProgramValueType::Float3, .Literal = {0.2f, 0.3f, 0.4f, 0.0f}}, false));
	ASSERT_TRUE(MaterialEditing::PromoteSurfaceOutputToParameter(*Material, {.Output = EMaterialProperty::MP_BaseColor}, false));

	const FGuid ParameterId = Material->GetParameterDefinitions().front().Id;
	const uint64 CompileGeneration =
		Material->GetMaterialCompileDebugInfo().RequestId;
	FVector4 OriginalBaseColor;
	ASSERT_TRUE(ReadVector4Parameter(*Material,
		Material->FindParameterDefinition(ParameterId)->Name, OriginalBaseColor));
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	FMaterialEditorPreviewParameters Parameters(Material);
	auto Preview = Parameters.Find(ParameterId);
	ASSERT_TRUE(Preview);
	PropertyEditor::FPropertyControl Session;
	ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Preview->GetEditTarget()), true));
	ASSERT_TRUE(Durin::Testing::ApplyControlInput(Session, FVector4{0.4, 0.5, 0.6, 0}));
	FVector4 BaseColor;
	ASSERT_TRUE(ReadVector4Parameter(*Material,
		Material->FindParameterDefinition(ParameterId)->Name, BaseColor));
	EXPECT_EQ(BaseColor, OriginalBaseColor);
	EXPECT_EQ(Preview->GetValue().GetVector4(), FVector4(0.4, 0.5, 0.6, 0));
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().RequestId,
		CompileGeneration);
	ASSERT_TRUE(Durin::Testing::ApplyControlInput(Session, FVector4{0.7, 0.8, 0.9, 0}));
	ASSERT_TRUE(ReadVector4Parameter(*Material,
		Material->FindParameterDefinition(ParameterId)->Name, BaseColor));
	EXPECT_EQ(BaseColor, OriginalBaseColor);
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().RequestId,
		CompileGeneration);
	ASSERT_TRUE(Session.Finish(false));
	ASSERT_TRUE(Graph->MoveNodes(std::array{FMaterialGraphNodePresentation{Graph->GetRootNode()->GetView().Node.Id, 713, -91}}, false));

	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(ReadVector4Parameter(*Material,
		Material->FindParameterDefinition(ParameterId)->Name, BaseColor));
	EXPECT_EQ(BaseColor, OriginalBaseColor);
	EXPECT_EQ(Material->GetRootPosition().first, 713);
	EXPECT_EQ(Material->GetRootPosition().second, -91);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(ReadVector4Parameter(*Material,
		Material->FindParameterDefinition(ParameterId)->Name, BaseColor));
	EXPECT_EQ(BaseColor, FVector4(0.7, 0.8, 0.9, 0));

	Parameters.RegenerateArrays();
	Preview = Parameters.Find(ParameterId);
	ASSERT_TRUE(Preview);
	ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Preview->GetEditTarget()), true, true));
	ASSERT_TRUE(Durin::Testing::ApplyControlInput(Session, FVector4{0.1, 0.1, 0.1, 0}));
	ASSERT_TRUE(Session.Finish(true));
	ASSERT_TRUE(ReadVector4Parameter(*Material,
		Material->FindParameterDefinition(ParameterId)->Name, BaseColor));
	EXPECT_EQ(BaseColor, FVector4(0.7, 0.8, 0.9, 0));
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().RequestId,
		CompileGeneration);
	Transactions->Reset();
	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, MoveAndDefaultHistoryDoNotGrowWithUnrelatedNodes)
{
	InitializeDObjectSystem();
	std::optional<size_t> SmallMoveBytes, SmallDefaultBytes;
	for (uint32 UnrelatedCount : {0u, 128u})
	{
		TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		auto Add = Testing::MakeGraphExpression<DMaterialExpressionAdd>();
		const auto Created = DMaterialGraph::Create(*Material)->CreateExpression(*Add.Get(), 10, 20, false).transform(Testing::GraphNodeId);
		ASSERT_TRUE(Created);
		const auto NodeId = Created.value();
		std::vector<DMaterialExpression*> Nodes;
		for (auto& E : Material->GetExpressionCollection().Expressions) Nodes.push_back(E.Get());
		for (uint32 I = 0; I < UnrelatedCount; ++I)
		{
			auto* E = NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None);
			E->Id = FGuid::NewGuid(); Nodes.push_back(E);
		}
		ASSERT_TRUE(Material->SetMaterialExpressions(Nodes));
		Durin::Tests::FTestEditorTransactorOwner Transactions;
		const FMaterialGraphNodePresentation Moved{NodeId, 40, 80};
		TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
		ASSERT_TRUE(Graph->MoveNodes(std::span(&Moved, 1)));
		const auto MoveBytes = Transactions->GetUndoSize();
		EXPECT_GT(MoveBytes, 0u);
		if (SmallMoveBytes) EXPECT_EQ(MoveBytes, *SmallMoveBytes);
		else SmallMoveBytes = MoveBytes;
		Transactions->Reset();
		const auto Input = Testing::ExpressionInput(NodeId, 0);
		ASSERT_TRUE(Graph->FindNode(NodeId)->PinDefaultValueChanged(Testing::ResolvePin(*Graph->FindNode(NodeId), Input), {.Kind = EMaterialInputDefaultKind::Literal, .Type = EMaterialProgramValueType::Float, .Literal = {.41f, 0.f, 0.f, 0.f}}));
		const auto DefaultBytes = Transactions->GetUndoSize();
		EXPECT_GT(DefaultBytes, 0u);
		if (SmallDefaultBytes) EXPECT_EQ(DefaultBytes, *SmallDefaultBytes);
		else SmallDefaultBytes = DefaultBytes;
		ASSERT_TRUE(Transactions->Undo());
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_FLOAT_EQ(Testing::InputView(*Graph, Input)->RetainedConstant.Literal.X, .41f);
	}
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
	auto Node = Testing::MakeGraphExpression<DMaterialExpressionVector4Parameter>();
	Node->Metadata.Id = Definition.Id; Node->Metadata.Name = Definition.Name;
	Node->Metadata.DisplayName = Definition.DisplayName;
	Node->DefaultValue = Definition.Value.GetVector4();
	ASSERT_TRUE(Source->SetMaterialExpressions(std::array<DMaterialExpression*, 1>{Node.Get()}, {}));
	TStrongObjectPtr<DMaterialGraph> SourceEditorGraph(DMaterialGraph::Create(*Source));
	ASSERT_TRUE(SourceEditorGraph->Layout({}, false));
	ASSERT_TRUE(Target->SetMaterialExpressions({}, {}));
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(SourceEditorGraph->CopySelection(std::span(&Node->Id, 1), Payload));
	ASSERT_EQ(Payload.Nodes.size(), 1u);
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	const auto BeforeRevision = Target->GetMaterialCompileDebugInfo().AuthoredRevision;
	TStrongObjectPtr<DMaterialGraph> TargetEditorGraph(DMaterialGraph::Create(*Target));
	const auto Pasted = TargetEditorGraph->Paste(Payload, 300, 200);
	ASSERT_TRUE(Pasted) << FormatMaterialGraphCommandResult(Pasted);
	ASSERT_EQ(Target->GetParameterDefinitions().size(), 1u);
	const auto LocalId = Target->GetParameterDefinitions().front().Id;
	EXPECT_NE(LocalId, Definition.Id);
	EXPECT_EQ(Cast<DMaterialExpressionParameter>(Target->GetExpressionCollection().Expressions.front().Get())->Metadata.Id, LocalId);
	EXPECT_EQ(Target->GetMaterialCompileDebugInfo().AuthoredRevision, BeforeRevision + 1);
	const auto AfterPresentation = Target->GetMaterialGraphPresentation();
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_TRUE(Target->GetParameterDefinitions().empty());
	EXPECT_EQ(Target->GetExpressionCollection().Expressions.size(), 0u);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Target->GetParameterDefinitions().front().Id, LocalId);
	EXPECT_EQ(Target->GetMaterialGraphPresentation(), AfterPresentation);
	ASSERT_TRUE(TargetEditorGraph->Paste(Payload, 600, 200, false));
	EXPECT_EQ(Target->GetParameterDefinitions().size(), 1u);
	EXPECT_EQ(Cast<DMaterialExpressionParameter>(Target->GetExpressionCollection().Expressions.back().Get())->Metadata.Id, LocalId);

	const auto BeforeProgram = CaptureExpressions(*Target);
	const auto BeforePresentation = Target->GetMaterialGraphPresentation();
	const auto Revision = Target->GetMaterialCompileDebugInfo().AuthoredRevision;
	Cast<DMaterialExpressionParameter>(Payload.Nodes.front().Expression.Get())->Metadata.Name = {};
	EXPECT_FALSE(TargetEditorGraph->Paste(Payload, 0, 0, false));
	EXPECT_EQ(CaptureExpressions(*Target), BeforeProgram);
	EXPECT_EQ(Target->GetMaterialGraphPresentation(), BeforePresentation);
	EXPECT_EQ(Target->GetMaterialCompileDebugInfo().AuthoredRevision, Revision);
	Cast<DMaterialExpressionParameter>(Payload.Nodes.front().Expression.Get())->Metadata = {};
	EXPECT_FALSE(TargetEditorGraph->Paste(Payload, 0, 0, false));
	EXPECT_EQ(CaptureExpressions(*Target), BeforeProgram);
	MarkAsGarbage(Source);
	MarkAsGarbage(Target);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, SameRootClipboardCreatesIndependentSnapshotOwners)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "ClipboardRename");
	FMaterialParameterDefinition Definition;
	Definition.Id = FGuid::NewGuid();
	Definition.Name = "Amount";
	Definition.DisplayName = "Amount";
	auto Node = Testing::MakeGraphExpression<DMaterialExpressionScalarParameter>();
	Node->Metadata.Id = Definition.Id; Node->Metadata.Name = Definition.Name;
	Node->Metadata.DisplayName = Definition.DisplayName;
	Node->DefaultValue = Definition.Value.GetScalar();
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 1>{Node.Get()}, {}));
	TStrongObjectPtr<DMaterialGraph> MaterialEditorGraph(DMaterialGraph::Create(*Material));
	ASSERT_TRUE(MaterialEditorGraph->Layout({}, false));
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(MaterialEditorGraph->CopySelection(std::span(&Node->Id, 1), Payload));
	ASSERT_TRUE(MaterialEditing::RenameParameter(*Material, Definition.Id, "RenamedAmount", false));
	ASSERT_TRUE(MaterialEditorGraph->Paste(Payload, 400, 0, false));
	EXPECT_NE(Cast<DMaterialExpressionParameter>(Material->GetExpressionCollection().Expressions.back().Get())->Metadata.Id, Definition.Id);
	EXPECT_EQ(Cast<DMaterialExpressionParameter>(Material->GetExpressionCollection().Expressions.back().Get())->Metadata.Name, Definition.Name);
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 2u);
	ASSERT_TRUE(Material->SetMaterialExpressions({}, {}));
	EXPECT_TRUE(MaterialEditorGraph->Paste(Payload, 400, 0, false));
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 1u);
	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, DeclarationCommandsAndConstantPromotionShareAtomicHistory)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "DeclarationCommands");
	ASSERT_TRUE(Material->SetMaterialExpressions({}, {}));
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	FMaterialParameterDefinition Definition;
	Definition.Name = "Amount";
	Definition.DisplayName = "Amount";
	Definition.Value = FMaterialParameterValue::MakeScalar(0.25f);
	const auto Created = MaterialEditing::CreateParameter(*Material, Definition).transform(Testing::ExpressionNodeId);
	ASSERT_TRUE(Created);

	const auto* CreatedParameter = FindExpression<DMaterialExpressionParameter>(*Material, Created.value());
	ASSERT_NE(CreatedParameter, nullptr);
	const auto Id = CreatedParameter->Metadata.Id;
	ASSERT_TRUE(Id.IsValid());
	Definition.Value = FMaterialParameterValue::MakeScalar(0.75f);
	const auto Reused = MaterialEditing::CreateParameter(*Material, Definition).transform(Testing::ExpressionNodeId);
	ASSERT_TRUE(Reused);

	const auto* ReusedParameter = FindExpression<DMaterialExpressionParameter>(*Material, Reused.value());
	ASSERT_NE(ReusedParameter, nullptr);
	EXPECT_EQ(ReusedParameter->Metadata.Id, Id);
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 1u);
	EXPECT_EQ(Material->FindParameterDefinition(Id)->Value.GetScalar(), 0.25f);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_TRUE(Material->GetParameterDefinitions().empty());
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(MaterialEditing::RenameParameter(*Material, Id, "BlendAmount"));
	EXPECT_EQ(Material->FindParameterDefinition(Id)->Name, FName("BlendAmount"));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->FindParameterDefinition(Id)->Name, FName("Amount"));
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(MaterialEditing::DeleteParameter(*Material, Id));
	EXPECT_TRUE(Material->GetParameterDefinitions().empty());
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_NE(Material->FindParameterDefinition(Id), nullptr);
	Transactions->Reset();

	auto Constant = Testing::MakeGraphExpression<DMaterialExpressionVector4Constant>();
	Constant->Value = {1, 2, 3, 4};
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 1>{Constant.Get()}, {}));
	const auto Original = CaptureExpressions(*Material);
	const auto Revision = Material->GetMaterialCompileDebugInfo().AuthoredRevision;
	const auto Promoted = MaterialEditing::PromoteConstantToParameter(*Material, Constant->Id, "Tint");
	ASSERT_TRUE(Promoted) << FormatMaterialGraphCommandResult(Promoted);

	const auto* Tint = FindExpression<DMaterialExpressionParameter>(*Material, Constant->Id);
	ASSERT_NE(Tint, nullptr);
	const auto TintId = Tint->Metadata.Id;
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision + 1);
	EXPECT_EQ(Material->FindParameterDefinition(TintId)->Value.GetVector4(), FVector4(1, 2, 3, 4));
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.front()->Id, Constant->Id);
	EXPECT_EQ(Material->FindParameterDefinition("Tint")->Id, TintId);
	ASSERT_TRUE(MaterialEditing::DeleteParameter(*Material, TintId));
	EXPECT_TRUE(Material->GetParameterDefinitions().empty());
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->FindParameterDefinition(TintId), nullptr);
	EXPECT_EQ(CaptureExpressions(*Material), Original);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Material->FindParameterDefinition("Tint")->Id, TintId);
	Transactions->Reset();
	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
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
	auto Node = Testing::MakeGraphExpression<DMaterialExpressionTextureParameter>();
	Node->Metadata.Id = Definition.Id; Node->Metadata.Name = Definition.Name;
	Node->Metadata.DisplayName = Definition.DisplayName;
	Node->DefaultValue = Definition.Value.GetTexture();
	ASSERT_TRUE(Source->SetMaterialExpressions(std::array<DMaterialExpression*, 1>{Node.Get()}, {}));
	TStrongObjectPtr<DMaterialGraph> SourceEditorGraph(DMaterialGraph::Create(*Source));
	ASSERT_TRUE(SourceEditorGraph->Layout({}, false));
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(SourceEditorGraph->CopySelection(std::span(&Node->Id, 1), Payload));
	Node.Reset();
	MarkAsGarbage(Source);
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_NE(WeakTexture.Get(), nullptr);
	EXPECT_EQ(Payload.SourceRoot.Get(), nullptr);
	DMaterial* Target = NewObject<DMaterial>(nullptr, "ClipboardTextureTarget");
	ASSERT_TRUE(Target->SetMaterialExpressions({}, {}));
	TStrongObjectPtr<DMaterialGraph> TargetEditorGraph(DMaterialGraph::Create(*Target));
	ASSERT_TRUE(TargetEditorGraph->Paste(Payload, 0, 0, false));
	EXPECT_EQ(Target->GetParameterDefinitions().front().Value.GetTexture().Texture.Get(), WeakTexture.Get());
	MarkAsGarbage(Target);
	CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_NE(WeakTexture.Get(), nullptr);
	Payload = {};
	CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(WeakTexture.Get(), nullptr);
}

TEST(FMaterialGraphOperationsTests, TypedHistoryRestoresDeletedChildrenWithoutSharingReplayState)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "TypedHistoryMaterial"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialExpressionScalarParameter> Parameter(
		NewObject<DMaterialExpressionScalarParameter>(nullptr, "HistoryParameter"));
	Parameter->Id = FGuid::NewGuid();
	Parameter->Metadata.Id = FGuid::NewGuid();
	Parameter->Metadata.Name = FName("HistoryValue");
	Parameter->DefaultValue = 0.37f;
	Parameter->bHasRange = true;
	Parameter->MinimumValue = 0.1f;
	Parameter->MaximumValue = 0.9f;
	DMaterialExpression* Expressions[] = {Parameter.Get()};
	ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Material->CaptureRootInputs()));
	const FGuid Id = Parameter->Id;
	TWeakObjectPtr<DMaterialExpression> Deleted(Material->GetExpressionCollection().Expressions[0].Get());
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	ASSERT_TRUE(DMaterialGraph::Create(*Material)->RemoveNodes(std::span(&Id, 1)));
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 0u);
	Parameter.Reset();
	CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_FALSE(Deleted.IsValid());
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
	auto* Restored = Cast<DMaterialExpressionScalarParameter>(Material->GetExpressionCollection().Expressions[0].Get());
	ASSERT_NE(Restored, nullptr);
	EXPECT_EQ(Restored->GetOuter(), Material.Get());
	EXPECT_EQ(Restored->Id, Id);
	EXPECT_FLOAT_EQ(Restored->DefaultValue, 0.37f);
	EXPECT_FLOAT_EQ(Restored->MinimumValue, 0.1f);
	EXPECT_FLOAT_EQ(Restored->MaximumValue, 0.9f);
	// Redo captures the live inverse, including independent edits on the restored object.
	Restored->DefaultValue = 0.72f;
	Restored->Metadata.DisplayName = "Live edit";
	ASSERT_TRUE(Transactions->Redo());
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(Transactions->Undo());
	Restored = Cast<DMaterialExpressionScalarParameter>(Material->GetExpressionCollection().Expressions[0].Get());
	ASSERT_NE(Restored, nullptr);
	EXPECT_FLOAT_EQ(Restored->DefaultValue, 0.72f);
	EXPECT_EQ(Restored->Metadata.DisplayName, "Live edit");
	Transactions->Reset();
	CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(Material->GetExpressionCollection().Expressions[0].Get(), Restored);
	Material.Reset();
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, DeletingSurfaceOverrideSourceRestoresBaseAndSupportsUndo)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "SurfaceDelete"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialExpressionMakeMaterialAttributes> Base(NewObject<DMaterialExpressionMakeMaterialAttributes>(nullptr, NAME_None));
	Base->Id = FGuid::NewGuid();
	TStrongObjectPtr<DMaterialExpressionScalarConstant> Value(NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None));
	Value->Id = FGuid::NewGuid(); Value->Value = .7f;
	TStrongObjectPtr<DMaterialExpressionSetMaterialAttributes> Surface(NewObject<DMaterialExpressionSetMaterialAttributes>(nullptr, NAME_None));
	Surface->Id = FGuid::NewGuid(); Surface->Surface = {Base.Get()};
	Surface->Attributes = {{EMaterialProperty::MP_Roughness, {Value.Get()}}};
	const std::array<DMaterialExpression*, 3> Expressions{Base.Get(), Value.Get(), Surface.Get()};
	FMaterialRootInputs Outputs; Outputs.Attributes = {Surface.Get()}; Outputs.bUseMaterialAttributes = true;
	ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
	Tests::FTestEditorTransactorOwner Transactions;
	const auto DeletedId = Value->Id;
	ASSERT_TRUE(DMaterialGraph::Create(*Material)->RemoveNodes(std::span(&DeletedId, 1)));
	const auto* Remaining = Cast<DMaterialExpressionSetMaterialAttributes>(Material->GetExpressionCollection().Expressions.back().Get());
	ASSERT_NE(Remaining, nullptr);
	ASSERT_EQ(Remaining->Attributes.size(), 1u);
	EXPECT_FALSE(Remaining->Attributes.front().Input.IsConnected());
	EXPECT_EQ(Remaining->Surface.GetExpressionId(), Base->Id);
	ASSERT_TRUE(Transactions->Undo());
	Remaining = Cast<DMaterialExpressionSetMaterialAttributes>(Material->GetExpressionCollection().Expressions.back().Get());
	ASSERT_EQ(Remaining->Attributes.size(), 1u);
	EXPECT_EQ(Remaining->Attributes.front().Input.GetExpressionId(), DeletedId);
	Transactions->Reset(); Material.Reset();
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, SharedParametersSynchronizeRebindAndUndo)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "SharedParameterCommands");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	Durin::Tests::FTestEditorTransactorOwner Transactions;

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	FMaterialParameterDefinition Definition;
	Definition.Name = "Amount"; Definition.Value = FMaterialParameterValue::MakeScalar(.25f);
	const auto A = MaterialEditing::CreateParameter(*Material, Definition, false).transform(Testing::ExpressionNodeId);
	ASSERT_TRUE(A);

	const auto* CreatedParameter = FindExpression<DMaterialExpressionParameter>(*Material, A.value());
	ASSERT_NE(CreatedParameter, nullptr);
	const auto Id = CreatedParameter->Metadata.Id;
	const auto B = AuthoringGraph->DuplicateNodes(std::array{A.value()}, 100, 0, false).transform(Testing::NodeIds);
	ASSERT_TRUE(B) << FormatMaterialGraphCommandResult(B);
	ASSERT_EQ(Material->GetParameterDefinitions().size(), 1u);
	ASSERT_TRUE(Testing::WriteConnection(Material, EMaterialProperty::MP_Metallic, FMaterialProgramLink{A.value()}, true, false));
	ASSERT_TRUE(Testing::WriteConnection(Material, EMaterialProperty::MP_Roughness, FMaterialProgramLink{B->front()}, true, false));
	std::vector<DMaterialExpression*> Expressions;
	for (const auto& E : Material->GetExpressionCollection().Expressions) Expressions.push_back(E.Get());
	const auto Built = MIR::FGraphBuilder(Expressions).FinishSurface(Material->CaptureRootInputs());
	ASSERT_TRUE(Built.Diagnostics.empty());
	ASSERT_EQ(Built.Parameters.size(), 1u);
	EXPECT_EQ(Built.Parameters.front().Id, Material->FindParameterDefinition(Id)->Name.ToString());
	auto* Instance = NewObject<DMaterialInstance>(nullptr, "SharedParameterInstance");
	ASSERT_TRUE(Instance->SetParent(Material));
	ASSERT_TRUE(Instance->SetParameterValueByInfo({.Name = CreatedParameter->Metadata.Name}, FMaterialParameterValue::MakeScalar(.8f)));
	ASSERT_TRUE(Testing::WritePreviewParameter(*Material, Id, .6f));
	ASSERT_EQ(Transactions->GetQueueLength(), 1u);
	EXPECT_EQ(Transactions->GetTransaction(0)->GetRecordCount(), 3u);
	for (const auto& E : Material->GetExpressionCollection().Expressions)
		if (const auto* Parameter = Cast<DMaterialExpressionScalarParameter>(E.Get())) EXPECT_FLOAT_EQ(Parameter->DefaultValue, .6f);
	ASSERT_TRUE(Transactions->Undo());
	for (const auto& E : Material->GetExpressionCollection().Expressions)
		if (const auto* Parameter = Cast<DMaterialExpressionScalarParameter>(E.Get())) EXPECT_FLOAT_EQ(Parameter->DefaultValue, .25f);
	ASSERT_TRUE(Transactions->Redo());

	TStrongObjectPtr<DMaterialExpressionScalarParameter> Edit(Cast<DMaterialExpressionScalarParameter>(
		DuplicateObject(Material->GetExpressionCollection().Expressions.back().Get(), nullptr, NAME_None).value()));
	Edit->Metadata.GroupName = "Shared Group";
	ASSERT_TRUE(AuthoringGraph->ReplaceExpression(*Edit.Get()));
	EXPECT_EQ(Cast<DMaterialExpressionParameter>(Material->GetExpressionCollection().Expressions.front().Get())->Metadata.GroupName, FName("Shared Group"));
	Edit->Metadata.Name = "OtherAmount";
	ASSERT_TRUE(AuthoringGraph->ReplaceExpression(*Edit.Get()));
	ASSERT_EQ(Material->GetParameterDefinitions().size(), 2u);
	const auto OtherId = Material->FindParameterDefinition("OtherAmount")->Id;
	EXPECT_NE(OtherId, Id);
	Edit->Metadata.Name = "Amount";
	Edit->DefaultValue = .1f;
	ASSERT_TRUE(AuthoringGraph->ReplaceExpression(*Edit.Get()));
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 1u);
	EXPECT_FLOAT_EQ(Cast<DMaterialExpressionScalarParameter>(Material->GetExpressionCollection().Expressions.back().Get())->DefaultValue, .6f);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_NE(Material->FindParameterDefinition(OtherId), nullptr);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(MaterialEditing::RenameParameter(*Material, Id, "RenamedAmount"));
	FResolvedMaterialParameter Resolved;
	ASSERT_TRUE(Instance->ResolveParameterValueByInfo(Instance->FindParameterDefinition(Id)->GetParameterInfo(), Resolved));
	EXPECT_FLOAT_EQ(Resolved.Value.GetScalar(), .8f);
	EXPECT_EQ(Resolved.Definition->Name, FName("RenamedAmount"));
	ASSERT_TRUE(AuthoringGraph->RemoveNodes(B.value()));
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 1u);
	ASSERT_TRUE(AuthoringGraph->RemoveNodes(std::array{A.value()}));
	EXPECT_TRUE(Material->GetParameterDefinitions().empty());
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_NE(Material->FindParameterDefinition(Id), nullptr);
	Transactions->Reset();
	MarkAsGarbage(Instance); MarkAsGarbage(Material); CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, SharedParameterValidationAndForeignPasteAreAtomic)
{
	InitializeDObjectSystem();
	auto* Source = NewObject<DMaterial>(nullptr, "SharedPasteSource");
	auto* Target = NewObject<DMaterial>(nullptr, "SharedPasteTarget");
	Source->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	Target->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	auto A = Testing::MakeGraphExpression<DMaterialExpressionScalarParameter>();
	A->Metadata = {FGuid::NewGuid(), "Amount"}; A->DefaultValue = .4f;
	auto B = Testing::MakeGraphExpression<DMaterialExpressionScalarParameter>();
	B->Metadata = A->Metadata; B->DefaultValue = .4f;
	const std::array<DMaterialExpression*, 2> Nodes{A.Get(), B.Get()};
	ASSERT_TRUE(Source->SetMaterialExpressions(Nodes, {}));
	TStrongObjectPtr<DMaterialGraph> SourceEditorGraph(DMaterialGraph::Create(*Source));
	ASSERT_TRUE(SourceEditorGraph->Layout({}, false));
	const auto Before = CaptureExpressions(*Source);
	B->DefaultValue = .7f;
	EXPECT_FALSE(Source->SetMaterialExpressions(Nodes, {}));
	EXPECT_FALSE(MIR::FGraphBuilder(Nodes).FinishSurface({}).Diagnostics.empty());
	EXPECT_EQ(CaptureExpressions(*Source), Before);
	B->DefaultValue = .4f;
	B->Metadata.Id = FGuid::NewGuid();
	EXPECT_FALSE(Source->SetMaterialExpressions(Nodes, {}));
	FMaterialGraphClipboardPayload Payload;
	const std::array Ids{A->Id, B->Id};
	ASSERT_TRUE(SourceEditorGraph->CopySelection(Ids, Payload));
	TStrongObjectPtr<DMaterialGraph> TargetEditorGraph(DMaterialGraph::Create(*Target));
	ASSERT_TRUE(TargetEditorGraph->Paste(Payload, 0, 0, false));
	ASSERT_EQ(Target->GetParameterDefinitions().size(), 1u);
	const auto TargetId = Target->GetParameterDefinitions().front().Id;
	EXPECT_NE(TargetId, A->Metadata.Id);
	ASSERT_TRUE(Target->SetParameterValueByInfo(Target->GetParameterDefinitions().front().GetParameterInfo(), FMaterialParameterValue::MakeScalar(.9f)));
	ASSERT_TRUE(TargetEditorGraph->Paste(Payload, 100, 0, false));
	EXPECT_EQ(Target->GetParameterDefinitions().size(), 1u);
	for (const auto& E : Target->GetExpressionCollection().Expressions)
		if (const auto* Parameter = Cast<DMaterialExpressionScalarParameter>(E.Get())) EXPECT_FLOAT_EQ(Parameter->DefaultValue, .9f);
	FMaterialParameterDefinition Conflict;
	Conflict.Name = "Amount"; Conflict.Type = EMaterialParameterType::Vector;
	Conflict.Value = FMaterialParameterValue::MakeVector({1, 1, 1});
	const auto TargetBefore = CaptureExpressions(*Target);
	const auto Rejected = MaterialEditing::CreateParameter(*Target, Conflict, false);
	ASSERT_FALSE(Rejected);
	EXPECT_EQ(Rejected.error().Message.find("A parameter with this name already exists with a different type."), 0u);
	EXPECT_EQ(CaptureExpressions(*Target), TargetBefore);
	Conflict.Name = "IndependentAmount";
	ASSERT_TRUE(MaterialEditing::CreateParameter(*Target, Conflict, false));
	EXPECT_EQ(Rejected.error().Message.find("A parameter with this name already exists with a different type."), 0u);

	const auto* Existing = Cast<DMaterialExpressionParameter>(Target->GetExpressionCollection().Expressions.front().Get());
	ASSERT_NE(Existing, nullptr);
	TStrongObjectPtr<DMaterialExpressionParameter> Invalid(DuplicateObject(Existing, nullptr, NAME_None).value());
	Invalid->Metadata.Name = NAME_None;
	const auto BeforeInvalid = CaptureExpressions(*Target);
	const auto InvalidResult = DMaterialGraph::Create(*Target)->ReplaceExpression(*Invalid.Get(), false);
	EXPECT_FALSE(InvalidResult);
	EXPECT_EQ(InvalidResult.Message.find("The parameter definition is invalid."), 0u);
	EXPECT_EQ(CaptureExpressions(*Target), BeforeInvalid);
	Invalid->Metadata.Name = "ValidRetry";
	ASSERT_TRUE(DMaterialGraph::Create(*Target)->ReplaceExpression(*Invalid.Get(), false));
	MarkAsGarbage(Source); MarkAsGarbage(Target); CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, NarrowPromotionUsesAndReusesFourComponentOwnerThroughMask)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, NAME_None);
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	const auto Color = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float3);
	ASSERT_TRUE(Color);
	const auto Id = Color->front();
	ASSERT_TRUE(AuthoringGraph->FindNode(Id)->SetConstantValue(FMaterialParameterValue::MakeVector({.2, .4, .6}), false));
	const auto Before = CaptureExpressions(*Material);
	const auto Promoted = MaterialEditing::PromoteConstantToParameter(*Material, Id, "Tint");
	ASSERT_TRUE(Promoted) << FormatMaterialGraphCommandResult(Promoted);
	const auto* Definition = Material->FindParameterDefinition("Tint");
	ASSERT_NE(Definition, nullptr);
	EXPECT_EQ(Definition->Type, EMaterialParameterType::Vector4);
	EXPECT_NEAR(Definition->Value.GetVector4().z, .6, 1.e-6);
	const auto ParameterId = Definition->Id;
	const auto* Mask = FindExpression<DMaterialExpressionSwizzle>(*Material, Id);
	ASSERT_NE(Mask, nullptr);
	EXPECT_EQ(Mask->Components, (std::vector<uint8>{0, 1, 2}));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	ASSERT_TRUE(Transactions->Redo());
	const auto UV = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float2);
	ASSERT_TRUE(UV);
	const auto Reused = MaterialEditing::PromoteConstantToParameter(*Material, UV->front(), "Tint", false);
	ASSERT_TRUE(Reused) << FormatMaterialGraphCommandResult(Reused);
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 1u);
	const auto* UVMask = FindExpression<DMaterialExpressionSwizzle>(*Material, UV->front());
	ASSERT_NE(UVMask, nullptr);
	const auto* MaskSource = Cast<DMaterialExpressionParameter>(UVMask->Input.Connection.Expression.Get());
	ASSERT_NE(MaskSource, nullptr);
	EXPECT_EQ(MaskSource->Metadata.Id, ParameterId);
	EXPECT_EQ(UVMask->Components, (std::vector<uint8>{0, 1}));
}

TEST(FMaterialGraphOperationsTests, ChangeSetCoalescesNodeLifetimeAndReset)
{
	using N = EMaterialGraphNodeChange;
	FMaterialGraphChangeSet Changes;
	const auto Id = FGuid::NewGuid();
	Changes.MarkNode(Id, N::Content);
	Changes.MarkNode(Id, N::Position);
	ASSERT_EQ(Changes.Nodes.size(), 1u);
	EXPECT_EQ(Changes.Nodes[0].Flags, N::Content | N::Position);
	Changes.MarkNode(Id, N::Removed);
	EXPECT_EQ(Changes.Nodes[0].Flags, N::Removed);
	Changes.MarkNode(Id, N::Added);
	EXPECT_NE(Changes.Nodes[0].Flags & N::Interface, N::None);
	FMaterialGraphChangeSet Transient;
	Transient.MarkNode(Id, N::Added);
	Transient.MarkNode(Id, N::Content);
	Transient.MarkNode(Id, N::Removed);
	EXPECT_TRUE(Transient.IsEmpty());
	Changes.MarkGraph(EMaterialGraphChange::Reset);
	Changes.MarkNode(Id, N::Content);
	EXPECT_TRUE(Changes.Nodes.empty());
	EXPECT_TRUE(Changes.Has(EMaterialGraphChange::Reset));
}

TEST(FMaterialGraphOperationsTests, OwnerPublishesCompleteCommandAndHistoryOnce)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "GraphChangeHistory"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material.Get()));
	std::vector<FMaterialGraphChangeSet> Events;
	const auto Handle = Material->GetGraphChanges().Subscribe(*Material.Get(), [&](const auto& Change) {
		Events.push_back(Change);
		for (const auto& Node : Change.Nodes)
			if ((Node.Flags & EMaterialGraphNodeChange::Added) != EMaterialGraphNodeChange::None)
				EXPECT_TRUE(FindMaterialExpression(Material->GetExpressionCollection().Expressions, Node.NodeId)->EditorLayout.bAuthored);
	});
	const auto Created = Testing::CreateGraphConstant(*AuthoringGraph, .25f, 123, 456, true);
	ASSERT_TRUE(Created);
	ASSERT_EQ(Events.size(), 1u);
	ASSERT_EQ(Events.back().Nodes.size(), 1u);
	EXPECT_EQ(Events.back().Nodes[0].Flags, EMaterialGraphNodeChange::Added);
	EXPECT_TRUE(Transactions->Undo());
	ASSERT_EQ(Events.size(), 2u);
	EXPECT_EQ(Events.back().Nodes[0].Flags, EMaterialGraphNodeChange::Removed);
	EXPECT_TRUE(Transactions->Redo());
	ASSERT_EQ(Events.size(), 3u);
	EXPECT_EQ(Events.back().Nodes[0].Flags, EMaterialGraphNodeChange::Added);
	Material->GetGraphChanges().Unsubscribe(Handle);
	Transactions->Reset();
}

TEST(FMaterialGraphOperationsTests, GraphObservesDirectAndReflectedEditsWithoutCompilePolling)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "GraphChangeDirect"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	auto Constant = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
	Constant->Value = .25f;
	const std::array<DMaterialExpression*, 1> Expressions{Constant.Get()};
	ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, {}));
	TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
	auto* Node = Graph->FindNode(Constant->Id);
	ASSERT_TRUE(Node);
	ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, {}));
	EXPECT_EQ(Graph->FindNode(Constant->Id), Node);
	Constant->Value = .75f;
	ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, {}));
	EXPECT_EQ(Graph->FindNode(Constant->Id), Node);
	EXPECT_FLOAT_EQ(Node->GetView().Node.GetConstantLiteral().X, .75f);
	auto* Owned = Cast<DMaterialExpressionScalarConstant>(Material->GetExpressionCollection().Expressions[0].Get());
	Owned->Value = .5f;
	Owned->PostEditChangeProperty({.MemberProperty = Owned->GetClass()->FindPropertyByName("Value")});
	Owned->Value = .875f;
	EXPECT_FLOAT_EQ(Node->GetView().Node.GetConstantLiteral().X, .5f);
	Owned->Value = .5f;
	Owned->PostEditChangeProperty({.MemberProperty = Owned->GetClass()->FindPropertyByName("Value")});
	EXPECT_FLOAT_EQ(Node->GetView().Node.GetConstantLiteral().X, .5f);
	FMaterialRootInputs InvalidOutputs;
	InvalidOutputs.Get(EMaterialProperty::MP_Roughness) = {NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None)};
	const auto Invalid = Material->SetMaterialExpressions(Expressions, InvalidOutputs);
	EXPECT_FALSE(Invalid);
	EXPECT_FLOAT_EQ(Node->GetView().Node.GetConstantLiteral().X, .5f);
	ASSERT_TRUE(Material->SetStaticProperties(Material->GetStaticProperties()));
	EXPECT_FLOAT_EQ(Node->GetView().Node.GetConstantLiteral().X, .5f);
}

TEST(FMaterialGraphOperationsTests, GraphSubscriptionsTrackReplacementDependenciesAndStayDocumentLocal)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "GraphObserverMaterial"));
	TStrongObjectPtr<DMaterialFunction> First(NewObject<DMaterialFunction>(nullptr, "GraphObserverFirst"));
	TStrongObjectPtr<DMaterialFunction> Second(NewObject<DMaterialFunction>(nullptr, "GraphObserverSecond"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto Call = DMaterialGraph::Create(*Material.Get())->InsertFunctionCall(*First.Get(), 0, 0, false).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Call);
	TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
	// Emulate the external-reference rewrite performed by committed package replacement.
	auto* CallNode = Cast<DMaterialExpressionFunctionCall>(Material->GetExpressionCollection().Expressions[0].Get());
	ASSERT_NE(CallNode, nullptr);
	CallNode->Function = Second.Get();
	RefreshMaterialGraphObservers();
	EXPECT_EQ(Graph->FindNode(Call.value())->GetView().PrimaryLabel, Second->GetName());
	TStrongObjectPtr<DMaterialGraph> FirstGraph(DMaterialGraph::Create(*First));
	const auto BeforeOldDependencyEdit = Graph->FindNode(Call.value())->GetOutputPin(0)->GetId();
	auto* FirstOutput = Testing::FindFunctionTerminal<DMaterialExpressionFunctionOutput>(*FirstGraph, First->GetFunctionSignature().Outputs.front().Id);
	ASSERT_TRUE(FirstOutput);
	ASSERT_TRUE(FirstOutput->OnRenameNode("Old dependency", false));
	EXPECT_EQ(Graph->FindNode(Call.value())->GetOutputPin(0)->GetId(), BeforeOldDependencyEdit);
	EXPECT_NE(Graph->FindNode(Call.value())->GetView().Outputs[0].Name, "Old dependency");
	TStrongObjectPtr<DMaterialGraph> SecondGraph(DMaterialGraph::Create(*Second));
	auto* SecondOutput = Testing::FindFunctionTerminal<DMaterialExpressionFunctionOutput>(*SecondGraph, Second->GetFunctionSignature().Outputs.front().Id);
	ASSERT_TRUE(SecondOutput);
	const auto OriginalName = Graph->FindNode(Call.value())->GetView().Outputs[0].Name;
	Durin::Tests::FTestEditorTransactorOwner History;
	ASSERT_TRUE(SecondOutput->OnRenameNode("New dependency", true));
	ASSERT_EQ(Graph->FindNode(Call.value())->GetView().Outputs[0].Name, "New dependency");
	ASSERT_TRUE(History->Undo());
	EXPECT_EQ(Graph->FindNode(Call.value())->GetView().Outputs[0].Name, OriginalName);
	ASSERT_TRUE(History->Redo());
	EXPECT_EQ(Graph->FindNode(Call.value())->GetView().Outputs[0].Name, "New dependency");
	const auto FirstOutputId = Testing::GraphNodeId(FirstOutput);
	ASSERT_TRUE(SecondOutput->OnRenameNode("Detached dependency", false));
	EXPECT_EQ(FirstGraph->FindNode(FirstOutputId)->GetView().PrimaryLabel, "Old dependency");
	EXPECT_EQ(Graph->FindNode(Call.value())->GetView().Outputs[0].Name, "Detached dependency");
}

TEST(FMaterialGraphOperationsTests, GraphPositionUpdatesAndReentrantPublicationAreCoherent)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "GraphReentrantChanges"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto Created = Testing::CreateGraphConstant(*DMaterialGraph::Create(*Material.Get()), .25f);
	ASSERT_TRUE(Created);
	TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
	auto* Node = Graph->FindNode(Created.value());
	ASSERT_TRUE(Node);
	auto* Expression = Cast<DMaterialExpressionScalarConstant>(Node->GetMaterialExpression());
	ASSERT_TRUE(Expression);
	// A position publication must not reread an unrelated, unpublished expression value.
	Expression->Value = .75f;
	int Depth = 0, MaximumDepth = 0, Events = 0;
	const auto Handle = Material->GetGraphChanges().Subscribe(*Material.Get(), [&](const auto&) {
		MaximumDepth = std::max(MaximumDepth, ++Depth);
		if (++Events == 1)
		{
			auto Position = Material->GetMaterialGraphPresentation();
			Position.Nodes[0].X = 222;
			EXPECT_TRUE(Material->SetMaterialGraphPresentation(Position) != Durin::EMaterialGraphPresentationResult::Rejected);
		}
		--Depth;
	});
	auto Position = Material->GetMaterialGraphPresentation();
	Position.Nodes[0].X = 111;
	const auto CompileRevision = Material->GetMaterialCompileDebugInfo().AuthoredRevision;
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(Position) != Durin::EMaterialGraphPresentationResult::Rejected);
	EXPECT_EQ(Events, 2);
	EXPECT_EQ(MaximumDepth, 1);
	EXPECT_EQ(Node->GetView().Presentation.X, 222);
	EXPECT_FLOAT_EQ(Node->GetView().Node.GetConstantLiteral().X, .25f);
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, CompileRevision);
	Expression->Value = .25f;
	Material->GetGraphChanges().Unsubscribe(Handle);
}

TEST(FMaterialGraphOperationsTests, ChangeObserversCanDetachDuringDispatchAndSuppressNetNoOps)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, "GraphObserverLifetime"));
	auto& Source = Function->GetGraphChanges();
	int FirstCalls = 0, DetachedCalls = 0;
	FDelegateHandle Detached;
	const auto First = Source.Subscribe(*Function.Get(), [&](const auto&) {
		++FirstCalls;
		Source.Unsubscribe(Detached);
	});
	Detached = Source.Subscribe(*Function.Get(), [&](const auto&) { ++DetachedCalls; });
	const auto Original = Function->GetFunctionPresentation();
	auto Moved = Original;
	Moved.Nodes[0].X += 40;
	{
		FScopedMaterialGraphChange Batch(*Function.Get());
		ASSERT_TRUE(Function->SetFunctionPresentation(Moved));
		ASSERT_TRUE(Function->SetFunctionPresentation(Original));
	}
	EXPECT_EQ(FirstCalls, 0);
	ASSERT_TRUE(Function->SetFunctionPresentation(Moved));
	EXPECT_EQ(FirstCalls, 1);
	EXPECT_EQ(DetachedCalls, 0);
	Source.Unsubscribe(First);
	ASSERT_TRUE(Function->SetFunctionPresentation(Original));
	EXPECT_EQ(FirstCalls, 1);
}

TEST(FMaterialGraphOperationsTests, VertexOffsetPinRetainsConnectionsAndDefaultsAcrossAttributesMode)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "VertexOffsetPin"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	Tests::FTestEditorTransactorOwner Transactions;
	const auto OutputId = AuthoringGraph->GetRootNode()->GetView().Node.Id;
	const auto BeforeInvalidProperty = Material->CaptureRootInputs();
	EXPECT_FALSE(GraphEditInternals::WriteInputDefault(Material.Get(), static_cast<EMaterialProperty>(254), {.Kind = EMaterialInputDefaultKind::Literal,
		.Type = EMaterialProgramValueType::Float3, .Literal = {1, 1, 1}}, false));
	EXPECT_EQ(Material->CaptureRootInputs(), BeforeInvalidProperty);
	const auto PinId = EMaterialProperty::MP_WorldPositionOffset;
	ASSERT_TRUE(GraphEditInternals::WriteInputDefault(Material.Get(), PinId, {.Kind = EMaterialInputDefaultKind::Literal, .Type = EMaterialProgramValueType::Float3,
			.Literal = {1.f, 2.f, 3.f}}));
	EXPECT_EQ(ReadMaterialNumericDefault(*Material->GetNumericInputForProperty(EMaterialProperty::MP_WorldPositionOffset), EMaterialProperty::MP_WorldPositionOffset),
		(std::vector<float>{1.f, 2.f, 3.f}));
	const auto Normal = Testing::CreateGraphCatalogNode(*AuthoringGraph,
		EMaterialProgramOpcode::VertexNormal, EMaterialProgramValueType::Float3);
	ASSERT_TRUE(Normal);
	auto View = Testing::CaptureGraphView(*AuthoringGraph);
	const auto* Terminal = FindViewNode(View, OutputId);
	ASSERT_NE(Terminal, nullptr);
	const auto Pin = std::ranges::find(Terminal->Inputs, PinId, &FMaterialGraphPinView::MaterialProperty);
	ASSERT_NE(Pin, Terminal->Inputs.end());
	EXPECT_EQ(Pin->Constraint.Stages, EMaterialEvaluationStage::Vertex);
	ASSERT_TRUE(Testing::WriteConnection(Material.Get(), PinId, FMaterialProgramLink{Normal->front()}, false));
	ASSERT_TRUE(MaterialEditing::SetUseMaterialAttributes(*Material, true, false));
	View = Testing::CaptureGraphView(*AuthoringGraph);
	Terminal = FindViewNode(View, OutputId);
	ASSERT_NE(Terminal, nullptr);
	const auto InactivePin = std::ranges::find(Terminal->Inputs, PinId, &FMaterialGraphPinView::MaterialProperty);
	ASSERT_NE(InactivePin, Terminal->Inputs.end());
	EXPECT_FALSE(InactivePin->bActive);
	EXPECT_EQ(InactivePin->Link.SourceNodeId, Normal->front());
	EXPECT_EQ(Material->GetNumericInputForProperty(EMaterialProperty::MP_WorldPositionOffset)->Connection.GetExpressionId(), Normal->front());
	std::vector<DMaterialExpression*> Expressions;
	for (const auto& Expression : Material->GetExpressionCollection().Expressions) Expressions.push_back(Expression.Get());
	const auto Built = MIR::FGraphBuilder(Expressions).FinishSurface(Material->CaptureRootInputs());
	ASSERT_TRUE(Built);
	EXPECT_FALSE(Built.IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset).bExpression);
	EXPECT_EQ(Built.IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset).Literal, FMaterialProgramLiteral{});
	ASSERT_TRUE(Testing::WriteConnection(Material.Get(), EMaterialProperty::MP_WorldPositionOffset, FMaterialProgramLink{}, true));
	EXPECT_EQ(ReadMaterialNumericDefault(*Material->GetNumericInputForProperty(EMaterialProperty::MP_WorldPositionOffset), EMaterialProperty::MP_WorldPositionOffset),
		(std::vector<float>{1.f, 2.f, 3.f}));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->GetNumericInputForProperty(EMaterialProperty::MP_WorldPositionOffset)->Connection.GetExpressionId(), Normal->front());
}

TEST(FMaterialGraphOperationsTests, MaterialOutputUsesStablePinsAndOrdinaryNodeChanges)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "OutputNodeContract"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material.Get()));
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	const auto OutputId = AuthoringGraph->GetRootNode()->GetView().Node.Id;
	const auto Created = Testing::CreateGraphConstant(*AuthoringGraph, .4f, 0, 0);
	ASSERT_TRUE(Created);
	const auto View = Testing::CaptureGraphView(*AuthoringGraph);
	const auto* Terminal = FindViewNode(View, OutputId);
	ASSERT_NE(Terminal, nullptr);
	EXPECT_TRUE(Terminal->Node.bMaterialOutput);
	EXPECT_TRUE(Terminal->Outputs.empty());
	ASSERT_EQ(Terminal->Inputs.size(), FMaterialAttributeDefinitionMap::GetDefinitions().size() + 1);
	EXPECT_EQ(Terminal->Inputs.front().MaterialProperty, EMaterialProperty::MP_BaseColor);
	EXPECT_GT(GraphNodeHeight(*Terminal), GraphNodePinOffset(*Terminal) + 7 * FMaterialGraphGeometry::GetMetrics().PinRowHeight);
	EXPECT_TRUE(GetMaterialDomainInputProperties(static_cast<EMaterialDomain>(255)).empty());
	std::vector<FMaterialGraphChangeSet> Events;
	const auto Handle = Material->GetGraphChanges().Subscribe(*Material.Get(), [&](const auto& Change) { Events.push_back(Change); });
	ASSERT_TRUE(Testing::WriteConnection(Material.Get(), EMaterialProperty::MP_Roughness, FMaterialProgramLink{Created.value()}, false));
	EXPECT_EQ(Material->GetExpressionInputForProperty(EMaterialProperty::MP_Roughness)->GetExpressionId(), Created.value());
	EXPECT_FALSE(Material->GetExpressionInputForProperty(EMaterialProperty::MP_BaseColor)->IsConnected());
	ASSERT_EQ(Events.size(), 1u);
	EXPECT_TRUE(Events.back().Has(EMaterialGraphChange::RootInputs));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FALSE(Material->GetExpressionInputForProperty(EMaterialProperty::MP_Roughness)->IsConnected());
	ASSERT_TRUE(Transactions->Redo());
	const auto Revision = Material->GetMaterialCompileDebugInfo().AuthoredRevision;
	const FMaterialGraphNodePresentation Position{OutputId, 600, 120};
	ASSERT_TRUE(AuthoringGraph->MoveNodes(std::span(&Position, 1)));
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision);
	EXPECT_TRUE(Events.back().Has(EMaterialGraphChange::RootPosition));
	const FMaterialInputDefault Default{.Kind = EMaterialInputDefaultKind::Literal,
		.Type = EMaterialProgramValueType::Float, .Literal = {.X = .7f}};
	ASSERT_TRUE(GraphEditInternals::WriteInputDefault(Material.Get(), EMaterialProperty::MP_Metallic, Default));
	EXPECT_FLOAT_EQ(ReadMaterialNumericDefault(*Material->GetNumericInputForProperty(EMaterialProperty::MP_Metallic), EMaterialProperty::MP_Metallic)[0], .7f);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FLOAT_EQ(ReadMaterialNumericDefault(*Material->GetNumericInputForProperty(EMaterialProperty::MP_Metallic), EMaterialProperty::MP_Metallic)[0], 0.f);
	ASSERT_TRUE(Transactions->Redo());
	const auto Extracted = AuthoringGraph->FindNode(OutputId)->ExtractInputDefault(Testing::ResolvePin(*AuthoringGraph->FindNode(OutputId), EMaterialProperty::MP_Metallic));
	ASSERT_TRUE(Extracted) << FormatMaterialGraphCommandResult(Extracted);
	ASSERT_TRUE(AuthoringGraph->FindNode(OutputId)->InlineInputNode(Testing::ResolvePin(*AuthoringGraph->FindNode(OutputId), EMaterialProperty::MP_Metallic)));
	EXPECT_FLOAT_EQ(ReadMaterialNumericDefault(*Material->GetNumericInputForProperty(EMaterialProperty::MP_Metallic), EMaterialProperty::MP_Metallic)[0], .7f);
	EXPECT_FALSE(GraphEditInternals::WriteInputDefault(Material.Get(), static_cast<EMaterialProperty>(255), Default, false));
	const auto BeforeRejected = Events.size();
	EXPECT_FALSE(AuthoringGraph->RemoveNodes(std::span(&OutputId, 1)));
	FMaterialGraphClipboardPayload Clipboard;
	EXPECT_FALSE(AuthoringGraph->CopySelection(std::span(&OutputId, 1), Clipboard));
	{
		const auto Missing = FGuid::NewGuid();
		EXPECT_EQ(AuthoringGraph->RemoveNodes(std::span(&Missing, 1)).Status, EMaterialGraphCommandStatus::NoChange);
	}
	EXPECT_EQ(Events.size(), BeforeRejected);
	EXPECT_EQ(AuthoringGraph->GetRootNode()->GetView().Node.Id, OutputId);
	Material->GetGraphChanges().Unsubscribe(Handle);
	Transactions->Reset();
}

TEST(FMaterialGraphOperationsTests, InvalidTypesRemainEditableAndUndoRestoresDiagnostics)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	const auto Vector = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float2);
	const auto Scalar = Testing::CreateGraphConstant(*AuthoringGraph, .5f);
	ASSERT_TRUE(Vector); ASSERT_TRUE(Scalar);
	const auto Original = Material->GetExpressionCollection().Expressions;
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	ASSERT_TRUE(Testing::WriteConnection(Material.Get(), EMaterialProperty::MP_BaseColor, FMaterialProgramLink{Vector->front()}, true));
	EXPECT_TRUE(HasGraphDiagnostics(*Material));
	ASSERT_TRUE(AuthoringGraph->FindNode(Scalar.value())->SetConstantValue(FMaterialParameterValue::MakeScalar(.75f)));
	EXPECT_TRUE(HasGraphDiagnostics(*Material));
	EXPECT_EQ(Material->GetExpressionCollection().Expressions, Original);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_TRUE(HasGraphDiagnostics(*Material));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FALSE(HasGraphDiagnostics(*Material));
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_TRUE(HasGraphDiagnostics(*Material));
	EXPECT_EQ(Material->GetExpressionCollection().Expressions, Original);
}

TEST(FMaterialGraphOperationsTests, GraphChangesJoinAnOuterRecording)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	const auto Source = Testing::CreateGraphConstant(*AuthoringGraph, .5f);
	const auto Destination = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::Add, EMaterialProgramValueType::Float);
	ASSERT_TRUE(Source); ASSERT_TRUE(Destination);
	const auto Before = CaptureExpressions(*Material);
	const auto Original = Material->GetExpressionCollection().Expressions;
	const auto Revision = Material->GetMaterialProgramRevision();
	uint32 Notifications = 0;
	const auto Observer = Material->GetGraphChanges().Subscribe(*Material, [&](const auto&) { ++Notifications; });
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	const auto Pending = Transactions->Begin("test", "Other operation");
	ASSERT_GE(Pending, 0);
	EXPECT_TRUE(Testing::WriteConnection(Material.Get(), Testing::ExpressionInput(Destination->front(), 0), FMaterialProgramLink{Source.value()}, false));
	EXPECT_NE(CaptureExpressions(*Material), Before);
	EXPECT_EQ(Material->GetExpressionCollection().Expressions, Original);
	EXPECT_GT(Material->GetMaterialProgramRevision(), Revision);
	EXPECT_EQ(Notifications, 1u);
	ASSERT_NE(GUndo, nullptr);
	const auto Id = GUndo->GetContext().TransactionId;
	EXPECT_EQ(Transactions->GetQueueLength(), 0u);
	EXPECT_FALSE(Transactions->CanUndo());
	EXPECT_EQ(Transactions->End(), 1);
	ASSERT_EQ(Transactions->GetQueueLength(), 1u);
	EXPECT_EQ(Transactions->GetUndoContext().TransactionId, Id);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	Material->GetGraphChanges().Unsubscribe(Observer);
}

TEST(FMaterialGraphOperationsTests, CompoundCreationAndDeletionReplayAcrossGC)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
	Durin::Tests::FTestEditorTransactorOwner History;
	ASSERT_GE(History->Begin("MaterialGraph", "Create and edit"), 0);
	const auto Created = Testing::CreateGraphConstant(*Graph, .25f, 12, 34, true);
	ASSERT_TRUE(Created);
	const FGuid Id = Created.value();
	ASSERT_TRUE(Graph->FindNode(Id)->SetConstantValue(FMaterialParameterValue::MakeScalar(.75f)));
	ASSERT_EQ(History->End(), 1);
	ASSERT_EQ(History->GetQueueLength(), 1u);
	auto Find = [&]() { return Cast<DMaterialExpressionScalarConstant>(
		FindMaterialExpression(Material->GetExpressionCollection().Expressions, Id)); };
	ASSERT_TRUE(History->Undo());
	EXPECT_EQ(Find(), nullptr);
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(History->Redo());
	ASSERT_NE(Find(), nullptr);
	EXPECT_EQ(Find()->GetOuter(), Material.Get());
	EXPECT_FLOAT_EQ(Find()->Value, .75f);

	ASSERT_GE(History->Begin("MaterialGraph", "Edit and delete"), 0);
	ASSERT_TRUE(Graph->FindNode(Id)->SetConstantValue(FMaterialParameterValue::MakeScalar(.5f)));
	ASSERT_TRUE(Graph->RemoveNodes(std::array{Id}));
	ASSERT_EQ(History->End(), 1);
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(History->Undo());
	ASSERT_NE(Find(), nullptr);
	EXPECT_FLOAT_EQ(Find()->Value, .75f);
	ASSERT_TRUE(History->Redo());
	EXPECT_EQ(Find(), nullptr);
}

TEST(FMaterialGraphOperationsTests, MissingAllocationRejectsTheWholeGraphReplay)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
	const auto Created = Testing::CreateGraphConstant(*Graph, .25f);
	ASSERT_TRUE(Created);
	auto* Child = FindMaterialExpression(Material->GetExpressionCollection().Expressions, Created.value());
	ASSERT_NE(Child, nullptr);
	const FObjectKey Key(Child);
	// Standalone records deliberately have no GC owner. A normal undo buffer
	// keeps this allocation alive and is exercised by the compound edit case.
	FTransaction History(1, {.Description = "Delete graph child"});
	ASSERT_TRUE(History.BeginOperation());
	History.SaveObject(Material.Get());
	History.SaveObject(Child);
	ASSERT_EQ(History.GetRecordCount(), 2u);
	ASSERT_TRUE(Graph->RemoveNodes(std::array{Created.value()}, false));
	History.Finalize();
	ASSERT_TRUE(History.EndOperation());
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_EQ(GDObjectArray.Resolve(Key), nullptr);
	const auto Before = CaptureExpressions(*Material);
	const auto Revision = Material->GetMaterialCompileDebugInfo().AuthoredRevision;
	const auto Result = History.Validate();
	ASSERT_FALSE(Result);
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision);
}

TEST(FMaterialGraphOperationsTests, StructuralReplayRejectsDifferentMemberWithTheSameGuid)
{
	InitializeDObjectSystem();
	for (bool bUndo : {true, false})
	{
		TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
		Durin::Tests::FTestEditorTransactorOwner History;
		auto Draft = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
		ASSERT_TRUE(Graph->CreateExpression(*Draft, 0, 0, bUndo ? false : true));
		if (bUndo) ASSERT_TRUE(Graph->RemoveNodes(std::array{Draft->Id}));
		else ASSERT_TRUE(History->Undo());
		ASSERT_TRUE(Graph->CreateExpression(*Draft, 10, 15, false));
		const auto Current = Material->GetExpressionCollection().Expressions;
		int Events = 0;
		const auto Handle = Material->GetGraphChanges().Subscribe(*Material, [&](const auto&) { ++Events; });
		const auto Revision = Material->GetMaterialCompileDebugInfo().AuthoredRevision;
		EXPECT_FALSE(bUndo ? History->Undo() : History->Redo());
		EXPECT_TRUE(bUndo ? History->CanUndo() : History->CanRedo());
		EXPECT_EQ(Material->GetExpressionCollection().Expressions, Current);
		EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision);
		EXPECT_EQ(Events, 0);
		Material->GetGraphChanges().Unsubscribe(Handle);
	}
}

TEST(FMaterialGraphOperationsTests, MissingExpressionBlocksUndoWithoutChangingGraph)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	const auto Created = Testing::CreateGraphConstant(*AuthoringGraph, .25f);
	ASSERT_TRUE(Created);
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	ASSERT_TRUE(AuthoringGraph->FindNode(Created.value())->SetConstantValue(FMaterialParameterValue::MakeScalar(.5f)));
	std::vector<DMaterialExpression*> Nodes;
	for (auto& E : Material->GetExpressionCollection().Expressions)
		if (E->Id != Created.value()) Nodes.push_back(E.Get());
	ASSERT_TRUE(Material->SetMaterialExpressions(Nodes));
	const auto Before = CaptureExpressions(*Material);
	const auto Revision = Material->GetMaterialProgramRevision();
	const auto Id = Transactions->GetUndoContext().TransactionId;
	EXPECT_FALSE(Transactions->UndoExpected(Id));
	EXPECT_EQ(Transactions->GetUndoContext().TransactionId, Id);
	EXPECT_FALSE(Transactions->CanRedo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_EQ(Material->GetMaterialProgramRevision(), Revision);
	EXPECT_TRUE(Transactions->CanUndo());
}

TEST(FMaterialGraphOperationsTests, AttributeInputsSharePinAdmissionAndUndoStructuralChanges)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	auto Set = Testing::MakeGraphExpression<DMaterialExpressionSetMaterialAttributes>();
	ASSERT_TRUE(AuthoringGraph->CreateExpression(*Set, 0, 0, false));
	auto Get = Testing::MakeGraphExpression<DMaterialExpressionGetMaterialAttributes>();
	const auto Property = EMaterialProperty::MP_EmissiveColor;
	const auto PortId = FMaterialAttributeDefinitionMap::GetID(Property);
	const auto Slot = static_cast<uint8>(Property);
	const auto* Definition = FMaterialAttributeDefinitionMap::Find(Property);
	ASSERT_NE(Definition, nullptr);
	Get->Surface = {FindMaterialExpression(Material->GetExpressionCollection().Expressions, Set->Id)};
	Get->AttributeIDs = {PortId};
	ASSERT_TRUE(AuthoringGraph->CreateExpression(*Get, 0, 0, false));
	const auto* GetNode = AuthoringGraph->FindNode(Get->Id);
	ASSERT_NE(GetNode, nullptr);
	const auto* GetExpression = GetNode->GetMaterialExpression();
	ASSERT_NE(GetExpression, nullptr);
	const auto Selected = GetExpression->GetOutput(0);
	ASSERT_TRUE(Selected);
	EXPECT_EQ(Selected->Name, Definition->Name);
	EXPECT_EQ(Selected->Type, Definition->Semantics.Type);
	EXPECT_EQ(Selected->PortId, PortId);
	EXPECT_EQ(Selected->StoredIndex, Slot);
	EXPECT_FALSE(GetExpression->GetOutput(1));
	const auto* RootInput = Material->GetExpressionInputForProperty(Property);
	ASSERT_TRUE(Testing::WriteConnection(Material.Get(), Property, {Get->Id, 0, PortId}, false, false));
	EXPECT_EQ(RootInput->OutputIndex, Slot);
	EXPECT_EQ(RootInput->GetOutputId(), PortId);
	ASSERT_TRUE(Testing::WriteConnection(Material.Get(), Property, {}, true, false));
	ASSERT_TRUE(Testing::WriteConnection(Material.Get(), Property, {Get->Id, Slot}, false, false));
	EXPECT_EQ(RootInput->OutputIndex, Slot);
	EXPECT_EQ(RootInput->GetOutputId(), PortId);
	const auto Scalar = Testing::CreateGraphConstant(*AuthoringGraph, .25f);
	ASSERT_TRUE(Scalar);
	const auto OtherScalar = Testing::CreateGraphConstant(*AuthoringGraph, .75f);
	ASSERT_TRUE(OtherScalar);
	const auto OtherSource = FMaterialProgramLink{OtherScalar.value()};
	auto Vector2 = Testing::MakeGraphExpression<DMaterialExpressionVector2Constant>();
	ASSERT_TRUE(AuthoringGraph->CreateExpression(*Vector2, 0, 0, false));
	const auto WrongWidth = FMaterialProgramLink{Vector2->Id};
	const auto& Schema = AuthoringGraph->GetSchema();
	auto* Node = AuthoringGraph->FindNode(Set->Id);
	ASSERT_TRUE(Node);
	const auto Pin = Testing::ExpressionInput(Set->Id, static_cast<uint32>(Property) + 1);
	const auto Source = FMaterialProgramLink{Scalar.value()};
	const auto Cycle = FMaterialProgramLink{Get->Id, 0, PortId};
	Tests::FTestEditorTransactorOwner History;
	ASSERT_FALSE(Testing::InputView(*AuthoringGraph, Pin));
	for (auto ExactVector : {EMaterialProperty::MP_Normal, EMaterialProperty::MP_WorldPositionOffset})
		EXPECT_FALSE(Node->CanSetAttributeInput(ExactVector, Testing::ResolvePin(*AuthoringGraph, Source)).Allows(true));
	ASSERT_TRUE(Testing::ResolvePin(*AuthoringGraph, Cycle));
	ASSERT_TRUE(Testing::ResolvePin(*AuthoringGraph, WrongWidth));
	EXPECT_FALSE(Node->SetAttributeInput(Property, *Testing::ResolvePin(*AuthoringGraph, WrongWidth)));
	EXPECT_FALSE(Node->CanSetAttributeInput(Property, Testing::ResolvePin(*AuthoringGraph, Cycle)).Allows(true));
	EXPECT_FALSE(Node->SetAttributeInput(Property, *Testing::ResolvePin(*AuthoringGraph, Cycle)));
	EXPECT_FALSE(Testing::ResolvePin(*AuthoringGraph, Pin));
	EXPECT_EQ(History->GetQueueLength(), 0u);
	ASSERT_TRUE(Testing::ResolvePin(*AuthoringGraph, Source));
	ASSERT_TRUE(Node->CanSetAttributeInput(Property, Testing::ResolvePin(*AuthoringGraph, Source)).Allows(true));
	ASSERT_TRUE(Node->SetAttributeInput(Property, *Testing::ResolvePin(*AuthoringGraph, Source)));
	const auto* SetExpression = Node->GetMaterialExpression();
	ASSERT_NE(SetExpression, nullptr);
	EXPECT_EQ(SetExpression->GetInputName(Slot + 1), Definition->Name);
	EXPECT_EQ(SetExpression->GetInputValueType(Slot + 1), Definition->Semantics.Type);
	EXPECT_NE(SetExpression->GetInput(Slot + 1), nullptr);
	EXPECT_EQ(SetExpression->GetInput(1), nullptr);
	EXPECT_EQ(History->GetQueueLength(), 1u);
	EXPECT_EQ(Node->CanSetAttributeInput(Property, Testing::ResolvePin(*AuthoringGraph, Source)).Response,
		Schema.CanCreateConnection(Testing::ResolvePin(*AuthoringGraph, Source), Testing::ResolvePin(*AuthoringGraph, Pin)).Response);
	EXPECT_FALSE(Node->CanSetAttributeInput(Property, Testing::ResolvePin(*AuthoringGraph, WrongWidth)).Allows(true));
	EXPECT_FALSE(Schema.CanCreateConnection(Testing::ResolvePin(*AuthoringGraph, WrongWidth), Testing::ResolvePin(*AuthoringGraph, Pin)).Allows(true));
	EXPECT_FALSE(Node->SetAttributeInput(Property, *Testing::ResolvePin(*AuthoringGraph, Source)));
	EXPECT_EQ(History->GetQueueLength(), 1u);
	ASSERT_TRUE(Testing::InputView(*AuthoringGraph, Pin));
	EXPECT_EQ(Testing::InputView(*AuthoringGraph, Pin)->Link.SourceNodeId, Source.SourceNodeId);
	ASSERT_TRUE(History->Undo());
	EXPECT_FALSE(Testing::InputView(*AuthoringGraph, Pin));
	ASSERT_TRUE(History->Redo());
	const auto* OtherOutput = Testing::ResolvePin(*AuthoringGraph, OtherSource);
	ASSERT_TRUE(OtherOutput);
	EXPECT_EQ(Node->CanSetAttributeInput(Property, OtherOutput).Response, EMaterialGraphConnectionResponse::ReplaceInput);
	EXPECT_EQ(Schema.CanCreateConnection(OtherOutput, Testing::ResolvePin(*AuthoringGraph, Pin)).Response, EMaterialGraphConnectionResponse::ReplaceInput);
	ASSERT_TRUE(Node->SetAttributeInput(Property, *OtherOutput));
	EXPECT_EQ(Testing::InputView(*AuthoringGraph, Pin)->Link.SourceNodeId, OtherSource.SourceNodeId);
	ASSERT_TRUE(History->Undo());
	EXPECT_EQ(Testing::InputView(*AuthoringGraph, Pin)->Link.SourceNodeId, Source.SourceNodeId);
	EXPECT_FALSE(AuthoringGraph->FindNode(Set->Id)->PinDefaultValueChanged(Testing::ResolvePin(*AuthoringGraph->FindNode(Set->Id), Pin), {.Kind = EMaterialInputDefaultKind::Literal,
		.Type = EMaterialProgramValueType::Float3, .Literal = {.X = .5f}}));
	const auto Count = History->GetQueueLength() - History->GetUndoCount();
	ASSERT_TRUE(Node->ClearAttributeInput(Property));
	EXPECT_EQ(History->GetQueueLength(), Count + 1);
	EXPECT_FALSE(Testing::ResolvePin(*AuthoringGraph, Pin));
	EXPECT_TRUE(Cast<DMaterialExpressionSetMaterialAttributes>(Node->GetMaterialExpression())->Attributes.empty());
	EXPECT_EQ(Node->ClearAttributeInput(Property).Status, EMaterialGraphCommandStatus::NoChange);
	EXPECT_EQ(History->GetQueueLength(), Count + 1);
	ASSERT_TRUE(History->Undo());
	ASSERT_TRUE(Testing::InputView(*AuthoringGraph, Pin));
	EXPECT_FALSE(Testing::InputView(*AuthoringGraph, Pin)->bSupportsConstant);
	EXPECT_EQ(Testing::InputView(*AuthoringGraph, Pin)->Link.SourceNodeId, Source.SourceNodeId);
}

TEST(FMaterialGraphOperationsTests, PinDefaultUndoRestoresAppendAndDownstreamWidth)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	const auto Append = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::AppendVector, EMaterialProgramValueType::Float2);
	ASSERT_TRUE(Append);
	const auto AppendId = Append->front();
	const auto Add = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::Add, EMaterialProgramValueType::Float2, {AppendId});
	ASSERT_TRUE(Add);
	Tests::FTestEditorTransactorOwner History;
	const auto Widths = [&](EMaterialProgramValueType Type) {
		EXPECT_EQ(AuthoringGraph->FindNode(AppendId)->GetView().Node.ResultType, Type);
		EXPECT_EQ(AuthoringGraph->FindNode(Add->front())->GetView().Node.ResultType, Type);
	};
	Widths(EMaterialProgramValueType::Float2);
	ASSERT_TRUE(AuthoringGraph->FindNode(AppendId)->PinDefaultValueChanged(Testing::ResolvePin(*AuthoringGraph->FindNode(AppendId), Testing::ExpressionInput(AppendId, 0)), {.Kind = EMaterialInputDefaultKind::Literal, .Type = EMaterialProgramValueType::Float2, .Literal = {1.f, 2.f}}));
	Widths(EMaterialProgramValueType::Float3);
	ASSERT_EQ(History->GetQueueLength(), 1u);
	ASSERT_TRUE(History->Undo());
	Widths(EMaterialProgramValueType::Float2);
	ASSERT_TRUE(History->Redo());
	Widths(EMaterialProgramValueType::Float3);
}

TEST(FMaterialGraphOperationsTests, TypeInferenceSkipsValuesAndUnrelatedBranches)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	auto Scalar = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
	auto Vector = Testing::MakeGraphExpression<DMaterialExpressionVector3Constant>();
	auto OtherVector = Testing::MakeGraphExpression<DMaterialExpressionVector3Constant>();
	auto Add = Testing::MakeGraphExpression<DMaterialExpressionAdd>();
	auto Multiply = Testing::MakeGraphExpression<DMaterialExpressionMultiply>();
	Add->A = {Scalar.Get()}; Multiply->A = {Add.Get()};
	{
		auto Setup = Material->GetExpressionCollection();
		for (auto* E : std::array<DMaterialExpression*, 5>{Scalar.Get(), Vector.Get(), OtherVector.Get(), Add.Get(), Multiply.Get()})
			Setup.Expressions.emplace_back(E);
		for (uint32 I = 0; I < 64; ++I)
			Setup.Expressions.emplace_back(Testing::MakeGraphExpression<DMaterialExpressionAdd>().Get());
		ASSERT_TRUE(GraphEditInternals::CommitGraphEdit(*Material, nullptr, "Build branches", Setup));
	}
	Tests::FTestEditorTransactorOwner Transactions;
	TStrongObjectPtr<DMaterialGraph> EditorGraph(DMaterialGraph::Create(*Material));
	ASSERT_TRUE(EditorGraph->FindNode(Scalar->Id)->SetConstantValue(FMaterialParameterValue::MakeScalar(.25f)));
	EXPECT_EQ(Transactions->GetTransaction(0)->GetRecordCount(), 1u);
	{
		const FMaterialExpressionInput Next{Vector.Get()};
		uint32 Inferred = 0;
		ASSERT_TRUE(GraphEditInternals::EditExpressionInputs(*Material, Transactions.Get(), "Connect vector", {Add.Get()},
			[&] { Add->A.Connection = Next; }, {{&Add->A.Connection, Next}}, {}, {}, &Inferred));
		EXPECT_EQ(Inferred, 2u);
		EXPECT_EQ(Add->ResultType, EMaterialProgramValueType::Float3);
		EXPECT_EQ(Multiply->ResultType, EMaterialProgramValueType::Float3);
	}
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Add->ResultType, EMaterialProgramValueType::Float);
	EXPECT_EQ(Multiply->ResultType, EMaterialProgramValueType::Float);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Multiply->ResultType, EMaterialProgramValueType::Float3);
	{
		const FMaterialExpressionInput Next{OtherVector.Get()};
		uint32 Inferred = 0;
		ASSERT_TRUE(GraphEditInternals::EditExpressionInputs(*Material, Transactions.Get(), "Reconnect same width", {Add.Get()},
			[&] { Add->A.Connection = Next; }, {{&Add->A.Connection, Next}}, {}, {}, &Inferred));
		EXPECT_EQ(Inferred, 1u);
		EXPECT_EQ(Multiply->ResultType, EMaterialProgramValueType::Float3);
	}

	ASSERT_TRUE(EditorGraph->FindNode(OtherVector->Id)->SetConstantValue(FMaterialParameterValue::MakeVector2({.2f, .3f})));
	EXPECT_EQ(Add->ResultType, EMaterialProgramValueType::Float2);
	EXPECT_EQ(Multiply->ResultType, EMaterialProgramValueType::Float2);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Multiply->ResultType, EMaterialProgramValueType::Float3);
	{
		// A same-class draft may explicitly author the result width before inference.
		auto Draft = Testing::MakeGraphExpression<DMaterialExpressionAdd>();
		Draft->Id = Add->Id;
		Draft->ResultType = EMaterialProgramValueType::Float4;
		ASSERT_TRUE(EditorGraph->ReplaceExpression(*Draft));
		EXPECT_EQ(Multiply->ResultType, EMaterialProgramValueType::Float4);
	}
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Multiply->ResultType, EMaterialProgramValueType::Float3);
	ASSERT_TRUE(Testing::WriteConnection(Material.Get(), Testing::ExpressionInput(Add->Id, 0), FMaterialProgramLink{Scalar->Id}, true));
	EXPECT_EQ(Add->ResultType, EMaterialProgramValueType::Float);
	EXPECT_EQ(Multiply->ResultType, EMaterialProgramValueType::Float);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Add->ResultType, EMaterialProgramValueType::Float3);
	EXPECT_EQ(Multiply->ResultType, EMaterialProgramValueType::Float3);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Add->ResultType, EMaterialProgramValueType::Float);
	EXPECT_EQ(Multiply->ResultType, EMaterialProgramValueType::Float);
}

TEST(FMaterialGraphOperationsTests, ExtractOutputDefaultPreservesMaterialAttributesConnection)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "ExtractOutputDefault"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	auto Graph = Testing::MakePBRMaterialExpressionsForTest();
	auto Packed = Testing::MakeGraphExpression<DMaterialExpressionMakeMaterialAttributes>();
	for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions())
		*Packed->GetExpressionInputForProperty(Field.Property) = Graph.Outputs.Get(Field.Property).Connection;
	Graph.Expressions.emplace_back(Packed.Get());
	Graph.Outputs.Attributes = {Packed.Get()};
	Graph.Outputs.Get(EMaterialProperty::MP_Metallic).Connection = {};
	Graph.Outputs.Get(EMaterialProperty::MP_Metallic).SetConstant({.7f});
	ASSERT_TRUE(Graph.Apply(*Material));
	TStrongObjectPtr<DMaterialGraph> EditorGraph(DMaterialGraph::Create(*Material));
	const FMaterialGraphNodePresentation Position{EditorGraph->GetRootNode()->GetView().Node.Id, 600, 120};
	ASSERT_TRUE(EditorGraph->MoveNodes(std::span(&Position, 1), false));

	Durin::Tests::FTestEditorTransactorOwner Transactions;
	const auto Before = Material->CaptureRootInputs();
	const auto Extracted = EditorGraph->FindNode(EditorGraph->GetRootNode()->GetView().Node.Id)->ExtractInputDefault(Testing::ResolvePin(*EditorGraph->FindNode(EditorGraph->GetRootNode()->GetView().Node.Id), EMaterialProperty::MP_Metallic)).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Extracted) << FormatMaterialGraphCommandResult(Extracted);

	const auto After = Material->CaptureRootInputs();
	auto Expected = Before;
	Expected.Get(EMaterialProperty::MP_Metallic) = {FindMaterialExpression(Material->GetExpressionCollection().Expressions, Extracted.value())};
	EXPECT_EQ(After, Expected);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->CaptureRootInputs(), Before);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Material->CaptureRootInputs(), Expected);
	ASSERT_TRUE(MaterialEditing::SetUseMaterialAttributes(*Material, true, false));
	EXPECT_EQ(*Material->GetExpressionInputForProperty(EMaterialProperty::MP_MaterialAttributes), Before.Attributes);
	std::vector<DMaterialExpression*> Expressions;
	for (const auto& Expression : Material->GetExpressionCollection().Expressions) Expressions.push_back(Expression.Get());
	const auto Built = MIR::FGraphBuilder(Expressions).FinishSurface(Material->CaptureRootInputs());
	ASSERT_TRUE(Built);
	EXPECT_FALSE(Built.IR.bAggregateAttributes);
	EXPECT_TRUE(Built.IR.GetAttribute(EMaterialProperty::MP_BaseColor).bExpression);
}

TEST(FMaterialGraphOperationsTests, OutputModeKeepsRootPinsAndLinksAcrossUndoRedo)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "OutputModes");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	auto Graph = Testing::MakePBRMaterialExpressionsForTest();
	auto Packed = Testing::MakeGraphExpression<DMaterialExpressionMakeMaterialAttributes>();
	for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions())
		*Packed->GetExpressionInputForProperty(Field.Property) = Graph.Outputs.Get(Field.Property).Connection;
	Graph.Expressions.emplace_back(Packed.Get());
	Graph.Outputs.Attributes = {Packed.Get()};
	ASSERT_TRUE(Graph.Apply(*Material));

	Durin::Tests::FTestEditorTransactorOwner Transactions;
	const auto Before = Material->CaptureRootInputs();
	TStrongObjectPtr<DMaterialGraph> InspectionGraph(DMaterialGraph::Create(*Material));
	using P = EMaterialProperty;
	const FGuid ColorPin = InspectionGraph->FindRootPin(P::MP_BaseColor)->GetId();
	const FGuid PackedPin = InspectionGraph->FindRootPin(P::MP_MaterialAttributes)->GetId();
	const auto CheckPins = [&](bool bPacked) {
		const auto* Root = InspectionGraph->GetRootNode();
		ASSERT_TRUE(Root);
		EXPECT_EQ(Root->GetView().Inputs.size(), FMaterialAttributeDefinitionMap::GetDefinitions().size() + 1);
		for (auto [Property, Id, Expected] : {std::tuple{P::MP_BaseColor, ColorPin, Before.Get(P::MP_BaseColor).Connection.GetExpressionId()},
			std::tuple{P::MP_MaterialAttributes, PackedPin, Before.Attributes.GetExpressionId()}})
		{
			const auto* Pin = Root->FindInputPin(Property);
			ASSERT_TRUE(Pin);
			EXPECT_EQ(Pin, Root->FindPinById(Id));
			ASSERT_EQ(Pin->LinkedTo.size(), 1u);
			EXPECT_EQ(Pin->LinkedTo.front()->GetOwningNode()->GetView().Node.Id, Expected);
			EXPECT_EQ(Root->GetView().Inputs[Pin->DisplayIndex].bActive, bPacked == (Property == P::MP_MaterialAttributes));
		}
	};
	CheckPins(false);
	ASSERT_TRUE(MaterialEditing::SetUseMaterialAttributes(*Material, true));
	CheckPins(true);
	EXPECT_EQ(*Material->GetNumericInputForProperty(EMaterialProperty::MP_BaseColor), Before.Get(EMaterialProperty::MP_BaseColor));
	EXPECT_EQ(*Material->GetExpressionInputForProperty(EMaterialProperty::MP_MaterialAttributes), Before.Attributes);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->CaptureRootInputs(), Before);
	CheckPins(false);
	ASSERT_TRUE(Transactions->Redo());
	CheckPins(true);
	// Editing either stored connection set never switches the mode or clears the other.
	ASSERT_TRUE(Testing::WriteConnection(Material, EMaterialProperty::MP_BaseColor, FMaterialProgramLink{}, true, false));
	EXPECT_TRUE(Material->UsesMaterialAttributes());
	EXPECT_EQ(*Material->GetExpressionInputForProperty(EMaterialProperty::MP_MaterialAttributes), Before.Attributes);
	ASSERT_TRUE(Testing::WriteConnection(Material, EMaterialProperty::MP_BaseColor, FMaterialProgramLink{Before.Get(EMaterialProperty::MP_BaseColor).Connection.GetExpressionId(), static_cast<uint8>(Before.Get(EMaterialProperty::MP_BaseColor).Connection.OutputIndex), Before.Get(EMaterialProperty::MP_BaseColor).Connection.GetOutputId()}, true, false));
	ASSERT_TRUE(Testing::WriteConnection(Material, EMaterialProperty::MP_MaterialAttributes, FMaterialProgramLink{}, true, false));
	EXPECT_EQ(*Material->GetNumericInputForProperty(EMaterialProperty::MP_BaseColor), Before.Get(EMaterialProperty::MP_BaseColor));
	// An empty packed input uses standard defaults, independently of retained individual values.
	std::vector<DMaterialExpression*> Expressions;
	for (const auto& Expression : Material->GetExpressionCollection().Expressions) Expressions.push_back(Expression.Get());
	const auto Built = MIR::FGraphBuilder(Expressions).FinishSurface(Material->CaptureRootInputs());
	ASSERT_TRUE(Built);
	EXPECT_FALSE(Built.IR.GetAttribute(EMaterialProperty::MP_BaseColor).bExpression);
	EXPECT_EQ(Built.IR.GetAttribute(EMaterialProperty::MP_BaseColor).Literal.X, 0.0f);
	ASSERT_TRUE(MaterialEditing::SetUseMaterialAttributes(*Material, false, false));
	auto Properties = Material->GetStaticProperties();
	Properties.ShadingModel = EMaterialShadingModel::Unlit;
	Properties.BlendMode = EMaterialBlendMode::Masked;
	ASSERT_TRUE(Material->SetStaticProperties(Properties));
	EXPECT_FALSE(Testing::InputView(*InspectionGraph, P::MP_Metallic)->bActive);
	EXPECT_FALSE(Testing::InputView(*InspectionGraph, P::MP_Opacity)->bActive);
	EXPECT_TRUE(Testing::InputView(*InspectionGraph, P::MP_OpacityMask)->bActive);
	EXPECT_EQ(*Material->GetNumericInputForProperty(EMaterialProperty::MP_BaseColor), Before.Get(EMaterialProperty::MP_BaseColor));
	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphOperationsTests, CreationRequestConnectsAndUndoesAsOneEdit)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	const auto Scalar = Testing::CreateGraphConstant(*AuthoringGraph, .25f);
	ASSERT_TRUE(Scalar);
	const auto Catalog = EnumerateCatalog();
	const auto Entry = std::ranges::find_if(Catalog, [](const auto& E) {
		return E.Opcode == EMaterialProgramOpcode::Add && E.ResultType == EMaterialProgramValueType::Float;
	});
	ASSERT_NE(Entry, Catalog.end());
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	const auto Before = CaptureExpressions(*Material);
	const auto Revision = Material->GetMaterialCompileDebugInfo().AuthoredRevision;
	const auto Action = MakeCreationAction(*Entry);
	EXPECT_TRUE(AuthoringGraph->GetSchema().CanCreate(*Material, Action, EMaterialProgramValueType::Float));
	EXPECT_FALSE(AuthoringGraph->GetSchema().CanCreate(*Material, Action, EMaterialProgramValueType::MaterialAttributes));
	const auto Created = Action.PerformAction(*AuthoringGraph, 120, 240, Testing::ResolvePin(*AuthoringGraph, FMaterialProgramLink{Scalar.value()})).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Created) << FormatMaterialGraphCommandResult(Created);
	const auto View = Testing::CaptureGraphView(*AuthoringGraph);
	const auto* Node = FindViewNode(View, Created.value());
	ASSERT_NE(Node, nullptr);
	EXPECT_EQ(Node->Inputs.front().Link.SourceNodeId, Scalar.value());
	EXPECT_EQ(Node->Presentation.X, 120);
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision + 1);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_FALSE(Transactions->CanUndo());
	ASSERT_TRUE(Transactions->Redo());
	const auto After = CaptureExpressions(*Material);
	Transactions->Reset();
	EXPECT_FALSE(Action.PerformAction(*AuthoringGraph, 0, 0, AuthoringGraph->FindRootPin(EMaterialProperty::MP_Roughness)));
	EXPECT_EQ(CaptureExpressions(*Material), After);
	EXPECT_FALSE(Transactions->CanUndo());
}

TEST(FMaterialGraphOperationsTests, PinConnectionsPreserveDefaultsAndRejectReplacementWithoutHistory)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	const auto A = Testing::CreateGraphConstant(*AuthoringGraph, .2f);
	const auto B = Testing::CreateGraphConstant(*AuthoringGraph, .8f);
	ASSERT_TRUE(A); ASSERT_TRUE(B);
	const auto Target = EMaterialProperty::MP_Roughness;
	const auto Default = Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->Constant[0];
	ASSERT_TRUE(Testing::WriteConnection(Material.Get(), Target, FMaterialProgramLink{A.value()}, false, false));
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	EXPECT_FALSE(Testing::WriteConnection(Material.Get(), Target, FMaterialProgramLink{B.value()}, false));
	EXPECT_FALSE(Testing::WriteConnection(Material.Get(), Target, FMaterialProgramLink{A.value()}, false));
	EXPECT_FALSE(Transactions->CanUndo());
	const auto Replaced = Testing::WriteConnection(Material.Get(), Target, FMaterialProgramLink{B.value()}, true);
	ASSERT_TRUE(Replaced);
	EXPECT_EQ(Material->GetNumericInputForProperty(Target)->Connection.GetExpressionId(), B.value());

	const auto Unchanged = Testing::WriteConnection(Material.Get(), Target, FMaterialProgramLink{B.value()}, true);
	EXPECT_FALSE(Unchanged);

	EXPECT_EQ(Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->Constant[0], Default);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->Connection.GetExpressionId(), A.value());
	const auto Disconnected = Testing::WriteConnection(Material.Get(), Target, FMaterialProgramLink{}, true);
	ASSERT_TRUE(Disconnected);

	EXPECT_FALSE(Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->Connection.IsConnected());
	EXPECT_EQ(Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->Constant[0], Default);
	EXPECT_FALSE(AuthoringGraph->GetSchema().TryCreateConnection(AuthoringGraph->FindRootPin(Target), AuthoringGraph->FindRootPin(Target), true, false));
}

TEST(FMaterialGraphOperationsTests, SharedMoveDraftPreservesIndependentPresentationEdits)
{
	InitializeDObjectSystem();
	for (const bool bFunction : {false, true})
	{
		SCOPED_TRACE(bFunction);
		TStrongObjectPtr<DObject> Owner(bFunction ? static_cast<DObject*>(NewObject<DMaterialFunction>(nullptr, NAME_None))
			: static_cast<DObject*>(NewObject<DMaterial>(nullptr, NAME_None)));

		TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Owner.Get()));
		const auto Created = Testing::CreateGraphConstant(*AuthoringGraph, .5f, 40, 80);
		const auto Other = Testing::CreateGraphConstant(*AuthoringGraph, .7f, 200, 80);
		ASSERT_TRUE(Created); ASSERT_TRUE(Other);
		const auto Id = Created.value(), OtherId = Other.value();
		const auto SemanticRevision = [&]() -> uint64 {
			if (const auto* Material = Cast<DMaterial>(Owner.Get())) return Material->GetMaterialCompileDebugInfo().AuthoredRevision;
			return Cast<DMaterialFunction>(Owner.Get())->GetFunctionRevision();
		};
		const auto Revision = SemanticRevision();
		const auto Read = [&] { return GraphEditInternals::ReadGraphPresentation(*Owner.Get()); };
		const auto Write = [&](FMaterialGraphPresentation P) {
			ASSERT_NE(GraphEditInternals::WriteGraphPresentation(*Owner.Get(), std::move(P)), EMaterialGraphPresentationResult::Rejected);
		};
		auto Initial = Read();
		std::ranges::find(Initial.Nodes, Id, &FMaterialGraphNodePresentation::NodeId)->DisplayName = "Original";
		Write(Initial);
		Durin::Tests::FTestEditorTransactorOwner Transactions;
		FMaterialGraphMoveSession Move;
		ASSERT_TRUE(Move.Begin(*AuthoringGraph, std::array{Id}, Transactions.Get()));
		ASSERT_TRUE(Move.Apply(std::array{FMaterialGraphNodePresentation{Id, 400, 240, "Ignored"}}));
		EXPECT_EQ(Read(), Initial); // Preview cannot dirty or publish the owner.
		EXPECT_EQ((Transactions->GetQueueLength() - Transactions->GetUndoCount()), 0u);
		auto External = Read();
		std::ranges::find(External.Nodes, Id, &FMaterialGraphNodePresentation::NodeId)->DisplayName = "Renamed";
		std::ranges::find(External.Nodes, OtherId, &FMaterialGraphNodePresentation::NodeId)->X = 900;
		Write(External);
		Move.Cancel();
		EXPECT_EQ(Read(), External);
		ASSERT_TRUE(Move.Begin(*AuthoringGraph, std::array{Id}, Transactions.Get()));
		ASSERT_TRUE(Move.Apply(std::array{FMaterialGraphNodePresentation{Id, 400, 240}}));
		ASSERT_TRUE(Move.Commit());
		EXPECT_EQ((Transactions->GetQueueLength() - Transactions->GetUndoCount()), 1u);
		ASSERT_NE(Transactions->GetTransaction(0), nullptr);
		EXPECT_EQ(Transactions->GetTransaction(0)->GetRecordCount(), 1u);
		EXPECT_EQ(AuthoringGraph->FindNode(Id)->GetView().Presentation.DisplayName, "Renamed");
		auto Later = Read();
		std::ranges::find(Later.Nodes, Id, &FMaterialGraphNodePresentation::NodeId)->DisplayName = "After move";
		std::ranges::find(Later.Nodes, OtherId, &FMaterialGraphNodePresentation::NodeId)->X = 1100;
		Write(Later);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(AuthoringGraph->FindNode(Id)->GetView().Presentation.X, 40);
		// Replay restores the moved expression's baseline and leaves the other expression alone.
		EXPECT_EQ(AuthoringGraph->FindNode(Id)->GetView().Presentation.DisplayName, "Renamed");
		EXPECT_EQ(AuthoringGraph->FindNode(OtherId)->GetView().Presentation.X, 1100);
		EXPECT_EQ(SemanticRevision(), Revision);
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(Read(), Later);
		EXPECT_EQ(SemanticRevision(), Revision);
		Transactions->Reset();
		ASSERT_TRUE(Move.Begin(*AuthoringGraph, std::array{Id}, Transactions.Get()));
		ASSERT_TRUE(Move.Apply(std::array{FMaterialGraphNodePresentation{Id, 400, 240}}));
		ASSERT_TRUE(Move.Commit());
		EXPECT_EQ((Transactions->GetQueueLength() - Transactions->GetUndoCount()), 0u);
		// Direct position commands must also ignore the presentation label argument.
		const std::array Positions{
			FMaterialGraphNodePresentation{Id, 450, 240, "Ignored"}, AuthoringGraph->FindNode(OtherId)->GetView().Presentation};
		ASSERT_TRUE(AuthoringGraph->MoveNodes(Positions));
		const auto HistoryLength = Transactions->GetQueueLength();
		ASSERT_TRUE(AuthoringGraph->MoveNodes(Positions));
		EXPECT_EQ(Transactions->GetQueueLength(), HistoryLength);

		EXPECT_EQ(AuthoringGraph->FindNode(Id)->GetView().Presentation.DisplayName, "After move");
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(Read(), Later);

		Transactions->Reset();
		ASSERT_TRUE(Move.Begin(*AuthoringGraph, std::array{Id, OtherId}, Transactions.Get()));
		const std::array GroupPositions{FMaterialGraphNodePresentation{Id, 700, 240},
			FMaterialGraphNodePresentation{OtherId, 1300, 80}};
		ASSERT_TRUE(Move.Apply(GroupPositions));
		EXPECT_EQ(Read(), Later);
		ASSERT_TRUE(Move.Commit());
		ASSERT_EQ(Transactions->GetQueueLength(), 1u);
		EXPECT_EQ(Transactions->GetTransaction(0)->GetRecordCount(), 2u);
		EXPECT_EQ(AuthoringGraph->FindNode(Id)->GetView().Presentation.X, 700);
		EXPECT_EQ(AuthoringGraph->FindNode(OtherId)->GetView().Presentation.X, 1300);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(Read(), Later);
		EXPECT_EQ(SemanticRevision(), Revision);
	}
}

TEST(FMaterialGraphOperationsTests, SharedMoveRejectsInvalidDraftsAndStaleOwners)
{
	InitializeDObjectSystem();
	for (const bool bFunction : {false, true})
	{
		SCOPED_TRACE(bFunction);
		TStrongObjectPtr<DObject> Owner(bFunction ? static_cast<DObject*>(NewObject<DMaterialFunction>(nullptr, NAME_None))
			: static_cast<DObject*>(NewObject<DMaterial>(nullptr, NAME_None)));

		TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Owner.Get()));
		const auto Created = Testing::CreateGraphConstant(*AuthoringGraph, .5f, 40, 80);
		ASSERT_TRUE(Created);
		const auto Id = Created.value();
		Durin::Tests::FTestEditorTransactorOwner Transactions;
		FMaterialGraphMoveSession Move;
		ASSERT_TRUE(Move.Begin(*AuthoringGraph, std::array{Id}, Transactions.Get()));
		const FMaterialGraphNodePresentation Position{Id, 400, 240};
		ASSERT_TRUE(Move.Apply(std::array{Position}));
		const auto Duplicate = Move.Apply(std::array{Position, Position});
		ASSERT_FALSE(Duplicate);
		EXPECT_EQ(Duplicate.error().find("The graph move preview contains a duplicate node GUID."), 0u);
		const auto OutsideId = FGuid::NewGuid();
		const auto Outside = Move.Apply(std::array{FMaterialGraphNodePresentation{OutsideId, 0, 0}});
		ASSERT_FALSE(Outside);
		EXPECT_EQ(Outside.error().find("The graph move preview addresses a node outside the selection."), 0u);
		const auto Coordinate = Move.Apply(std::array{FMaterialGraphNodePresentation{Id, MaterialGraphPresentationCoordinateLimit + 1, 0}});
		ASSERT_FALSE(Coordinate);
		EXPECT_EQ(Coordinate.error().find("The graph move preview is outside the supported coordinate range."), 0u);
		ASSERT_EQ(Move.GetPositions().size(), 1u);
		EXPECT_EQ(Move.GetPositions().front().X, 400);
		ASSERT_TRUE(AuthoringGraph->FindNode(Id)->SetConstantValue(FMaterialParameterValue::MakeScalar(.8f), false));
		const auto Stale = Move.Commit();
		ASSERT_FALSE(Stale);
		EXPECT_EQ(Stale.error().find("The graph changed semantically during the move."), 0u);
		EXPECT_FALSE(Move.IsActive());
		EXPECT_EQ((Transactions->GetQueueLength() - Transactions->GetUndoCount()), 0u);
		EXPECT_EQ(AuthoringGraph->FindNode(Id)->GetView().Presentation.X, 40);
		// A node without an authored presentation can still be moved and undone.
		auto P = GraphEditInternals::ReadGraphPresentation(*Owner.Get());
		std::erase_if(P.Nodes, [&](const auto& N) { return N.NodeId == Id; });
		ASSERT_NE(GraphEditInternals::WriteGraphPresentation(*Owner.Get(), P), EMaterialGraphPresentationResult::Rejected);
		const auto Fallback = AuthoringGraph->FindNode(Id)->GetView().Presentation;
		EXPECT_TRUE(AuthoringGraph->MoveNodes(std::array{Fallback}));
		EXPECT_EQ(GraphEditInternals::ReadGraphPresentation(*Owner.Get()), P);
		ASSERT_TRUE(Move.Begin(*AuthoringGraph, std::array{Id}, Transactions.Get()));
		ASSERT_TRUE(Move.Apply(std::array{Position})); ASSERT_TRUE(Move.Commit());
		EXPECT_EQ(Transactions->GetQueueLength(), 1u);
		EXPECT_EQ(AuthoringGraph->FindNode(Id)->GetView().Presentation, Position);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(GraphEditInternals::ReadGraphPresentation(*Owner.Get()), P);
		EXPECT_EQ(AuthoringGraph->FindNode(Id)->GetView().Presentation, Fallback);
		EXPECT_FALSE(Transactions->CanUndo());
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(AuthoringGraph->FindNode(Id)->GetView().Presentation, Position);
		auto Labeled = GraphEditInternals::ReadGraphPresentation(*Owner.Get());
		std::ranges::find(Labeled.Nodes, Id, &FMaterialGraphNodePresentation::NodeId)->DisplayName = "New label";
		ASSERT_NE(GraphEditInternals::WriteGraphPresentation(*Owner.Get(), Labeled), EMaterialGraphPresentationResult::Rejected);
		ASSERT_TRUE(Transactions->Undo());
		const auto Restored = AuthoringGraph->FindNode(Id)->GetView().Presentation;
		EXPECT_EQ(Restored.X, Fallback.X); EXPECT_EQ(Restored.Y, Fallback.Y);
		EXPECT_EQ(Restored.DisplayName, Fallback.DisplayName);
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(GraphEditInternals::ReadGraphPresentation(*Owner.Get()), Labeled);
	}
}

TEST(FMaterialGraphOperationsTests, FunctionLayoutUsesPositionHistoryAndPreservesLabels)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Function.Get()));
	const auto First = Testing::CreateGraphConstant(*AuthoringGraph, .5f, 0, 0);
	const auto Second = Testing::CreateGraphConstant(*AuthoringGraph, .7f, 0, 0);
	ASSERT_TRUE(First); ASSERT_TRUE(Second);
	auto Original = Function->GetFunctionPresentation();
	Original.Nodes.front().DisplayName = "Keep label";
	ASSERT_TRUE(Function->SetFunctionPresentation(Original));
	const auto Revision = Function->GetFunctionRevision();
	FMaterialGraphPresentation A, B;
	ASSERT_TRUE(AuthoringGraph->CalculateLayout({}, A)); ASSERT_TRUE(AuthoringGraph->CalculateLayout({}, B));
	EXPECT_EQ(A, B);
	EXPECT_EQ(Function->GetFunctionPresentation(), Original);
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	ASSERT_TRUE(AuthoringGraph->Layout({}));
	const auto HistoryLength = Transactions->GetQueueLength();
	ASSERT_TRUE(AuthoringGraph->Layout({}));
	EXPECT_EQ(Transactions->GetQueueLength(), HistoryLength);
	EXPECT_EQ(Function->GetFunctionPresentation().Nodes, A.Nodes);
	ASSERT_TRUE(Transactions->Undo()); EXPECT_EQ(Function->GetFunctionPresentation(), Original);
	ASSERT_TRUE(Transactions->Redo()); EXPECT_EQ(Function->GetFunctionPresentation().Nodes, A.Nodes);
	EXPECT_EQ(Function->GetFunctionRevision(), Revision);
}

TEST(FMaterialGraphOperationsTests, GraphSessionRetainsHistoryFailureAcrossRollbackAndRetry)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material.Get()));
	const auto Created = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float);
	ASSERT_TRUE(Created);
	const auto NodeId = Created->front();
	const auto Before = CaptureExpressions(*Material.Get());
	const auto Revision = Material->GetMaterialCompileDebugInfo().AuthoredRevision;
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	Durin::FTransBufferTestAccess::SetHistoryBusy(*Transactions, true);
	const auto Rejected = AuthoringGraph->FindNode(NodeId)->SetConstantValue(FMaterialParameterValue::MakeScalar(.75f));
	ASSERT_FALSE(Rejected);
	EXPECT_EQ(Rejected.GetStatus(), EMaterialGraphCommandStatus::Rejected);
	EXPECT_NE(Rejected.Message.find("The active editor transaction service is unavailable or busy."), std::string::npos);
	EXPECT_FALSE(Rejected.Message.empty());
	EXPECT_EQ(CaptureExpressions(*Material.Get()), Before);
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision);
	EXPECT_FALSE(Transactions->CanUndo());
	Durin::FTransBufferTestAccess::SetHistoryBusy(*Transactions, false);
	ASSERT_TRUE(AuthoringGraph->FindNode(NodeId)->SetConstantValue(FMaterialParameterValue::MakeScalar(.75f)));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material.Get()), Before);

	EXPECT_NE(Rejected.Message.find("The active editor transaction service is unavailable or busy."), std::string::npos);
}

TEST(FMaterialGraphOperationsTests, RejectedExpressionReplacementLeavesTheOriginalAndHistoryIntact)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
	auto Add = Testing::MakeGraphExpression<DMaterialExpressionAdd>();
	ASSERT_TRUE(Graph->CreateExpression(*Add, 0, 0, false));
	auto* Live = Graph->FindNode(Add->Id)->GetMaterialExpression();
	auto Foreign = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
	Add->A = {Foreign.Get()};
	Tests::FTestEditorTransactorOwner History;
	EXPECT_FALSE(Graph->ReplaceExpression(*Add));
	EXPECT_FALSE(History->CanUndo());
	EXPECT_FALSE(Cast<DMaterialExpressionAdd>(Live)->A.Connection.IsConnected());
	Add->A = {};
	Add->A.SetConstant({.75f});
	ASSERT_TRUE(Graph->ReplaceExpression(*Add));
	EXPECT_EQ(Graph->FindNode(Add->Id)->GetMaterialExpression(), Live);
	ASSERT_TRUE(History->Undo());
	EXPECT_FALSE(Cast<DMaterialExpressionAdd>(Live)->A.UseConstant);
}

TEST(FMaterialGraphOperationsTests, SharedScalarParameterWritesBothDeclarationsOnRelease)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto Id = FGuid::NewGuid();
	const auto Node = MaterialEditing::CreateParameter(*Material, {.Id = Id, .Name = "SharedScalar",
		.Type = EMaterialParameterType::Scalar, .Value = FMaterialParameterValue::MakeScalar(.25f)},
		false).transform(Testing::ExpressionNodeId);
	ASSERT_TRUE(Node);
	TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
	ASSERT_TRUE(Graph->DuplicateNodes(std::array{*Node}, 100, 0, false));
	const auto CheckDeclarations = [&](float Expected) {
		ASSERT_EQ(Material->GetExpressionCollection().Expressions.size(), 2u);
		for (const auto& Expression : Material->GetExpressionCollection().Expressions)
			EXPECT_FLOAT_EQ(Cast<DMaterialExpressionScalarParameter>(Expression.Get())->GetParameterDefinition().Value.GetScalar(), Expected);
		EXPECT_FLOAT_EQ(Material->FindParameterDefinition(Id)->Value.GetScalar(), Expected);
	};
	Tests::FTestEditorTransactorOwner History;
	FMaterialEditorPreviewParameters Parameters(Material);
	auto Preview = Parameters.Find(Id);
	ASSERT_TRUE(Preview);
	PropertyEditor::FPropertyControl Edit;
	ASSERT_TRUE(Edit.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Preview->GetEditTarget()), true));
	EXPECT_TRUE(History->IsActive());
	const auto Applied = Durin::Testing::ApplyControlInput(Edit, .75f);
	EXPECT_TRUE(Applied);
	CheckDeclarations(.25f);
	int Publications = 0;
	const auto Refresh = Material->GetGraphChanges().Subscribe(*Material, [&](const auto&) {
		++Publications;
		CheckDeclarations(.75f);
		EXPECT_EQ(Parameters.Find(Id), Preview);
		EXPECT_FALSE(Edit.IsActive());
		EXPECT_TRUE(Edit.Finish(true));
	});
	const auto Committed = Edit.Finish(false);
	Material->GetGraphChanges().Unsubscribe(Refresh);
	ASSERT_TRUE(Committed);
	EXPECT_EQ(Publications, 1);
	CheckDeclarations(.75f);
	ASSERT_EQ(History->GetQueueLength(), 1u);
	EXPECT_EQ(History->GetTransaction(0)->GetRecordCount(), 3u);
	ASSERT_TRUE(History->Undo());
	CheckDeclarations(.25f);
	ASSERT_TRUE(History->Redo());
	CheckDeclarations(.75f);

	// An unrelated outer edit survives cancellation of a nested preview.
	ASSERT_EQ(History->Begin("test", "Outer edit"), 0);
	const auto OtherId = FGuid::NewGuid();
	ASSERT_TRUE(MaterialEditing::CreateParameter(*Material, {.Id = OtherId, .Name = "OuterParameter",
		.Type = EMaterialParameterType::Scalar, .Value = FMaterialParameterValue::MakeScalar(.1f)}, false));
	Preview = Parameters.Find(Id);
	ASSERT_TRUE(Preview);
	ASSERT_TRUE(Edit.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Preview->GetEditTarget()), true));
	ASSERT_TRUE(Durin::Testing::ApplyControlInput(Edit, .9f));
	ASSERT_TRUE(Edit.Finish(true));
	EXPECT_TRUE(History->IsActive());
	ASSERT_EQ(History->End(), 1);
	ASSERT_TRUE(History->Undo());
	EXPECT_EQ(Material->FindParameterDefinition(OtherId), nullptr);
	CheckDeclarations(.75f);
}

TEST(FMaterialGraphOperationsTests, CanceledParameterGestureExitsBeforeReplacementAndBusyMoveCanRetry)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialParameterDefinition Definition;
	Definition.Name = "GestureValue";
	Definition.Type = EMaterialParameterType::Scalar;
	Definition.Value = FMaterialParameterValue::MakeScalar(.25f);
	const auto Created = MaterialEditing::CreateParameter(*Material, Definition, false).transform(Testing::ExpressionNodeId);
	ASSERT_TRUE(Created);

	const auto* CreatedParameter = FindExpression<DMaterialExpressionParameter>(*Material, Created.value());
	ASSERT_NE(CreatedParameter, nullptr);
	const auto ParameterId = CreatedParameter->Metadata.Id;
	Tests::FTestEditorTransactorOwner Transactions;
	FMaterialEditorPreviewParameters Parameters(Material);
	auto Preview = Parameters.Find(ParameterId);
	ASSERT_TRUE(Preview);
	PropertyEditor::FPropertyControl Edit;
	ASSERT_TRUE(Edit.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Preview->GetEditTarget()), true));
	ASSERT_TRUE(Durin::Testing::ApplyControlInput(Edit, .75f));
	ASSERT_TRUE(Edit.Finish(true));
	EXPECT_FLOAT_EQ(Material->FindParameterDefinition(ParameterId)->Value.GetScalar(), .75f);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FLOAT_EQ(Material->FindParameterDefinition(ParameterId)->Value.GetScalar(), .25f);
	ASSERT_TRUE(Edit.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Preview->GetEditTarget()), true));
	ASSERT_TRUE(Durin::Testing::ApplyControlInput(Edit, .5f));
	ASSERT_TRUE(Edit.Finish(false));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FLOAT_EQ(Material->FindParameterDefinition(ParameterId)->Value.GetScalar(), .25f);

	// Let the control unwind its canceled gesture before opening independent history.
	Parameters.RegenerateArrays();
	Preview = Parameters.Find(ParameterId);
	ASSERT_TRUE(Preview);
	ASSERT_TRUE(Edit.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Preview->GetEditTarget()), true));
	ASSERT_TRUE(Durin::Testing::ApplyControlInput(Edit, .9f));
	Transactions->Cancel();
	EXPECT_FALSE(Edit.Finish(false));
	ASSERT_EQ(Transactions->Begin("test", "Replacement"), 0);
	EXPECT_TRUE(Transactions->IsActive());
	EXPECT_FLOAT_EQ(Material->FindParameterDefinition(ParameterId)->Value.GetScalar(), .25f);
	ASSERT_EQ(Transactions->End(), 1);

	Transactions->Reset();
	const auto NodeId = Created.value();
	const auto Before = Material->GetMaterialGraphPresentation();
	FMaterialGraphMoveSession Move;
	TStrongObjectPtr<DMaterialGraph> EditorGraph(DMaterialGraph::Create(*Material));
	ASSERT_TRUE(Move.Begin(*EditorGraph, std::array{NodeId}, Transactions.Get()));
	ASSERT_TRUE(Move.Apply(std::array{FMaterialGraphNodePresentation{NodeId, 777, 222}}));
	Durin::FTransBufferTestAccess::SetHistoryBusy(*Transactions, true);
	const auto MoveFailure = Move.Commit();
	ASSERT_FALSE(MoveFailure);
	EXPECT_NE(MoveFailure.error().find("The editor transactor is busy."), std::string::npos);
	EXPECT_TRUE(Move.IsActive());
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), Before);
	Durin::FTransBufferTestAccess::SetHistoryBusy(*Transactions, false);
	ASSERT_TRUE(Move.Commit());
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), Before);
	EXPECT_NE(MoveFailure.error().find("The editor transactor is busy."), std::string::npos);
}

TEST(FMaterialGraphOperationsTests, PreviewWriteRejectsBusyHistoryAndKeepsPublishedValueWhenObserverCancels)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialParameterDefinition Definition;
	Definition.Name = "DirectValue";
	Definition.Type = EMaterialParameterType::Scalar;
	Definition.Value = FMaterialParameterValue::MakeScalar(.25f);
	const auto Created = MaterialEditing::CreateParameter(*Material, Definition, false).transform(Testing::ExpressionNodeId);
	ASSERT_TRUE(Created);

	const auto* CreatedParameter = FindExpression<DMaterialExpressionParameter>(*Material, Created.value());
	ASSERT_NE(CreatedParameter, nullptr);
	const auto ParameterId = CreatedParameter->Metadata.Id;
	Tests::FTestEditorTransactorOwner Transactions;
	FMaterialEditorPreviewParameters Parameters(Material);
	auto Preview = Parameters.Find(ParameterId);
	ASSERT_TRUE(Preview);
	const auto Handle = PropertyEditor::MakePropertyHandle(Preview->GetEditTarget());
	Durin::FTransBufferTestAccess::SetHistoryBusy(*Transactions, true);
	ASSERT_EQ(Handle->SetValue(.75f), PropertyEditor::FPropertyAccess::Fail);
	EXPECT_FALSE(Handle->GetLastError().empty());
	EXPECT_FALSE(Transactions->CanUndo());
	EXPECT_FLOAT_EQ(Material->FindParameterDefinition(ParameterId)->Value.GetScalar(), .25f);
	Durin::FTransBufferTestAccess::SetHistoryBusy(*Transactions, false);
	ASSERT_EQ(Handle->SetValue(.75f), PropertyEditor::FPropertyAccess::Success);

	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FLOAT_EQ(Material->FindParameterDefinition(ParameterId)->Value.GetScalar(), .25f);

	Parameters.RegenerateArrays();
	Preview = Parameters.Find(ParameterId);
	ASSERT_TRUE(Preview);
	const auto RetryHandle = PropertyEditor::MakePropertyHandle(Preview->GetEditTarget());
	int Publications = 0;
	const auto Observer = Material->GetGraphChanges().Subscribe(*Material, [&](const auto&) {
		++Publications;
		Transactions->Cancel();
	});
	EXPECT_EQ(RetryHandle->SetValue(.75f), PropertyEditor::FPropertyAccess::Success);
	Material->GetGraphChanges().Unsubscribe(Observer);
	EXPECT_EQ(Publications, 1);
	EXPECT_FLOAT_EQ(Preview->GetValue().GetScalar(), .75f);
	EXPECT_FLOAT_EQ(Material->FindParameterDefinition(ParameterId)->Value.GetScalar(), .75f);
	EXPECT_FALSE(Transactions->CanUndo());
}

TEST(FMaterialGraphOperationsTests, BaseParameterMutationRetainsFailuresBeforePublication)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto Invalid = Material->SetParameterValueByInfo({}, FMaterialParameterValue::MakeScalar(.5f));
	EXPECT_FALSE(Invalid);
	const FName MissingName("MissingParameter");
	const auto Missing = Material->SetParameterValueByInfo({.Name = MissingName}, FMaterialParameterValue::MakeScalar(.5f));
	ASSERT_FALSE(Missing);
	EXPECT_NE(Missing.error().find(MissingName.ToString()), std::string::npos);
	FMaterialParameterDefinition Definition;
	Definition.Name = "MutationCause";
	Definition.Type = EMaterialParameterType::Scalar;
	Definition.Value = FMaterialParameterValue::MakeScalar(.25f);
	const auto Created = MaterialEditing::CreateParameter(*Material, Definition, false).transform(Testing::ExpressionNodeId);
	ASSERT_TRUE(Created);

	const auto* CreatedParameter = FindExpression<DMaterialExpressionParameter>(*Material, Created.value());
	ASSERT_NE(CreatedParameter, nullptr);
	const auto Id = CreatedParameter->Metadata.Id;
	auto* Owner = Cast<DMaterialExpressionScalarParameter>(Material->GetExpressionCollection().Expressions.front().Get());
	ASSERT_NE(Owner, nullptr);
	Owner->DefaultValue = .1f;
	const auto Mismatch = Material->SetScalarParameterValue("MutationCause", .75f);
	ASSERT_FALSE(Mismatch);
	EXPECT_NE(Mismatch.error().find(Id.ToString()), std::string::npos);
	EXPECT_FLOAT_EQ(Owner->DefaultValue, .1f);
	EXPECT_FLOAT_EQ(Material->FindParameterDefinition(Id)->Value.GetScalar(), .25f);
	Owner->DefaultValue = .25f;
	ASSERT_TRUE(Material->SetParameterValueByInfo(Definition.GetParameterInfo(), FMaterialParameterValue::MakeScalar(.75f)));
	EXPECT_FLOAT_EQ(Owner->DefaultValue, .75f);
}

TEST(FMaterialGraphOperationsTests, SerializedParameterMetadataResolvesTheOriginalStructName)
{
	InitializeDObjectSystem();
	auto* Metadata = FindStructByQualifiedName("Durin::FMaterialExpressionParameterMetadata");
	ASSERT_NE(Metadata, nullptr);
	EXPECT_EQ(FindStructBySerializedName("Durin::FMaterialParameterMetadata"), Metadata);
}

TEST(FMaterialGraphOperationsTests, ExpressionParameterWritesMatchByNameAndTypeAndNotifyOnChange)
{
	InitializeDObjectSystem();
	using Flags = EMaterialExpressionSetParameterValueFlags;
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialParameterDefinition Definition;
	Definition.Id = FGuid::NewGuid();
	Definition.Name = "Weight";
	Definition.Value = FMaterialParameterValue::MakeScalar(.25f);
	ASSERT_TRUE(MaterialEditing::CreateParameter(*Material, Definition, false));
	auto* Parameter = Cast<DMaterialExpressionScalarParameter>(Material->GetExpressionCollection().Expressions.front().Get());
	ASSERT_NE(Parameter, nullptr);
	DMaterialExpression& Expression = *Parameter;
	FMaterialParameterMetadata Meta(FMaterialParameterValue::MakeScalar(.75f));
	Meta.Group = "Surface";
	Meta.SortPriority = 7;
	int Notifications = 0;
	const auto Observer = Material->GetGraphChanges().Subscribe(*Material, [&](const auto&) { ++Notifications; });
	EXPECT_FALSE(Expression.SetParameterValue("Other", Meta));
	EXPECT_FALSE(Expression.SetParameterValue("Weight", FMaterialParameterValue::MakeVector4(FVector4(1.0))));
	EXPECT_FLOAT_EQ(Parameter->DefaultValue, .25f);
	EXPECT_TRUE(Expression.SetParameterValue("Weight", Meta, Flags::SendPostEditChangeProperty));
	EXPECT_FLOAT_EQ(Parameter->DefaultValue, .75f);
	EXPECT_NE(Parameter->Metadata.GroupName, Meta.Group);
	EXPECT_EQ(Notifications, 1);
	EXPECT_TRUE(Expression.SetParameterValue("Weight", Meta, Flags::SendPostEditChangeProperty | Flags::AssignGroupAndSortPriority));
	EXPECT_EQ(Parameter->Metadata.GroupName, Meta.Group);
	EXPECT_EQ(Parameter->Metadata.SortOrder, Meta.SortPriority);
	EXPECT_EQ(Notifications, 1);
	Material->GetGraphChanges().Unsubscribe(Observer);
}

TEST(FMaterialGraphOperationsTests, ParameterDefinitionFailurePreservesExpressionAndAllowsRetry)
{
	InitializeDObjectSystem();
	auto Parameter = Testing::MakeGraphExpression<DMaterialExpressionScalarParameter>();
	Parameter->Metadata = {FGuid::NewGuid(), "DefinitionCause"};
	Parameter->DefaultValue = .25f;
	const auto Before = Parameter->GetParameterDefinition();
	auto Invalid = Before;
	Invalid.Value = FMaterialParameterValue::MakeScalar(std::numeric_limits<float>::infinity());
	const auto Rejected = Parameter->SetParameterDefinition(Invalid);
	ASSERT_FALSE(Rejected);
	EXPECT_NE(Rejected.error().find(Before.Id.ToString()), std::string::npos);
	EXPECT_EQ(Parameter->GetParameterDefinition(), Before);
	Invalid.Value = FMaterialParameterValue::MakeScalar(.75f);
	ASSERT_TRUE(Parameter->SetParameterDefinition(Invalid));
	EXPECT_FLOAT_EQ(Parameter->DefaultValue, .75f);
}

TEST(FMaterialGraphOperationsTests, ParameterFactoryReportsValidationAndNormalizesValidRetry)
{
	InitializeDObjectSystem();
	FMaterialParameterDefinition Definition;
	Definition.Id = FGuid::NewGuid();
	Definition.Type = EMaterialParameterType::Vector;
	Definition.Value = FMaterialParameterValue::MakeVector({.2f, .3f, .4f});
	const auto Rejected = GraphEditInternals::MakeParameterExpression(Definition);
	ASSERT_FALSE(Rejected);
	EXPECT_FALSE(Rejected.Expression);
	EXPECT_EQ(Rejected.Message.find("The parameter definition is invalid."), 0u);

	Definition.Name = "ValidFactory";
	const auto Retried = GraphEditInternals::MakeParameterExpression(Definition);
	ASSERT_TRUE(Retried);
	ASSERT_TRUE(Retried.Expression);
	EXPECT_EQ(Retried.Expression->GetParameterDefinition().Type, EMaterialParameterType::Vector4);
	EXPECT_EQ(Retried.Expression->Metadata.Id, Definition.Id);
}

TEST(FMaterialGraphOperationsTests, LayoutRejectionsLeaveAuthoredPositionsUnchanged)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	const auto Created = Testing::CreateGraphConstant(*AuthoringGraph, .5f, 40, 80);
	ASSERT_TRUE(Created);
	const auto Id = Created.value();
	const auto Before = Material->GetMaterialGraphPresentation();
	FMaterialGraphPresentation Candidate;
	std::string Error;
	EXPECT_FALSE(AuthoringGraph->CalculateLayout(std::array{Id, Id}, Candidate, &Error));
	EXPECT_EQ(Error.find("The material graph layout request contains duplicate node GUIDs."), 0u);
	EXPECT_EQ(Candidate, Before);
	const auto MissingId = FGuid::NewGuid();
	EXPECT_FALSE(AuthoringGraph->CalculateLayout(std::array{MissingId}, Candidate, &Error));
	EXPECT_EQ(Error.find("A material graph layout node does not exist."), 0u);
	const std::vector<FGuid> Oversized(MaterialProgramMaxNodeCount + 2u, Id);
	EXPECT_FALSE(AuthoringGraph->CalculateLayout(Oversized, Candidate, &Error));
	EXPECT_EQ(Error.find("The material graph layout request exceeds the node bound."), 0u);
	EXPECT_FALSE(AuthoringGraph->MoveNodes(std::array{FMaterialGraphNodePresentation{Id, MaterialGraphPresentationCoordinateLimit + 1, 0}}, false, &Error));
	EXPECT_EQ(Error.find("A material graph position is outside the supported coordinate range."), 0u);
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), Before);
	ASSERT_TRUE(AuthoringGraph->CalculateLayout(std::array{Id}, Candidate, &Error));
	EXPECT_TRUE(Error.empty());
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), Before);
	ASSERT_TRUE(AuthoringGraph->MoveNodes(std::array{FMaterialGraphNodePresentation{Id, 100, 200}}, false));
}

TEST(FMaterialGraphOperationsTests, ClipboardRejectionsReportFailureAndAllowRetry)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	const auto Created = Testing::CreateGraphConstant(*AuthoringGraph, .5f, 40, 80);
	ASSERT_TRUE(Created);
	const auto Id = Created.value();
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(AuthoringGraph->CopySelection(std::array{Id}, Payload));
	std::string Error;
	EXPECT_FALSE(AuthoringGraph->CopySelection({}, Payload, &Error));
	EXPECT_EQ(Error.find("The material graph copy selection is empty or exceeds the node bound."), 0u);
	EXPECT_TRUE(Payload.Nodes.empty());
	ASSERT_TRUE(AuthoringGraph->CopySelection(std::array{Id}, Payload, &Error));
	EXPECT_TRUE(Error.empty());
	const auto Before = CaptureExpressions(*Material);
	const auto Presentation = Material->GetMaterialGraphPresentation();
	Tests::FTestEditorTransactorOwner Transactions;
	++Payload.SchemaVersion;
	const auto Schema = AuthoringGraph->Paste(Payload, 0, 0);
	ASSERT_FALSE(Schema);
	EXPECT_EQ(Schema.error().Message.find("The material graph clipboard schema version is unsupported."), 0u);
	Payload.SchemaVersion = CurrentMaterialGraphClipboardSchemaVersion;
	Payload.Nodes.front().RelativeX = -1;
	const auto Position = AuthoringGraph->Paste(Payload, 0, 0);
	ASSERT_FALSE(Position);
	EXPECT_EQ(Position.error().Message.find("The material graph clipboard contains an invalid relative position."), 0u);
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), Presentation);
	EXPECT_FALSE(Transactions->CanUndo());
	Payload.Nodes.front().RelativeX = 0;
	ASSERT_TRUE(AuthoringGraph->Paste(Payload, 100, 200));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
}

TEST(FMaterialGraphOperationsTests, InputDefaultRejectionsDoNotPublish)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	ASSERT_TRUE(Testing::CreateGraphConstant(*AuthoringGraph, .5f, 40, 80));
	const auto OutputId = AuthoringGraph->GetRootNode()->GetView().Node.Id;
	const auto Pin = EMaterialProperty::MP_Metallic;
	const auto Before = CaptureExpressions(*Material);
	Tests::FTestEditorTransactorOwner Transactions;
	const auto MissingId = FGuid::NewGuid();
	const auto Missing = Testing::WriteInputDefault(Material.Get(), Testing::ExpressionInput(MissingId, 3, {}), {});
	ASSERT_FALSE(Missing);
	EXPECT_EQ(Missing.Message.find("The input owner is unavailable."), 0u);
	const auto NonNumeric = GraphEditInternals::WriteInputDefault(Material.Get(), Pin, {.Kind = EMaterialInputDefaultKind::Literal, .Type = EMaterialProgramValueType::Texture2D});
	EXPECT_FALSE(NonNumeric);
	EXPECT_EQ(NonNumeric.Message.find("The input default is not numeric."), 0u);
	const auto Width = GraphEditInternals::WriteInputDefault(Material.Get(), Pin, {.Kind = EMaterialInputDefaultKind::Literal, .Type = EMaterialProgramValueType::Float3, .Literal = {1, 2, 3}});
	EXPECT_FALSE(Width);
	EXPECT_EQ(Width.Message.find("The input default has an incompatible width."), 0u);
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_FALSE(Transactions->CanUndo());
	ASSERT_TRUE(GraphEditInternals::WriteInputDefault(Material.Get(), Pin, {.Kind = EMaterialInputDefaultKind::Literal, .Type = EMaterialProgramValueType::Float, .Literal = {.X = .7f}}));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
}

TEST(FMaterialGraphOperationsTests, NodeValueRejectionsReportFailureAndAllowRetry)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	const auto Created = Testing::CreateGraphConstant(*AuthoringGraph, .5f, 40, 80);
	ASSERT_TRUE(Created);
	const auto Id = Created.value();
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	const auto Before = CaptureExpressions(*Material);
	const auto BadValue = AuthoringGraph->FindNode(Id)->SetConstantValue(FMaterialParameterValue::MakeScalar(std::numeric_limits<float>::infinity()));
	ASSERT_FALSE(BadValue);
	EXPECT_EQ(BadValue.Message.find("Constant values must be finite."), 0u);
	ASSERT_EQ(BadValue.Diagnostics.size(), 1u);
	EXPECT_EQ(BadValue.Diagnostics.front().Message, "Expression constant must be finite.");
	const auto BadComponents = AuthoringGraph->FindNode(Id)->SetSwizzleComponents(std::array<uint8, 2>{0, 7});
	ASSERT_FALSE(BadComponents);
	EXPECT_EQ(BadComponents.Message.find("Swizzles require one to four valid components."), 0u);
	const auto WrongNode = AuthoringGraph->FindNode(Id)->SetSwizzleComponents(std::array<uint8, 1>{0});
	EXPECT_EQ(WrongNode.Message.find("The selected expression is not a swizzle."), 0u);
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_FALSE(Transactions.Get()->CanUndo());
	ASSERT_TRUE(AuthoringGraph->FindNode(Id)->SetConstantValue(FMaterialParameterValue::MakeScalar(.75f)));
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_EQ(BadValue.Message.find("Constant values must be finite."), 0u);
}

TEST(FMaterialGraphOperationsTests, ConnectionRejectionsPreserveExistingBindings)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	const auto First = Testing::CreateGraphConstant(*AuthoringGraph, .5f, 40, 80);
	const auto Second = Testing::CreateGraphConstant(*AuthoringGraph, .75f, 40, 160);
	ASSERT_TRUE(First);
	ASSERT_TRUE(Second);
	const auto Target = EMaterialProperty::MP_Metallic;
	const auto Source = FMaterialProgramLink{Second.value()};
	ASSERT_TRUE(Testing::WriteConnection(Material.Get(), Target, FMaterialProgramLink{First.value()}, true, false));
	const auto Before = CaptureExpressions(*Material);
	Tests::FTestEditorTransactorOwner Transactions;
	std::string Error;
	EXPECT_FALSE(Testing::WriteConnection(Material.Get(), Target, Source, false, true, &Error));
	EXPECT_EQ(Error.find("The graph input is already connected."), 0u);
	EXPECT_FALSE(GraphEditInternals::WriteRootConnection(Material.Get(), Target, {Testing::FindExpression(Material.Get(), Second.value()), 256},
		true, true, &Error));
	EXPECT_EQ(Error, "The source output no longer exists.");
	EXPECT_FALSE(Testing::WriteConnection(Material.Get(), static_cast<EMaterialProperty>(254), Source, true, true, &Error));
	EXPECT_EQ(Error, "The material attribute pin is invalid.");
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_FALSE(Transactions->CanUndo());
	ASSERT_TRUE(Testing::WriteConnection(Material.Get(), Target, Source, true));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
}

TEST(FMaterialGraphOperationsTests, DocumentStructureRejectionsPreserveState)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	const auto Created = Testing::CreateGraphConstant(*AuthoringGraph, .5f, 40, 80);
	ASSERT_TRUE(Created);
	Tests::FTestEditorTransactorOwner Transactions;
	const auto Before = CaptureExpressions(*Material);
	const auto OutputId = AuthoringGraph->GetRootNode()->GetView().Node.Id;
	const auto Output = AuthoringGraph->RemoveNodes(std::array{OutputId});
	EXPECT_EQ(Output.Message.find("The material root cannot be removed."), 0u);
	const std::vector<FGuid> Oversized(MaterialProgramMaxNodeCount + 1);
	const auto Bounds = AuthoringGraph->RemoveNodes(Oversized);
	EXPECT_EQ(Bounds.Message.find("The removal selection exceeds the graph bound."), 0u);
	auto Replacement = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
	const auto Missing = AuthoringGraph->ReplaceExpression(*Replacement);
	EXPECT_EQ(Missing.Message.find("The expression no longer exists."), 0u);
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_FALSE(Transactions->CanUndo());
	Replacement->Id = Created.value();
	Replacement->Value = .75f;
	ASSERT_TRUE(AuthoringGraph->ReplaceExpression(*Replacement));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
}

TEST(FMaterialGraphOperationsTests, CatalogRejectionsReportStaleShapeAndAllowRetry)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	const auto Catalog = EnumerateCatalog();
	const auto Found = std::ranges::find_if(Catalog, [](const auto& Entry) {
		return Entry.Opcode == EMaterialProgramOpcode::MakeMaterialAttributes;
	});
	ASSERT_NE(Found, Catalog.end());
	auto Invalid = *Found;
	Invalid.ExpressionClass = DMaterialExpressionScalarConstant::StaticClass();
	Tests::FTestEditorTransactorOwner Transactions;
	const auto Before = CaptureExpressions(*Material);
	const auto Failure = Testing::CreateGraphCatalogNode(*AuthoringGraph, Invalid, 40, 80, {});
	ASSERT_FALSE(Failure);
	EXPECT_EQ(Failure.error().Message.find("The catalog expression shape is stale."), 0u);
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_FALSE(Transactions->CanUndo());
	ASSERT_TRUE(Testing::CreateGraphCatalogNode(*AuthoringGraph, *Found, 40, 80, {}));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
}

TEST(FMaterialGraphOperationsTests, CreationErrorsReportPathFailureAndAllowRetry)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	const auto Constant = Testing::CreateGraphConstant(*AuthoringGraph, .5f, 40, 80);
	ASSERT_TRUE(Constant);
	const auto Source = FMaterialProgramLink{Constant.value()};
	Tests::FTestEditorTransactorOwner Transactions;
	const auto Before = CaptureExpressions(*Material);
	auto Action = MakeFunctionCreationAction("invalid-relative-function");
	const auto Failed = Action.PerformAction(*AuthoringGraph, 0, 0, Testing::ResolvePin(*AuthoringGraph, Source));
	ASSERT_FALSE(Failed);
	EXPECT_FALSE(Failed.error().Message.empty());
	FTopLevelAssetPath Path;
	const auto Expected = FTopLevelAssetPath::TryCreateWithDiagnostic("invalid-relative-function", Path);
	EXPECT_NE(Failed.error().Message.find(ToString(Expected.error())), std::string::npos);
	Action.Payload = std::string("changed-request");
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_FALSE(Transactions->CanUndo());
	const auto Catalog = EnumerateCatalog();
	const auto Found = std::ranges::find_if(Catalog, [](const auto& Entry) {
		return Entry.Opcode == EMaterialProgramOpcode::MakeMaterialAttributes;
	});
	ASSERT_NE(Found, Catalog.end());
	ASSERT_TRUE(MakeCreationAction(*Found).PerformAction(*AuthoringGraph, 40, 160));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
}

TEST(FMaterialGraphOperationsTests, OperationRejectionsReportParameterSurfaceAndConnectionFailures)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	const auto Constant = Testing::CreateGraphConstant(*AuthoringGraph, .5f, 40, 80);
	ASSERT_TRUE(Constant);
	Tests::FTestEditorTransactorOwner Transactions;
	const auto Before = CaptureExpressions(*Material);
	const auto ParameterId = FGuid::NewGuid();
	const auto Missing = MaterialEditing::RenameParameter(*Material, ParameterId, "NewName");
	EXPECT_EQ(Missing.Message.find("Parameter owner is unavailable."), 0u);
	const auto InvalidOutput = static_cast<EMaterialProperty>(254);
	std::string Error;
	EXPECT_FALSE(Testing::WriteConnection(&*Material, InvalidOutput, FMaterialProgramLink{}, true, true, &Error));
	EXPECT_EQ(Error.find("The material attribute pin is invalid."), 0u);
	const auto Target = EMaterialProperty::MP_Metallic;
	auto Source = FMaterialProgramLink{FGuid::NewGuid()};
	EXPECT_FALSE(Testing::WriteConnection(Material.Get(), Target, Source, true, true, &Error));
	EXPECT_EQ(Error.find("The source output no longer exists."), 0u);
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_FALSE(Transactions->CanUndo());
	Source = FMaterialProgramLink{Constant.value()};
	ASSERT_TRUE(Testing::WriteConnection(Material.Get(), Target, Source, true));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
}

TEST(FMaterialGraphOperationsTests, CommandStatusAndFailurePresentationAreIndependent)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	TStrongObjectPtr<DMaterialGraph> EditorGraph(DMaterialGraph::Create(*Material));
	auto Rejected = EditorGraph->FindNode(EditorGraph->GetRootNode()->GetView().Node.Id)->SetSwizzleComponents({}, false);
	ASSERT_FALSE(Rejected);
	EXPECT_EQ(Rejected.GetStatus(), EMaterialGraphCommandStatus::Rejected);
	Rejected.Message.clear();
	ASSERT_FALSE(Rejected);
	EXPECT_FALSE(FormatMaterialGraphCommandResult(Rejected).empty());
	const auto NoChange = EditorGraph->RemoveNodes({}, false);
	EXPECT_TRUE(NoChange);
	EXPECT_FALSE(NoChange.HasError());
	EXPECT_EQ(NoChange.GetStatus(), EMaterialGraphCommandStatus::NoChange);
	EXPECT_TRUE(FormatMaterialGraphCommandResult(NoChange).empty());
}

TEST(FMaterialGraphOperationsTests, SharedTextureSelectionRecordsPreviewAndDeclarationsAndRetainsTextures)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	auto* Original = NewObject<DTexture2D>(nullptr, "HistoryTextureBefore");
	auto* Replacement = NewObject<DTexture2D>(nullptr, "HistoryTextureAfter");
	const TWeakObjectPtr<DTexture2D> Before(Original), After(Replacement);
	const auto Id = FGuid::NewGuid();
	const auto Parameter = MaterialEditing::CreateParameter(*Material, {.Id = Id, .Name = "SharedTexture",
		.Type = EMaterialParameterType::Texture, .Value = FMaterialParameterValue::MakeTexture(Original)},
		false).transform(Testing::ExpressionNodeId);
	ASSERT_TRUE(Parameter);
	TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
	ASSERT_TRUE(Graph->DuplicateNodes(std::array{*Parameter}, 100, 0, false));
	Tests::FTestEditorTransactorOwner History;
	FMaterialEditorPreviewParameters Parameters(Material);
	auto Preview = Parameters.Find(Id);
	ASSERT_TRUE(Preview);
	const auto Handle = PropertyEditor::MakePropertyHandle(Preview->GetEditTarget());
	ASSERT_EQ(Handle->SetValue(FMaterialParameterValue::MakeTexture(Replacement).GetTexture()), PropertyEditor::FPropertyAccess::Success);
	ASSERT_EQ(History->GetQueueLength(), 1u);
	EXPECT_EQ(History->GetTransaction(0)->GetRecordCount(), 3u);
	const auto RecordedObjects = History->GetTransaction(0)->GetTransactionObjects();
	EXPECT_FALSE(std::ranges::contains(RecordedObjects, Material.Get()));
	EXPECT_EQ(std::ranges::count_if(RecordedObjects, [](const auto* Object) {
		return Object && Object->IsA(DMaterialEditorPreviewParameter::StaticClass());
	}), 1);
	const auto CheckValue = [&](DTexture* Expected) {
		ASSERT_NE(Expected, nullptr);
		const auto* Definition = Material->FindParameterDefinition(Id);
		ASSERT_NE(Definition, nullptr);
		EXPECT_EQ(Definition->Value.GetTexture().Texture.Get(), Expected);
		for (const auto& Expression : Material->GetExpressionCollection().Expressions)
			EXPECT_EQ(Cast<DMaterialExpressionTextureParameter>(Expression.Get())->DefaultValue.Texture.Get(), Expected);
	};
	CollectGarbage(EObjectFlags::Standalone);
	CheckValue(After.Get());
	ASSERT_TRUE(History->Undo());
	CheckValue(Before.Get());
	CollectGarbage(EObjectFlags::Standalone);
	ASSERT_TRUE(History->Redo());
	CheckValue(After.Get());
}

TEST(FMaterialGraphOperationsTests, ResetAndReenableConstantsRetainValuesAndUndoState)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	Durin::Tests::FTestEditorTransactorOwner Transactions;
	const auto Id = FGuid{};
	const auto Pin = EMaterialProperty::MP_Roughness;
	ASSERT_TRUE(GraphEditInternals::WriteInputDefault(Material.Get(), Pin, {.Kind = EMaterialInputDefaultKind::Literal,
		.Type = EMaterialProgramValueType::Float, .Literal = {.5f}}));
	EXPECT_TRUE(Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->UseConstant);
	ASSERT_TRUE(GraphEditInternals::SetInputConstantEnabled(Material.Get(), Pin, false));
	EXPECT_FALSE(Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->UseConstant);
	EXPECT_TRUE(std::ranges::equal(Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->Constant, std::array{.5f}));
	EXPECT_EQ(GraphEditInternals::SetInputConstantEnabled(Material.Get(), Pin, false, false).GetStatus(), EMaterialGraphCommandStatus::NoChange);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_TRUE(Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->UseConstant);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_FALSE(Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->UseConstant);
	ASSERT_TRUE(GraphEditInternals::SetInputConstantEnabled(Material.Get(), Pin, true, false));
	EXPECT_TRUE(Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->UseConstant);
	EXPECT_TRUE(std::ranges::equal(Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->Constant, std::array{.5f}));
}

TEST(FMaterialGraphOperationsTests, DirectCreationRejectsMismatchedShapesAndUnboundCollectionParameters)
{
	InitializeDObjectSystem();
	const auto Catalog = EnumerateCatalog();
	const auto Scalar = std::ranges::find(Catalog, DMaterialExpressionScalarConstant::StaticClass(), &FMaterialGraphCatalogEntry::ExpressionClass);
	const auto Vector = std::ranges::find(Catalog, DMaterialExpressionVector3Constant::StaticClass(), &FMaterialGraphCatalogEntry::ExpressionClass);
	ASSERT_NE(Scalar, Catalog.end()); ASSERT_NE(Vector, Catalog.end());
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	auto Forged = *Vector;
	Forged.ExpressionClass = Scalar->ExpressionClass;
	const auto Before = CaptureExpressions(*Material);
	EXPECT_FALSE(Testing::CreateGraphCatalogNode(*AuthoringGraph, Forged, 0, 0, {}, false));
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	ASSERT_TRUE(Testing::CreateGraphCatalogNode(*AuthoringGraph, *Vector, 0, 0, {}, false));
	Forged.ExpressionClass = DMaterialExpressionCollectionParameter::StaticClass();
	const auto Created = CaptureExpressions(*Material);
	EXPECT_FALSE(Testing::CreateGraphCatalogNode(*AuthoringGraph, Forged, 0, 0, {}, false));
	EXPECT_EQ(CaptureExpressions(*Material), Created);
}

TEST(FMaterialGraphOperationsTests, CommandAdmissionRequiresEditorOrExplicitUnrecordedMode)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "AdmissionMaterial"));
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	auto First = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
	const auto Original = CaptureExpressions(*Material);
	EXPECT_FALSE(AuthoringGraph->CreateExpression(*First));
	EXPECT_EQ(CaptureExpressions(*Material), Original);
	ASSERT_TRUE(AuthoringGraph->CreateExpression(*First, 0, 0, false));
	Tests::FTestEditorTransactorOwner History;
	auto Second = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
	const auto BeforeRecorded = CaptureExpressions(*Material);
	ASSERT_TRUE(AuthoringGraph->CreateExpression(*Second));
	ASSERT_EQ(History->GetQueueLength(), 1u);
	ASSERT_TRUE(History->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), BeforeRecorded);
	ASSERT_TRUE(History->Redo());
	const auto BeforeJoined = CaptureExpressions(*Material);
	{
		Durin::FScopedTransaction Outer("Outer graph edit");
		auto Third = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
		ASSERT_TRUE(AuthoringGraph->CreateExpression(*Third, 0, 0, false));
	}
	ASSERT_EQ(History->GetQueueLength(), 2u);
	EXPECT_EQ(History->GetUndoContext().Description, "Outer graph edit");
	ASSERT_TRUE(History->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), BeforeJoined);
}

TEST(FMaterialGraphOperationsTests, CancelingRecordingOnStartLeavesThePreparedGraphEditUnpublished)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "CanceledGraphEdit"));
	TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
	auto Expression = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
	const auto Before = CaptureExpressions(*Material);
	Tests::FTestEditorTransactorOwner History;
	const auto Listener = History->OnTransactionStateChanged().AddLambda(
		[&](const FTransactionContext&, ETransactionStateEventType Event) {
			if (Event == ETransactionStateEventType::TransactionStarted) History->Cancel();
		});
	const auto Canceled = Graph->CreateExpression(*Expression);
	History->OnTransactionStateChanged().Remove(Listener);
	EXPECT_FALSE(Canceled);
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_FALSE(History->IsActive());
	EXPECT_EQ(History->GetQueueLength(), 0u);
	ASSERT_TRUE(Graph->CreateExpression(*Expression));
	ASSERT_TRUE(History->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
}

TEST(FMaterialGraphOperationsTests, MoveNodesPublishesWhenOneExpressionCannotBeRecorded)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "PartiallyRecordedMove"));
	TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
	for (int X : {0, 128})
	{
		auto Constant = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
		ASSERT_TRUE(Graph->CreateExpression(*Constant, X, 0, false));
	}
	const auto& Expressions = Material->GetExpressionCollection().Expressions;
	ASSERT_EQ(Expressions.size(), 2u);
	auto* First = Expressions[0].Get();
	auto* Second = Expressions[1].Get();
	const auto BeforeFirst = Graph->FindNode(First->Id)->GetView().Presentation;
	const auto BeforeSecond = Graph->FindNode(Second->Id)->GetView().Presentation;
	const std::array Moves{
		FMaterialGraphNodePresentation{First->Id, BeforeFirst.X + 32, BeforeFirst.Y},
		FMaterialGraphNodePresentation{Second->Id, BeforeSecond.X + 32, BeforeSecond.Y}};

	Tests::FTestEditorTransactorOwner History;
	// A default subobject of an instance is ineligible for transaction capture.
	First->SetFlags(EObjectFlags::DefaultSubobject);
	const bool bMoved = Graph->MoveNodes(Moves);
	First->ClearFlags(EObjectFlags::DefaultSubobject);
	ASSERT_TRUE(bMoved);
	EXPECT_EQ(Graph->FindNode(First->Id)->GetView().Presentation.X, Moves[0].X);
	EXPECT_EQ(Graph->FindNode(Second->Id)->GetView().Presentation.X, Moves[1].X);
	ASSERT_EQ(History->GetQueueLength(), 1u);
	EXPECT_EQ(History->GetTransaction(0)->GetRecordCount(), 1u);
	ASSERT_TRUE(History->Undo());
	EXPECT_EQ(First->EditorLayout.X, Moves[0].X);
	EXPECT_EQ(Second->EditorLayout.X, BeforeSecond.X);
}


TEST(FMaterialGraphOperationsTests, TextureSamplingModeUsesSharedUndoAndRetainsInactivePins)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	Durin::Tests::FTestEditorTransactorOwner Transactions;

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	auto Sample = Testing::MakeGraphExpression<DMaterialExpressionTextureSampleParameter2D>();
	Sample->Metadata = {.Id = FGuid::NewGuid(), .Name = "Sample"};
	Sample->MipValue.SetConstant({2.f});
	Sample->DDX.SetConstant({.1f,.2f});
	ASSERT_TRUE(AuthoringGraph->CreateExpression(*Sample));
	const auto Before = CaptureExpressions(*Material);
	auto* ReadMode = Sample->GetTextureSampleReadMode();
	ASSERT_NE(ReadMode, nullptr);
	*ReadMode = EMaterialTextureReadMode::Grad;
	ASSERT_TRUE(AuthoringGraph->ReplaceExpression(*Sample));
	const auto After = CaptureExpressions(*Material);
	const auto View = Testing::CaptureGraphView(*AuthoringGraph);
	const auto* Node = FindViewNode(View, Sample->Id);
	ASSERT_NE(Node, nullptr);
	ASSERT_EQ(Node->Inputs.size(), 4u);
	EXPECT_FALSE(Node->Inputs[1].bActive);
	EXPECT_TRUE(Node->Inputs[2].bActive);
	EXPECT_TRUE(Node->Inputs[3].bActive);
	EXPECT_FLOAT_EQ(Node->Inputs[1].RetainedConstant.Literal.X, 2.f);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(CaptureExpressions(*Material), After);
	FMaterialGraphClipboardPayload Clipboard;
	ASSERT_TRUE(AuthoringGraph->CopySelection(std::array{Sample->Id}, Clipboard));
	ASSERT_TRUE(AuthoringGraph->Paste(Clipboard, 200, 200));
	const auto& Expressions = Material->GetExpressionCollection().Expressions;
	EXPECT_EQ(std::ranges::count_if(Expressions, [](const auto& E) {
		const auto* S = Cast<DMaterialExpressionTextureSampleParameter2D>(E.Get());
		return S && S->ReadMode == EMaterialTextureReadMode::Grad && S->MipValue.Constant == std::vector<float>{2.f};
	}), 2);
}
