#include "MaterialGraphDocument.h"
#include "MaterialGraphTestSupport.h"
#include "Graph/MaterialExpressionParameters.h"
#include "Graph/MaterialGraphEditInternals.h"

TEST(FMaterialGraphOperationsTests, SamplingOutputsRejectRetiredSelectorsWithoutMutatingGraph)
{
	InitializeDObjectSystem();
	for (const auto Opcode : {EMaterialProgramOpcode::TextureSample2D, EMaterialProgramOpcode::TextureSampleParameter2D})
	{
		TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		FMaterialGraphDocument Document(*Material);
		FMaterialExpressionInput Resource;
		if (Opcode == EMaterialProgramOpcode::TextureSample2D)
		{
			const auto Texture = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::TextureParameter, EMaterialProgramValueType::Texture2D);
			ASSERT_TRUE(Texture) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Texture);
			Resource = {Texture.GeneratedNodeIds.front()};
		}
		const auto Sample = Testing::CreateGraphCatalogNode(Document, Opcode, EMaterialProgramValueType::Float4, Resource);
		const auto Surface = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::MakeSurface, EMaterialProgramValueType::Surface);
		ASSERT_TRUE(Sample) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Sample);
		ASSERT_TRUE(Surface) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Surface);
		const auto Id = Sample.GeneratedNodeIds.front();
		const auto Target = Surface.GeneratedNodeIds.front();
		const auto View = Document.Inspect();
		const auto* Node = FindViewNode(View, Id);
		ASSERT_NE(Node, nullptr);
		std::vector<uint8> Indices;
		for (const auto& Output : Node->Outputs) Indices.push_back(Output.OutputIndex);
		std::vector<uint8> Expected{1, 2, 3, 4, 5, 0};
		if (Opcode == EMaterialProgramOpcode::TextureSampleParameter2D) Expected.push_back(7);
		EXPECT_EQ(Indices, Expected);
		for (const uint8 Index : Expected)
		{
			ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::Input(Target, 0), FMaterialGraphPinAddress::Output({Id, Index}), true));
			const auto Connected = Document.Inspect();
			EXPECT_EQ(FindViewNode(Connected, Target)->Inputs[0].SourceType,
				FindMaterialSampleOutput(Opcode, Index)->Type);
		}
		const auto Before = CaptureExpressions(*Material);
		const auto Revision = Material->GetMaterialCompileStatus().AuthoredRevision;
		for (const uint8 Index : {6, 8, 255})
		{
			EXPECT_EQ(Document.Connect(FMaterialGraphPinAddress::Input(Target, 0), FMaterialGraphPinAddress::Output({Id, Index}), true).GetStatus(), EMaterialGraphCommandStatus::Rejected);
			EXPECT_EQ(Document.Connect(FMaterialGraphPinAddress::MaterialOutput(Material->GetOutputNode()->Id, EMaterialSurfaceOutput::BaseColor), FMaterialGraphPinAddress::Output({Id, Index}), true).GetStatus(),
				EMaterialGraphCommandStatus::Rejected);
		}
		if (Opcode == EMaterialProgramOpcode::TextureSample2D)
			EXPECT_EQ(Document.Connect(FMaterialGraphPinAddress::Input(Target, 0), FMaterialGraphPinAddress::Output({Id, 7}), true).GetStatus(), EMaterialGraphCommandStatus::Rejected);
		EXPECT_EQ(CaptureExpressions(*Material), Before);
		EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision, Revision);
	}
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
	auto Parameter = Testing::MakeGraphExpression<DMaterialExpressionVector4Parameter>();
	Parameter->Metadata.Id = Definition.Id;
	Parameter->Metadata.Name = Definition.Name;
	Parameter->DefaultValue = Definition.Value.GetVector4();
	FMaterialGraphDocument Document(*Material);
	const auto Original = CaptureExpressions(*Material);
	const uint64 Revision = Material->GetMaterialCompileStatus().AuthoredRevision;
	ASSERT_TRUE(Document.CreateExpression(*Parameter, 0, 0, Transactions.Get()));
	const auto Program = CaptureExpressions(*Material);
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision, Revision + 1);
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
	const auto NoChange = Document.ReplaceExpression(*Parameter, Transactions.Get());
	EXPECT_EQ(NoChange.GetStatus(), EMaterialGraphCommandStatus::NoChange);
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, PresentationWritesReportChangesAndPreserveSemanticState)
{
	InitializeDObjectSystem();
	DMaterial* Material = MakeExpandedGraphMaterial("PresentationResults");
	ASSERT_NE(Material, nullptr);
	const auto ProgramRevision = Material->GetMaterialProgramRevision();
	const auto AuthoredRevision = Material->GetMaterialCompileStatus().AuthoredRevision;
	const auto Definitions = Material->GetParameterDefinitions();
	const std::vector<FMaterialParameterDefinition> BeforeDefinitions(Definitions.begin(), Definitions.end());
	int Notifications = 0;
	const auto Observer = Material->GetGraphChanges().Subscribe(*Material,
		[&](const FMaterialGraphChangeSet&) { ++Notifications; });

	FMaterialGraphPresentation Presentation = Material->GetMaterialGraphPresentation();
	auto& Output = Testing::OutputPosition(*Material, Presentation);
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
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision, AuthoredRevision);
	EXPECT_TRUE(std::ranges::equal(Material->GetParameterDefinitions(), BeforeDefinitions));
	Material->GetGraphChanges().Unsubscribe(Observer);
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
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
	EXPECT_TRUE(Material->GetParameterDefinitions().empty());
	EXPECT_TRUE(Material->GetOutputNode() != nullptr);
	EXPECT_EQ(Material->GetExpressionOutputs().BaseColorDefault,
		(Durin::FVector3{0.5f, 0.5f, 0.5f}));

	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialGraphOperationsTests,
	MakeSurfaceCatalogAndAggregateCommandsAreAtomic)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "AggregateSurfaceCommands");
	ASSERT_NE(Material, nullptr);
	// This exercises authored graph commands and normalization, not shader publication.
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto Catalog = FMaterialGraphOperations::EnumerateCatalog();
	const auto Entry = std::ranges::find(Catalog,
		EMaterialProgramOpcode::MakeSurface,
		[](const FMaterialGraphCatalogEntry& Value) {
			return Value.Opcode;
		});
	ASSERT_NE(Entry, Catalog.end());
	EXPECT_EQ(Entry->ResultType, EMaterialProgramValueType::Surface);
	EXPECT_TRUE(std::ranges::none_of(Catalog,
		[](const FMaterialGraphCatalogEntry& Value) {
			return Value.Opcode
				== static_cast<EMaterialProgramOpcode>(30)
				|| Value.Opcode
					== static_cast<EMaterialProgramOpcode>(3);
		}));
	auto AggregateGraph = Testing::MakePBRMaterialExpressionsForTest();
	auto Surface = Testing::MakeGraphExpression<DMaterialExpressionMakeSurface>();
	const auto& Outputs = AggregateGraph.Outputs;
	Surface->BaseColor = Outputs.BaseColor; Surface->Normal = Outputs.Normal;
	Surface->Metallic = Outputs.Metallic; Surface->Roughness = Outputs.Roughness;
	Surface->AmbientOcclusion = Outputs.AmbientOcclusion; Surface->Emissive = Outputs.Emissive;
	Surface->Opacity = Outputs.Opacity; Surface->OpacityMask = Outputs.OpacityMask;
	AggregateGraph.Expressions.emplace_back(Surface.Get());
	AggregateGraph.Outputs = {.Surface = {Surface->Id}, .bUseMaterialAttributes = true};
	ASSERT_TRUE(AggregateGraph.Apply(*Material));
	const FGuid SurfaceId = Material->GetExpressionOutputs().Surface.ExpressionId;
	ASSERT_TRUE(SurfaceId.IsValid());
	FMaterialGraphPresentation AggregatePresentation;
	AggregatePresentation.Nodes.push_back({SurfaceId, 100, 100});
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(AggregatePresentation) != Durin::EMaterialGraphPresentationResult::Rejected);
	ASSERT_TRUE(FMaterialGraphDocument(*Material).Disconnect(FMaterialGraphPinAddress::MaterialOutput((*Material).GetOutputNode()->Id)));
	ASSERT_TRUE(FMaterialGraphDocument(*Material).Connect(FMaterialGraphPinAddress::MaterialOutput((*Material).GetOutputNode()->Id), FMaterialGraphPinAddress::Output({SurfaceId}), true));
	EXPECT_EQ(Material->GetExpressionOutputs().Surface.ExpressionId,
		SurfaceId);
	EXPECT_FALSE(Material->GetExpressionOutputs().BaseColor.ExpressionId.IsValid());
	const auto Normalized = Normalize(*Material);
	ASSERT_TRUE(Normalized);
	EXPECT_FALSE(Normalized.IR.SurfaceRoot.bAggregate);
	// Both authoring modes normalize to final per-property output roots.
	EXPECT_TRUE(Normalized.IR.SurfaceRoot.Inputs[0].bExpression);
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(FMaterialGraphDocument(*Material).CopySelection(std::array{SurfaceId}, Payload));
	EXPECT_TRUE(Payload.bConnectAggregateSurface);
	ASSERT_TRUE(FMaterialGraphDocument(*Material).Paste(Payload, 300, 100));
	EXPECT_NE(Material->GetExpressionOutputs().Surface.ExpressionId,
		SurfaceId);
	ASSERT_TRUE(FMaterialGraphDocument(*Material).Disconnect(FMaterialGraphPinAddress::MaterialOutput((*Material).GetOutputNode()->Id)));
	EXPECT_FALSE(Material->GetExpressionOutputs().Surface.ExpressionId.IsValid());
	MarkAsGarbage(Material);
	CollectGarbage();
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
		{.Nodes = {{NodeId, 100, -200}, {Material->GetOutputNode()->Id, 420, -30}}}) != Durin::EMaterialGraphPresentationResult::Rejected);
	DMaterial* Duplicate = Cast<DMaterial>(DuplicateObject(
		Material, nullptr, "PresentationDuplicate").Object);
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

	ASSERT_TRUE(FMaterialGraphDocument(*Material).MoveMaterialOutput(520, -80, Transactions.Get()));
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision, Revision);
	EXPECT_TRUE(Material->GetOutputNode() != nullptr);
	EXPECT_EQ(Testing::OutputPosition(*Material, Material->GetMaterialGraphPresentation()).X, 520);
	EXPECT_EQ(Testing::OutputPosition(*Material, Material->GetMaterialGraphPresentation()).Y, -80);
	FMaterialGraphPresentation UnrelatedPresentation =
		Material->GetMaterialGraphPresentation();
	ASSERT_FALSE(UnrelatedPresentation.Nodes.empty());
	auto Unrelated = std::ranges::find_if(UnrelatedPresentation.Nodes, [&](const auto& P) { return P.NodeId != Material->GetOutputNode()->Id; });
	ASSERT_NE(Unrelated, UnrelatedPresentation.Nodes.end());
	Unrelated->X += 37;
	const FMaterialGraphNodePresentation UnrelatedPosition = *Unrelated;
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(UnrelatedPresentation) != Durin::EMaterialGraphPresentationResult::Rejected);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Testing::OutputPosition(*Material, Material->GetMaterialGraphPresentation()).X,
		Testing::OutputPosition(*Material, OriginalPresentation).X);
	const auto PreservedNode = std::ranges::find(
		Material->GetMaterialGraphPresentation().Nodes,
		UnrelatedPosition.NodeId, &FMaterialGraphNodePresentation::NodeId);
	ASSERT_NE(PreservedNode, Material->GetMaterialGraphPresentation().Nodes.end());
	EXPECT_EQ(*PreservedNode, UnrelatedPosition);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Testing::OutputPosition(*Material, Material->GetMaterialGraphPresentation()).X, 520);
	EXPECT_TRUE(Transactions->Reset());
	FMaterialGraphMoveSession Move;
	ASSERT_TRUE(Move.Begin(*Material, std::array{Material->GetOutputNode()->Id}, Transactions.Get()));
	ASSERT_TRUE(Move.Apply(std::array{FMaterialGraphNodePresentation{Material->GetOutputNode()->Id, 600, 40}}));
	ASSERT_TRUE(Move.Cancel());
	EXPECT_EQ(Testing::OutputPosition(*Material, Material->GetMaterialGraphPresentation()).X, 520);
	EXPECT_FALSE(Transactions->CanUndo());
	ASSERT_TRUE(Move.Begin(*Material, std::array{Material->GetOutputNode()->Id}, Transactions.Get()));
	ASSERT_TRUE(Move.Apply(std::array{FMaterialGraphNodePresentation{Material->GetOutputNode()->Id, 600, 40}}));
	ASSERT_TRUE(Move.Commit());
	EXPECT_EQ(Testing::OutputPosition(*Material, Material->GetMaterialGraphPresentation()).X, 600);
	EXPECT_TRUE(Transactions->CanUndo());
	EXPECT_TRUE(Transactions->Reset());
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, CatalogAndSearchCoverTheClosedOpcodeDomain)
{
	InitializeDObjectSystem();
	const std::vector<FMaterialGraphCatalogEntry> Catalog =
		FMaterialGraphOperations::EnumerateCatalog();
	EXPECT_FALSE(Catalog.empty());
	for (uint8 Value = static_cast<uint8>(EMaterialProgramOpcode::Constant);
		Value <= static_cast<uint8>(EMaterialProgramOpcode::BlendNormalsRNM);
		++Value)
	{
		if (Value == static_cast<uint8>(EMaterialProgramOpcode::DecodeNormalRG))
		{
			EXPECT_FALSE(std::ranges::any_of(Catalog, [](const auto& Entry) {
				return Entry.Opcode == EMaterialProgramOpcode::DecodeNormalRG;
			}));
			EXPECT_TRUE(GetMaterialProgramNodeSignature(EMaterialProgramOpcode::DecodeNormalRG,
				EMaterialProgramValueType::Float3));
			continue;
		}
		if (Value == 3 || Value == 30 || (Value >= 25 && Value <= 27))
		{
			EXPECT_FALSE(std::ranges::any_of(Catalog, [Value](const auto& Entry) {
				return static_cast<uint8>(Entry.Opcode) == Value;
			}));
			for (uint8 Type = 0; Type <= static_cast<uint8>(EMaterialProgramValueType::Surface); ++Type)
				EXPECT_FALSE(GetMaterialProgramNodeSignature(static_cast<EMaterialProgramOpcode>(Value),
					static_cast<EMaterialProgramValueType>(Type)));
			continue;
		}
		EXPECT_TRUE(std::ranges::any_of(Catalog,
			[Value](const FMaterialGraphCatalogEntry& Entry) {
				return static_cast<uint8>(Entry.Opcode) == Value;
			})) << "Missing opcode " << static_cast<uint32>(Value);
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
}

TEST(FMaterialGraphOperationsTests, ParameterAndChannelPaletteGroupsWidths)
{
	InitializeDObjectSystem();
	using Op = EMaterialProgramOpcode;
	using Type = EMaterialProgramValueType;
	const auto Rows = FMaterialGraphOperations::SearchCatalog("");
	EXPECT_EQ(std::ranges::count(Rows, Op::Parameter, &FMaterialGraphCatalogEntry::Opcode), 2);
	for (const auto Opcode : {Op::AppendVector, Op::Swizzle})
		EXPECT_EQ(std::ranges::count(Rows, Opcode, &FMaterialGraphCatalogEntry::Opcode), 1);
	for (const auto Opcode : {Op::MakeFloat2, Op::MakeFloat3, Op::MakeFloat4, Op::Splat2, Op::Splat3, Op::Splat4})
		EXPECT_EQ(std::ranges::count(Rows, Opcode, &FMaterialGraphCatalogEntry::Opcode), 0);
	const auto Mask = FMaterialGraphOperations::SearchCatalog("component mask", Type::Float4);
	ASSERT_EQ(Mask.size(), 1u);
	EXPECT_EQ(Mask.front().Opcode, Op::Swizzle);
	EXPECT_EQ(FMaterialGraphOperations::SearchCatalog("truncate", Type::Float3).size(), 1u);
	EXPECT_TRUE(FMaterialGraphOperations::SearchCatalog("channels", Type::Texture2D).empty());
}

TEST(FMaterialGraphOperationsTests, AppendInfersWidthsAndKeepsUndoableOverflowDiagnostics)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, NAME_None);
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	Durin::Tests::FTestTransactorOwner Transactions;
	using Op = EMaterialProgramOpcode;
	using Type = EMaterialProgramValueType;
	const auto Vector = Testing::CreateGraphCatalogNode(Document, Op::Parameter, Type::Float4);
	ASSERT_TRUE(Vector);
	const auto Mask = Testing::CreateGraphCatalogNode(Document, Op::Swizzle, Type::Float2, {Vector.GeneratedNodeIds.front()});
	ASSERT_TRUE(Mask);
	const auto MaskId = Mask.GeneratedNodeIds.front();
	const auto Append = Testing::CreateGraphCatalogNode(Document, Op::AppendVector, Type::Float2, {MaskId});
	ASSERT_TRUE(Append) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Append);
	const auto Id = Append.GeneratedNodeIds.front();
	EXPECT_EQ(FindViewNode(Document.Inspect(), Id)->Node.ResultType, Type::Float3);
	const auto Before = CaptureExpressions(*Material);
	ASSERT_TRUE(Document.SetSwizzleComponents(MaskId, std::array<uint8, 3>{0, 1, 2}, Transactions.Get()));
	EXPECT_EQ(FindViewNode(Document.Inspect(), Id)->Node.ResultType, Type::Float4);
	const auto After = CaptureExpressions(*Material);
	ASSERT_TRUE(Document.SetSwizzleComponents(MaskId, std::array<uint8, 4>{0, 1, 2, 3}, Transactions.Get()));
	EXPECT_TRUE(HasGraphDiagnostics(*Material));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), After);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(CaptureExpressions(*Material), After);
}

TEST(FMaterialGraphOperationsTests, SwizzleWidthChangesRepeatChannelsAndUndo)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, NAME_None);
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	Durin::Tests::FTestTransactorOwner Transactions;
	const auto Source = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float);
	ASSERT_TRUE(Source);
	const auto Swizzle = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::Swizzle,
		EMaterialProgramValueType::Float, {Source.GeneratedNodeIds.front()});
	ASSERT_TRUE(Swizzle);
	const auto Id = Swizzle.GeneratedNodeIds.front();
	const auto Before = CaptureExpressions(*Material);
	ASSERT_TRUE(Document.SetSwizzleComponents(Id, std::array<uint8, 4>{0, 0, 0, 0}, Transactions.Get()));
	EXPECT_EQ(FindViewNode(Document.Inspect(), Id)->Node.ResultType, EMaterialProgramValueType::Float4);
	const auto After = CaptureExpressions(*Material);
	ASSERT_TRUE(Document.SetSwizzleComponents(Id, std::array<uint8, 2>{0, 1}, Transactions.Get()));
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
	const auto Catalog = FMaterialGraphOperations::EnumerateCatalog();
	for (const auto Source : {std::optional<Type>{}, std::optional{Type::Float}, std::optional{Type::Float2},
		std::optional{Type::Float3}, std::optional{Type::Float4}, std::optional{Type::Texture2D}})
	{
		const auto Rows = FMaterialGraphOperations::SearchCatalog(Catalog, {}, Source);
		for (const auto& Entry : Catalog)
		{
			if (!IsMaterialAdaptiveNumeric(Entry.Opcode)) continue;
			const bool bAllowed = Source != Type::Texture2D && !(Source == Type::Float && Entry.Opcode == EMaterialProgramOpcode::Normalize);
			EXPECT_EQ(std::ranges::count(Rows, Entry.Opcode, &FMaterialGraphCatalogEntry::Opcode), bAllowed ? 1 : 0);
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
		FMaterialGraphDocument Document(*Material);
		Durin::Tests::FTestTransactorOwner Transactions;
		const auto Catalog = FMaterialGraphOperations::EnumerateCatalog();
		const auto Create = [&](EMaterialProgramOpcode Opcode, Type T) {
			const auto Entry = std::ranges::find_if(Catalog, [&](const auto& E) { return E.Opcode == Opcode && E.ResultType == T; });
			return Document.CreateCatalogNode(*Entry);
		};
		const auto Vector = Create(EMaterialProgramOpcode::Constant, Width);
		const auto Scalar = Create(EMaterialProgramOpcode::Constant, Type::Float);
		const auto Other = Create(EMaterialProgramOpcode::Constant, Width == Type::Float2 ? Type::Float3 : Type::Float2);
		const auto Multiply = Create(EMaterialProgramOpcode::Multiply, Type::Float);
		const auto Add = Create(EMaterialProgramOpcode::Add, Type::Float);
		ASSERT_TRUE(Vector); ASSERT_TRUE(Scalar); ASSERT_TRUE(Other); ASSERT_TRUE(Multiply); ASSERT_TRUE(Add);
		const auto M = Multiply.GeneratedNodeIds.front(), A = Add.GeneratedNodeIds.front();
		ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::Input(A, 0), FMaterialGraphPinAddress::Output({M})));
		ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::Input(M, 1), FMaterialGraphPinAddress::Output({Scalar.GeneratedNodeIds.front()})));
		const auto Before = CaptureExpressions(*Material);
		ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::Input(M, 0), FMaterialGraphPinAddress::Output({Vector.GeneratedNodeIds.front()}), false, Transactions.Get()));
		auto View = Document.Inspect();
		EXPECT_EQ(FindViewNode(View, M)->Node.ResultType, Width);
		EXPECT_EQ(FindViewNode(View, A)->Node.ResultType, Width);
		std::vector<DMaterialExpression*> Expressions;
		for (const auto& E : Material->GetExpressionCollection().Expressions) Expressions.push_back(E.Get());
		const std::array Roots{FMaterialExpressionInput{A}};
		const auto Built = MIR::BuildGraph(Expressions, Roots);
		ASSERT_TRUE(Built);
		EXPECT_EQ(Built.IR.Nodes[Built.Roots.front()].ResultType, Width);
		EXPECT_TRUE(std::ranges::any_of(Built.IR.Nodes, [&](const auto& N) {
			return N.ResultType == Width && N.Opcode >= EMaterialProgramOpcode::Splat2 && N.Opcode <= EMaterialProgramOpcode::Splat4;
		}));
		const auto After = CaptureExpressions(*Material);
		ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::Input(M, 1), FMaterialGraphPinAddress::Output({Other.GeneratedNodeIds.front()}), true, Transactions.Get()));
		EXPECT_TRUE(HasGraphDiagnostics(*Material));
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(CaptureExpressions(*Material), After);
		ASSERT_TRUE(Transactions->Undo()); EXPECT_EQ(CaptureExpressions(*Material), Before);
		ASSERT_TRUE(Transactions->Redo()); EXPECT_EQ(CaptureExpressions(*Material), After);
		ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::Input(M, 0), FMaterialGraphPinAddress::Output({Scalar.GeneratedNodeIds.front()}), true));
		View = Document.Inspect();
		EXPECT_EQ(FindViewNode(View, M)->Node.ResultType, Type::Float);
		EXPECT_EQ(FindViewNode(View, A)->Node.ResultType, Type::Float);
		const auto MakeVector = Create(EMaterialProgramOpcode::MakeFloat2, Type::Float2);
		ASSERT_TRUE(MakeVector);
		ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::Input(MakeVector.GeneratedNodeIds.front(), 0), FMaterialGraphPinAddress::Output({A})));
		const auto Fixed = CaptureExpressions(*Material);
		ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::Input(M, 0), FMaterialGraphPinAddress::Output({Vector.GeneratedNodeIds.front()}), true, Transactions.Get()));
		EXPECT_TRUE(HasGraphDiagnostics(*Material));
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(CaptureExpressions(*Material), Fixed);
	}
}

TEST(FMaterialGraphOperationsTests, FixedInputsBroadcastScalarsAndUndoWithoutAuthoredConversionNodes)
{
	InitializeDObjectSystem();
	using Type = EMaterialProgramValueType;
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	Durin::Tests::FTestTransactorOwner Transactions;
	const auto Catalog = FMaterialGraphOperations::EnumerateCatalog();
	const auto Create = [&](EMaterialProgramOpcode Opcode, Type T) {
		const auto Entry = std::ranges::find_if(Catalog, [&](const auto& E) { return E.Opcode == Opcode && E.ResultType == T; });
		return Document.CreateCatalogNode(*Entry);
	};
	const auto Scalar = Create(EMaterialProgramOpcode::Constant, Type::Float);
	const auto Vector = Create(EMaterialProgramOpcode::Constant, Type::Float2);
	const auto Surface = Create(EMaterialProgramOpcode::MakeSurface, Type::Surface);
	ASSERT_TRUE(Scalar); ASSERT_TRUE(Vector); ASSERT_TRUE(Surface);
	const auto S = Scalar.GeneratedNodeIds.front(), V = Vector.GeneratedNodeIds.front(), M = Surface.GeneratedNodeIds.front();
	const auto Before = CaptureExpressions(*Material);
	ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::Input(M, 0), FMaterialGraphPinAddress::Output({S}), false, Transactions.Get()));
	const auto After = CaptureExpressions(*Material);
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 4u);
	const auto View = Document.Inspect();
	const auto& Types = FindViewNode(View, M)->Inputs[0].AcceptedTypes;
	EXPECT_NE(std::ranges::find(Types, Type::Float), Types.end());
	ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::Input(M, 0), FMaterialGraphPinAddress::Output({V}), true, Transactions.Get()));
	EXPECT_TRUE(HasGraphDiagnostics(*Material));
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::Input(M, 2), FMaterialGraphPinAddress::Output({V}), false, Transactions.Get()));
	EXPECT_TRUE(HasGraphDiagnostics(*Material));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), After);
	ASSERT_TRUE(Transactions->Undo()); EXPECT_EQ(CaptureExpressions(*Material), Before);
	ASSERT_TRUE(Transactions->Redo()); EXPECT_EQ(CaptureExpressions(*Material), After);
	ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::MaterialOutput(Material->GetOutputNode()->Id, EMaterialSurfaceOutput::BaseColor), FMaterialGraphPinAddress::Output({S}), true, Transactions.Get()));
	EXPECT_EQ(Material->GetExpressionOutputs().BaseColor.ExpressionId, S);
	std::vector<DMaterialExpression*> Expressions;
	for (const auto& E : Material->GetExpressionCollection().Expressions) Expressions.push_back(E.Get());
	const auto Built = MIR::FGraphBuilder(Expressions).FinishSurface(Material->GetExpressionOutputs());
	ASSERT_TRUE(Built);
	EXPECT_EQ(Built.IR.Nodes[Built.IR.SurfaceRoot.Inputs[0].ExpressionIndex].Opcode, EMaterialProgramOpcode::Splat3);
	ASSERT_TRUE(Transactions->Undo()); EXPECT_FALSE(Material->GetExpressionOutputs().BaseColor.ExpressionId.IsValid());
	ASSERT_TRUE(Transactions->Redo()); EXPECT_EQ(Material->GetExpressionOutputs().BaseColor.ExpressionId, S);
}

TEST(FMaterialGraphOperationsTests, FunctionMathAdaptsAndReportsNormalizeAndLerpDiagnostics)
{
	InitializeDObjectSystem();
	using Type = EMaterialProgramValueType;
	auto* Function = NewObject<DMaterialFunction>(nullptr, NAME_None);
	FMaterialGraphDocument Document(*Function);
	const auto Catalog = FMaterialGraphOperations::EnumerateCatalog();
	const auto Create = [&](EMaterialProgramOpcode Opcode, Type T, FMaterialExpressionInput Source = {}) {
		const auto Entry = std::ranges::find_if(Catalog, [&](const auto& E) { return E.Opcode == Opcode && E.ResultType == T; });
		return Document.CreateCatalogNode(*Entry, 0, 0, Source);
	};
	const auto Vector = Create(EMaterialProgramOpcode::Constant, Type::Float3);
	const auto Scalar = Create(EMaterialProgramOpcode::Constant, Type::Float);
	ASSERT_TRUE(Vector); ASSERT_TRUE(Scalar);
	const auto V = Vector.GeneratedNodeIds.front(), S = Scalar.GeneratedNodeIds.front();
	const auto Normalize = Create(EMaterialProgramOpcode::Normalize, Type::Float2, {V});
	ASSERT_TRUE(Normalize) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Normalize);
	const auto N = Normalize.GeneratedNodeIds.front();
	const auto Lerp = Create(EMaterialProgramOpcode::Lerp, Type::Float, {N});
	ASSERT_TRUE(Lerp) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Lerp);
	const auto L = Lerp.GeneratedNodeIds.front();
	const auto View = Document.Inspect();
	EXPECT_EQ(FindViewNode(View, N)->Node.ResultType, Type::Float3);
	EXPECT_EQ(FindViewNode(View, L)->Node.ResultType, Type::Float3);
	ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::Input(N, 0), FMaterialGraphPinAddress::Output({S}), true));
	ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::Input(L, 2), FMaterialGraphPinAddress::Output({V})));
	EXPECT_TRUE(HasGraphDiagnostics(*Function));
	ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::Input(L, 2), FMaterialGraphPinAddress::Output({S}), true));
	ASSERT_TRUE(Document.Disconnect(FMaterialGraphPinAddress::Input(N, 0)));
	ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::Input(N, 0), FMaterialGraphPinAddress::Output({V})));
}

TEST(FMaterialGraphOperationsTests, EveryCatalogShapeCreatesItsConcreteExpressionWithValidDefaults)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "TypedCatalogCreation");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	const auto Catalog = FMaterialGraphOperations::EnumerateCatalog();
	const auto Surface = std::ranges::find(Catalog, EMaterialProgramOpcode::MakeSurface, &FMaterialGraphCatalogEntry::Opcode);
	const auto Texture = std::ranges::find(Catalog, EMaterialProgramOpcode::TextureParameter, &FMaterialGraphCatalogEntry::Opcode);
	ASSERT_NE(Surface, Catalog.end()); ASSERT_NE(Texture, Catalog.end());
	for (const auto& Entry : Catalog)
	{
		SCOPED_TRACE(std::format("{} ({})", Entry.OperationName, static_cast<uint8>(Entry.ResultType)));
		ASSERT_TRUE(Material->SetMaterialExpressions({}, {}));
		FMaterialExpressionInput Source;
		if (!Entry.AcceptedInputTypes.empty() && !Entry.AcceptedInputTypes.front().empty())
		{
			const auto Type = Entry.AcceptedInputTypes.front().front();
			if (Type == EMaterialProgramValueType::Surface || Type == EMaterialProgramValueType::Texture2D)
			{
				const auto Prerequisite = Document.CreateCatalogNode(Type == EMaterialProgramValueType::Surface ? *Surface : *Texture);
				ASSERT_TRUE(Prerequisite) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Prerequisite);
				Source = {Prerequisite.GeneratedNodeIds.front()};
			}
		}
		const auto Created = Document.CreateCatalogNode(Entry, 400, 200, Source);
		ASSERT_TRUE(Created) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Created);
		ASSERT_EQ(Created.GeneratedNodeIds.size(), 1u);
		const auto& Expressions = Material->GetExpressionCollection().Expressions;
		const auto It = std::ranges::find(Expressions, Created.GeneratedNodeIds.front(), [](const auto& E) { return E->Id; });
		ASSERT_NE(It, Expressions.end());
		const bool NarrowParameter = Entry.Opcode == EMaterialProgramOpcode::Parameter
			&& (Entry.ResultType == EMaterialProgramValueType::Float2 || Entry.ResultType == EMaterialProgramValueType::Float3);
		EXPECT_EQ((*It)->GetClass(), NarrowParameter ? DMaterialExpressionSwizzle::StaticClass() : Entry.ExpressionClass);
		EXPECT_EQ((*It)->GetOuter(), Material);
		const auto ObjectRevision = GDObjectArray.GetRevision();
		const auto View = Document.Inspect(Catalog);
		EXPECT_EQ(View.Nodes.size(), Material->GetExpressionCollection().Expressions.size());
		for (const FMaterialGraphNodeView& Node : View.Nodes)
		{
			EXPECT_FALSE(Node.PrimaryLabel.empty());
			const auto Expression = std::ranges::find_if(Material->GetExpressionCollection().Expressions,
				[&](const auto& Value) { return Value->Id == Node.Node.Id; });
			ASSERT_NE(Expression, Material->GetExpressionCollection().Expressions.end());
			EXPECT_EQ(Node.Inputs.size(), Node.Node.bMaterialOutput ? 8u : (*Expression)->GetAuthoredInputCount());
			for (const FMaterialGraphPinView& Input : Node.Inputs)
			{
				EXPECT_FALSE(Input.Name.empty());
				EXPECT_FALSE(Input.AcceptedTypes.empty());
			}
		}
		const auto* Viewed = FindViewNode(View, (*It)->Id);
		ASSERT_NE(Viewed, nullptr);
		EXPECT_EQ(Viewed->Node.Opcode, NarrowParameter ? EMaterialProgramOpcode::Swizzle : Entry.Opcode);
		EXPECT_EQ(Viewed->Node.ResultType, Entry.Opcode == EMaterialProgramOpcode::AppendVector ? EMaterialProgramValueType::Float2 : Entry.ResultType);
		EXPECT_EQ(Viewed->Inputs.size(), (*It)->GetAuthoredInputCount());
		EXPECT_EQ(GDObjectArray.GetRevision(), ObjectRevision);
		VisitMaterialExpressionInputs(**It, [&](uint32 Index, FMaterialExpressionInput& Input) {
			ASSERT_LT(Index, Viewed->Inputs.size());
			EXPECT_EQ(Viewed->Inputs[Index].Link, (FMaterialProgramLink{Input.ExpressionId, Input.OutputIndex, Input.OutputId}));
		});
		// Numeric retained defaults must expose exactly the concrete stored components.
		(*It)->GetClass()->ForEachProperty([&](FProperty* Property) {
			if (Property->GetKind() != DurinCodeGen::EPropertyGenFlags::Struct
				|| static_cast<FStructProperty*>(Property)->GetStruct() != FMaterialExpressionInput::StaticStruct()) return;
			auto* Default = (*It)->GetClass()->FindPropertyByName(FName(Property->NamePrivate.ToString() + "Default"));
			if (!Default || Default->GetKind() != DurinCodeGen::EPropertyGenFlags::Array
				|| static_cast<FArrayProperty*>(Default)->GetInner()->GetKind() != DurinCodeGen::EPropertyGenFlags::Float) return;
			VisitMaterialExpressionInputs(**It, [&](uint32 Index, FMaterialExpressionInput& Input) {
				if (&Input != Property->GetValuePtr(It->Get())) return;
				const auto& Values = *static_cast<std::vector<float>*>(Default->GetValuePtr(It->Get()));
				if (Values.empty()) return;
				const auto& Inline = Viewed->Inputs[Index].InlineDefault;
				EXPECT_EQ(Inline.Kind, EMaterialInputDefaultKind::Literal);
				const std::array Components{Inline.Literal.X, Inline.Literal.Y, Inline.Literal.Z, Inline.Literal.W};
				ASSERT_LE(Values.size(), Components.size());
				for (size_t Component = 0; Component < Values.size(); ++Component) EXPECT_EQ(Values[Component], Components[Component]);
			});
		});
		if (const auto* Parameter = Cast<DMaterialExpressionParameter>(It->Get()))
		{
			ASSERT_EQ(Created.AffectedParameterIds.size(), 1u);
			EXPECT_EQ(Parameter->Metadata.Id, Created.AffectedParameterIds.front());
			EXPECT_EQ(Parameter->GetParameterDefinition().Type, Parameter->GetParameterDefinition().Value.GetType());
		}
	}
	auto Invalid = *Surface;
	Invalid.ExpressionClass = DMaterialExpressionScalarConstant::StaticClass();
	const auto Before = Material->GetExpressionCollection().Expressions;
	EXPECT_FALSE(Document.CreateCatalogNode(Invalid));
	EXPECT_EQ(Material->GetExpressionCollection().Expressions, Before);
	MarkAsGarbage(Material); CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, CatalogPinsAgreeWithRuntimeValidation)
{
	InitializeDObjectSystem();
	using Type = EMaterialProgramValueType;
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	const auto Catalog = FMaterialGraphOperations::EnumerateCatalog();
	// Input sources are immutable throughout the matrix. Build each type once;
	// every case still rewrites all target links and runs both validation checks.
	Testing::FTestMaterialExpressionGraph Graph;
	FMaterialParameterDefinition Texture;
	Texture.Id = FGuid::NewGuid(); Texture.Name = "SignatureTexture";
	Texture.Type = EMaterialParameterType::Texture; Texture.Value = FMaterialParameterValue::MakeTexture(nullptr);
	const std::array Definitions{Texture};
	std::function<FMaterialExpressionInput(Type)> AddSource = [&](Type ValueType) {
		if (ValueType == Type::Texture2D)
			return Testing::MakeLink(Graph.Add(EMaterialProgramOpcode::TextureParameter, ValueType, {}, Texture.Id, {}, Definitions));
		std::vector<FMaterialExpressionInput> Inputs;
		if (ValueType == Type::Surface)
			for (uint32 Index = 0; Index < 8; ++Index) Inputs.push_back(AddSource(GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(Index))));
		return Testing::MakeLink(Graph.Add(ValueType == Type::Surface ? EMaterialProgramOpcode::MakeSurface : EMaterialProgramOpcode::Constant,
			ValueType, std::move(Inputs), {}, {}));
	};
	std::array<FMaterialExpressionInput, 6> Sources;
	std::array<std::vector<DMaterialExpression*>, 6> SourceExpressions;
	for (uint32 I = 0; I < Sources.size(); ++I)
	{
		const auto First = Graph.Expressions.size();
		Sources[I] = AddSource(static_cast<Type>(I));
		for (size_t J = First; J < Graph.Expressions.size(); ++J) SourceExpressions[I].push_back(Graph.Expressions[J].Get());
	}
	std::vector<DMaterialExpression*> Expressions;
	for (const auto& Entry : Catalog)
	{
		if (Entry.AcceptedInputTypes.empty()) continue;
		ASSERT_TRUE(Material->SetMaterialExpressions({}, {}));
		FMaterialExpressionInput Prerequisite;
		const auto FirstType = Entry.AcceptedInputTypes.front().front();
		if (FirstType == Type::Surface || FirstType == Type::Texture2D)
		{
			const auto Opcode = FirstType == Type::Surface ? EMaterialProgramOpcode::MakeSurface : EMaterialProgramOpcode::TextureParameter;
			const auto Found = std::ranges::find(Catalog, Opcode, &FMaterialGraphCatalogEntry::Opcode);
			ASSERT_NE(Found, Catalog.end());
			const auto Created = Document.CreateCatalogNode(*Found);
			ASSERT_TRUE(Created); Prerequisite = {Created.GeneratedNodeIds.front()};
		}
		const auto Created = Document.CreateCatalogNode(Entry, 0, 0, Prerequisite);
		ASSERT_TRUE(Created) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Created);
		const auto* Original = FindExpression<DMaterialExpression>(*Material, Created.GeneratedNodeIds.front());
		ASSERT_NE(Original, nullptr);
		ASSERT_EQ(Original->GetAuthoredInputCount(), Entry.AcceptedInputTypes.size());
		TStrongObjectPtr<DMaterialExpression> Target(DuplicateObject(Original, nullptr, NAME_None).Object);
		if (auto* Swizzle = Cast<DMaterialExpressionSwizzle>(Target.Get()))
			std::ranges::fill(Swizzle->Components, 0);
		for (uint32 Pin = 0; Pin < Entry.AcceptedInputTypes.size(); ++Pin)
			for (Type SourceType : {Type::Float, Type::Float2, Type::Float3, Type::Float4, Type::Texture2D, Type::Surface})
			{
				SCOPED_TRACE(std::format("{} result {} pin {} source {}", Entry.OperationName,
					static_cast<uint8>(Entry.ResultType), Pin, static_cast<uint8>(SourceType)));
				Expressions.clear();
				std::array<bool, 6> Included{};
				VisitMaterialExpressionInputs(*Target, [&](uint32 Index, FMaterialExpressionInput& Input) {
					const auto TypeIndex = static_cast<size_t>(Index == Pin ? SourceType : Entry.AcceptedInputTypes[Index].front());
					Input = Sources[TypeIndex];
					if (!Included[TypeIndex])
					{
						Included[TypeIndex] = true;
						Expressions.insert(Expressions.end(), SourceExpressions[TypeIndex].begin(), SourceExpressions[TypeIndex].end());
					}
				});
				Expressions.push_back(Target.Get());
				const auto& Accepted = Entry.AcceptedInputTypes[Pin];
				const bool bAccepted = std::ranges::find(Accepted, SourceType) != Accepted.end();
				EXPECT_EQ(static_cast<bool>(MIR::FGraphBuilder::ValidateSurface(Expressions, {})), bAccepted);
				// Fixed concrete pins cannot disappear; a dangling source is rejected instead.
				VisitMaterialExpressionInputs(*Target, [&](uint32 Index, FMaterialExpressionInput& Input) {
					if (Index == Pin) Input.ExpressionId = FGuid::NewGuid();
				});
				EXPECT_FALSE(MIR::FGraphBuilder::ValidateSurface(Expressions, {}));
			}
	}
}

TEST(FMaterialGraphOperationsTests, SignaturesRejectInvalidResultsAndKeepSwizzlePayloadValidation)
{
	InitializeDObjectSystem();
	using Type = EMaterialProgramValueType;
	auto Source = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
	for (DClass* Class : {DMaterialExpressionAdd::StaticClass(), DMaterialExpressionNegate::StaticClass(), DMaterialExpressionClamp::StaticClass()})
		for (Type ResultType : {Type::Texture2D, Type::Surface})
		{
			TStrongObjectPtr<DMaterialExpression> Target(NewObject<DMaterialExpression>(Class, nullptr, NAME_None));
			Target->Id = FGuid::NewGuid();
			auto* Property = Class->FindPropertyByName("ResultType");
			ASSERT_NE(Property, nullptr);
			*static_cast<Type*>(Property->GetValuePtr(Target.Get())) = ResultType;
			VisitMaterialExpressionInputs(*Target, [&](uint32, FMaterialExpressionInput& Input) { Input = {Source->Id}; });
			EXPECT_FALSE(MIR::FGraphBuilder::ValidateSurface(std::array<DMaterialExpression*, 2>{Source.Get(), Target.Get()}, {}));
		}
	EXPECT_FALSE(GetMaterialProgramNodeSignature(static_cast<EMaterialProgramOpcode>(3), Type::Float));
	EXPECT_FALSE(GetMaterialProgramNodeSignature(static_cast<EMaterialProgramOpcode>(255), Type::Float));
	EXPECT_FALSE(GetMaterialProgramNodeSignature(EMaterialProgramOpcode::Constant, static_cast<Type>(255)));
	EXPECT_FALSE(GetMaterialProgramNodeSignature(EMaterialProgramOpcode::Normalize, Type::Float));
	auto Swizzle = Testing::MakeGraphExpression<DMaterialExpressionSwizzle>();
	Swizzle->Input = {Source->Id}; Swizzle->Components = {0};
	const std::array<DMaterialExpression*, 2> Expressions{Source.Get(), Swizzle.Get()};
	ASSERT_TRUE(MIR::FGraphBuilder::ValidateSurface(Expressions, {}));
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
		FMaterialGraphOperations::SearchCatalog("multiply", EMaterialProgramValueType::Float3);
	const auto Multiply = std::ranges::find_if(Catalog,
		[](const FMaterialGraphCatalogEntry& Entry) {
			return Entry.OperationName == "Multiply"
				&& Entry.ResultType == EMaterialProgramValueType::Float3;
		});
	ASSERT_NE(Multiply, Catalog.end());

	const auto Before = CaptureExpressions(*Material);
	Durin::Tests::FTestTransactorOwner Transactions;
	const FMaterialGraphCommandResult Created =
		FMaterialGraphDocument(*Material).CreateCatalogNode(*Multiply, 400, 200, {}, Transactions.Get());
	ASSERT_TRUE(Created) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Created);
	ASSERT_EQ(Created.GeneratedNodeIds.size(), 1u);
	const FMaterialGraphView View = FMaterialGraphDocument(*Material).Inspect();
	const FMaterialGraphNodeView* Node = FindViewNode(View, Created.GeneratedNodeIds.front());
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
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, CompactInputCommandsPreserveSharingFallbacksAndUndo)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "CompactInputCommands");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	Durin::Tests::FTestTransactorOwner Transactions;
	const auto Created = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::Multiply, EMaterialProgramValueType::Float, {}, 400, 200, Transactions.Get());
	ASSERT_TRUE(Created) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Created);
	const auto Id = Created.GeneratedNodeIds[0];
	const auto Before = CaptureExpressions(*Material);
	const auto Extracted = Document.ExtractInputDefault(Id, 1, {}, Transactions.Get());
	ASSERT_TRUE(Extracted) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Extracted);
	const auto ExtractedId = Extracted.GeneratedNodeIds[0];
	ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::Input(Id, 0), FMaterialGraphPinAddress::Output({ExtractedId}), false, Transactions.Get()));
	ASSERT_TRUE(Document.InlineInputNode(Id, 1, {}, Transactions.Get()));
	EXPECT_TRUE(std::ranges::any_of(Material->GetExpressionCollection().Expressions, [&](const auto& Node) { return Node->Id == ExtractedId; }));
	ASSERT_TRUE(Document.InlineInputNode(Id, 0, {}, Transactions.Get()));
	EXPECT_FALSE(std::ranges::any_of(Material->GetExpressionCollection().Expressions, [&](const auto& Node) { return Node->Id == ExtractedId; }));
	for (int Index = 0; Index < 4; ++Index) ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	for (int Index = 0; Index < 4; ++Index) ASSERT_TRUE(Transactions->Redo());
	const auto* Node = FindExpression<DMaterialExpressionMultiply>(*Material, Id);
	ASSERT_NE(Node, nullptr);
	EXPECT_EQ(Node->ADefault, (std::vector<float>{1}));
	EXPECT_EQ(Node->BDefault, (std::vector<float>{1}));
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, TypedInputDefaultsCoverWidthsCoordinatesAndFunctionBindings)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "TypedInputDefaults");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	Durin::Tests::FTestTransactorOwner Transactions;
	for (uint32 Width = 1; Width <= 4; ++Width)
	{
		auto* Add = NewObject<DMaterialExpressionAdd>(nullptr, NAME_None);
		Add->Id = FGuid::NewGuid();
		Add->ResultType = static_cast<EMaterialProgramValueType>(Width - 1);
		Add->ADefault.assign(Width, 0.f);
		Add->BDefault.assign(Width, 1.f);
		const auto Id = Add->Id;
		ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 1>{Add}, {}));
		ASSERT_TRUE(Material->SetMaterialGraphPresentation({.Nodes = {{Id, 400, 100}}}) != Durin::EMaterialGraphPresentationResult::Rejected);
		const FMaterialInputDefault Value{.Kind = EMaterialInputDefaultKind::Literal,
			.Type = Add->ResultType, .Literal = {2, 3, 4, 5}};
		ASSERT_TRUE(Document.SetInputDefault(Id, 1, Value));
		EXPECT_EQ(Document.SetInputDefault(Id, 1, Value).GetStatus(), EMaterialGraphCommandStatus::NoChange);
		const auto Extracted = Document.ExtractInputDefault(Id, 1);
		ASSERT_TRUE(Extracted) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Extracted);
		ASSERT_EQ(Material->GetExpressionCollection().Expressions.size(), 3u);
		EXPECT_EQ(Material->GetExpressionCollection().Expressions[Material->GetExpressionCollection().Expressions.size() - 2]->GetClass(),
			(std::array{DMaterialExpressionScalarConstant::StaticClass(), DMaterialExpressionVector2Constant::StaticClass(),
				DMaterialExpressionVector3Constant::StaticClass(), DMaterialExpressionVector4Constant::StaticClass()})[Width - 1]);
		ASSERT_TRUE(Document.InlineInputNode(Id, 1));
		ASSERT_EQ(Material->GetExpressionCollection().Expressions.size(), 2u);
		const auto* Result = Cast<DMaterialExpressionAdd>(Material->GetExpressionCollection().Expressions.front().Get());
		ASSERT_NE(Result, nullptr);
		ASSERT_EQ(Result->BDefault.size(), Width);
		for (uint32 Index = 0; Index < Width; ++Index) EXPECT_FLOAT_EQ(Result->BDefault[Index], 2.f + Index);
	}
	auto* Coordinates = NewObject<DMaterialExpressionTextureCoordinates>(nullptr, NAME_None);
	Coordinates->Id = FGuid::NewGuid();
	const auto CoordinatesId = Coordinates->Id;
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 1>{Coordinates}, {}));
	ASSERT_TRUE(Material->SetMaterialGraphPresentation({.Nodes = {{CoordinatesId, 400, 100}}}) != Durin::EMaterialGraphPresentationResult::Rejected);
	ASSERT_TRUE(Document.SetInputDefault(CoordinatesId, 0, {.Kind = EMaterialInputDefaultKind::Literal,
		.Type = EMaterialProgramValueType::Float3, .Literal = {2, 3, 4}}));
	EXPECT_TRUE(HasGraphDiagnostics(*Material));
	ASSERT_TRUE(Document.SetInputDefault(CoordinatesId, 0, {.Kind = EMaterialInputDefaultKind::Literal,
		.Type = EMaterialProgramValueType::Float, .Literal = {2}}, {}, Transactions.Get()));
	ASSERT_TRUE(Document.ExtractInputDefault(CoordinatesId, 0, {}, Transactions.Get()));
	ASSERT_TRUE(Document.InlineInputNode(CoordinatesId, 0, {}, Transactions.Get()));
	EXPECT_EQ(Cast<DMaterialExpressionTextureCoordinates>(Material->GetExpressionCollection().Expressions.front().Get())->ChannelDefault, (std::vector<float>{2}));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 3u);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 2u);

	auto* Function = NewObject<DMaterialFunction>(nullptr, "TypedDefaultFunction");
	FMaterialFunctionSignature Signature;
	const auto InputId = FGuid::NewGuid(), OutputId = FGuid::NewGuid();
	Signature.Inputs.push_back({.Id = InputId, .Type = EMaterialProgramValueType::Float3, .Name = "Value",
		.Default = {.Kind = EMaterialFunctionDefaultKind::Numeric}});
	Signature.Outputs.push_back({.Id = OutputId, .Type = EMaterialProgramValueType::Float3, .Name = "Result"});
	auto* Input = NewObject<DMaterialExpressionFunctionInput>(nullptr, NAME_None);
	Input->Id = FGuid::NewGuid(); Input->Port.Id = InputId;
	auto* Output = NewObject<DMaterialExpressionFunctionOutput>(nullptr, NAME_None);
	Output->Id = FGuid::NewGuid(); Output->Port.Id = OutputId; Output->Source = {Input->Id};
	ASSERT_TRUE(Function->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(Signature, std::array<DMaterialExpression*, 2>{Input, Output})));
	ASSERT_TRUE(Material->SetMaterialExpressions({}, {}));
	const auto Created = Document.InsertFunctionCall(*Function, 400, 100);
	ASSERT_TRUE(Created) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Created);
	const auto CallId = Created.GeneratedNodeIds.front();
	ASSERT_TRUE(Document.SetInputDefault(CallId, 0, {.Kind = EMaterialInputDefaultKind::Literal,
		.Type = EMaterialProgramValueType::Float3, .Literal = {4, 5, 6}}, InputId));
	ASSERT_TRUE(Document.ExtractInputDefault(CallId, 0, InputId));
	ASSERT_TRUE(Document.InlineInputNode(CallId, 0, InputId));
	const auto* Call = Cast<DMaterialExpressionFunctionCall>(Material->GetExpressionCollection().Expressions.front().Get());
	ASSERT_NE(Call, nullptr);
	ASSERT_EQ(Call->Inputs.size(), 1u);
	EXPECT_EQ(Call->Inputs.front().InputDefault, (std::vector<float>{4, 5, 6}));
	EXPECT_FALSE(Call->Inputs.front().Input.ExpressionId.IsValid());
	MarkAsGarbage(Material);
	MarkAsGarbage(Function);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, SamplingClipboardRemapsSharedExplicitCoordinates)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "CompactSamplingCommands");
	auto* Target = NewObject<DMaterial>(nullptr, "CompactSamplingTarget");
	FMaterialGraphDocument Document(*Material);
	Durin::Tests::FTestTransactorOwner Transactions;
	const auto Created = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::TextureSampleParameter2D, EMaterialProgramValueType::Float4, {}, 400, 0, Transactions.Get());
	ASSERT_TRUE(Created) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Created);
	const auto Id = Created.GeneratedNodeIds[0];
	const auto CreatedUV = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::TextureCoordinates,
		EMaterialProgramValueType::Float2, {}, 0, 0, Transactions.Get());
	ASSERT_TRUE(CreatedUV) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(CreatedUV);
	const auto CoordinatesId = CreatedUV.GeneratedNodeIds[0];
	ASSERT_TRUE(Document.SetInputDefault(CoordinatesId, 0, {.Kind = EMaterialInputDefaultKind::Literal,
		.Type = EMaterialProgramValueType::Float, .Literal = {1}}, {}, Transactions.Get()));
	const auto Before = CaptureExpressions(*Material);
	ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::Input(Id, 0), FMaterialGraphPinAddress::Output({CoordinatesId}), false, Transactions.Get()));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	ASSERT_TRUE(Transactions->Redo());
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(Document.CopySelection(std::array{Id, CoordinatesId}, Payload));
	EXPECT_EQ(Payload.Nodes.size(), 2u);
	const auto Pasted = FMaterialGraphDocument(*Target).Paste(Payload, 0, 0);
	ASSERT_TRUE(Pasted) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Pasted);
	ASSERT_EQ(Target->GetParameterDefinitions().size(), 1u);
	EXPECT_NE(Target->GetParameterDefinitions().front().Id, Material->GetParameterDefinitions().front().Id);
	for (const auto& Node : Target->GetExpressionCollection().Expressions)
	{
		if (const auto* Coordinates = Cast<DMaterialExpressionTextureCoordinates>(Node.Get()))
			EXPECT_EQ(Coordinates->ChannelDefault, (std::vector<float>{1}));
		if (const auto* Sample = Cast<DMaterialExpressionTextureSampleParameter2D>(Node.Get()))
			EXPECT_TRUE(Sample->UV.ExpressionId.IsValid());
	}
	auto* Function = NewObject<DMaterialFunction>(nullptr, "RejectRootBindings");
	EXPECT_FALSE(FMaterialGraphDocument(*Function).Paste(Payload, 0, 0));
	MarkAsGarbage(Function);
	MarkAsGarbage(Target);
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, ReplacementDraftCannotMutateOwnerOrHistoryAfterCommit)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	FMaterialGraphDocument Document(*Material.Get());
	const auto Created = Testing::CreateGraphConstant(Document, 0.25f);
	ASSERT_TRUE(Created);
	auto Draft = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>(Created.GeneratedNodeIds[0]);
	auto* Constant = Draft.Get();
	ASSERT_NE(Constant, nullptr);
	EXPECT_NE(Constant, Material->GetExpressionCollection().Expressions.front().Get());
	Constant->Value = 0.5f;
	Durin::Tests::FTestTransactorOwner Transactions;
	ASSERT_TRUE(Document.ReplaceExpression(*Constant, Transactions.Get()));
	Constant->Value = 0.9f;
	const auto Read = [&]() { return Cast<DMaterialExpressionScalarConstant>(Material->GetExpressionCollection().Expressions.front().Get())->Value; };
	EXPECT_FLOAT_EQ(Read(), 0.5f);
	CollectGarbage();
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FLOAT_EQ(Read(), 0.25f);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_FLOAT_EQ(Read(), 0.5f);
	const auto Revision = Material->GetMaterialCompileStatus().AuthoredRevision;
	Constant->Value = std::numeric_limits<float>::quiet_NaN();
	EXPECT_FALSE(Document.SetConstantValue(Constant->Id, FMaterialParameterValue::MakeScalar(Constant->Value), Transactions.Get()));
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision, Revision);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FLOAT_EQ(Read(), 0.25f);
}

TEST(FMaterialGraphOperationsTests, TypedReplacementRejectsRecursiveFunctionTargetsAtomically)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterialFunction> Caller(NewObject<DMaterialFunction>(nullptr, "TypedReplacementCaller"));
	TStrongObjectPtr<DMaterialFunction> Callee(NewObject<DMaterialFunction>(nullptr, "TypedReplacementCallee"));
	Durin::Tests::FTestTransactorOwner Transactions;
	FMaterialGraphDocument Document(*Caller.Get());
	const auto Inserted = Document.InsertFunctionCall(*Callee.Get(), 0, 0, Transactions.Get());
	ASSERT_TRUE(Inserted) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Inserted);
	const auto Revision = Caller->GetFunctionRevision();
	TStrongObjectPtr<DMaterialExpressionFunctionCall> Replacement(NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None));
	Replacement->Id = Inserted.GeneratedNodeIds.front();
	Replacement->Function = Caller.Get();
	for (const auto& Port : Caller->GetFunctionSignature().Outputs) Replacement->Outputs.push_back({Port.Id, Port.Type});
	const auto Rejected = Document.ReplaceExpression(*Replacement.Get(), Transactions.Get());
	EXPECT_FALSE(Rejected);
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
	Swizzle->Input = {Constant->Id};
	Swizzle->Components = {0, 1};
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 2>{Constant.Get(), Swizzle.Get()}, {}));
	Durin::Tests::FTestTransactorOwner Transactions;
	FMaterialGraphDocument Document(*Material);
	const auto Id = Constant->Id;
	Constant->Value = {0.5, 0.6, 0.7, 0.8};
	ASSERT_TRUE(Document.ReplaceExpression(*Constant.Get(), Transactions.Get()));
	Constant->Value = {9, 9, 9, 9};
	const auto ReadValue = [&]() { return Cast<DMaterialExpressionVector4Constant>(Material->GetExpressionCollection().Expressions[0].Get())->Value; };
	EXPECT_EQ(ReadValue(), FVector4(0.5, 0.6, 0.7, 0.8));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(ReadValue(), FVector4(0.1, 0.2, 0.3, 0.4));
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(ReadValue(), FVector4(0.5, 0.6, 0.7, 0.8));
	EXPECT_EQ(Material->GetExpressionCollection().Expressions[0]->GetOuter(), Material.Get());
	const auto ReadComponents = [&]() { return Cast<DMaterialExpressionSwizzle>(Material->GetExpressionCollection().Expressions[1].Get())->Components; };
	ASSERT_TRUE(Document.SetSwizzleComponents(Swizzle->Id, std::array<uint8, 1>{3}, Transactions.Get()));
	EXPECT_EQ(ReadComponents(), (std::vector<uint8>{3}));
	const auto Revision = Material->GetMaterialCompileStatus().AuthoredRevision;
	EXPECT_FALSE(Document.SetSwizzleComponents(Swizzle->Id, std::array<uint8, 1>{4}, Transactions.Get()));
	EXPECT_FALSE(Document.SetSwizzleComponents(Swizzle->Id, {}, Transactions.Get()));
	EXPECT_FALSE(Document.SetConstantValue(Id, FMaterialParameterValue::MakeTexture(nullptr), Transactions.Get()));
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision, Revision);
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
	auto Entries = FMaterialGraphOperations::SearchCatalog("constant");
	std::erase_if(Entries, [](const FMaterialGraphCatalogEntry& Entry) {
		return Entry.Opcode != EMaterialProgramOpcode::Constant;
	});
	ASSERT_EQ(Entries.size(), 1u);
	EXPECT_EQ(Entries.front().ResultType, EMaterialProgramValueType::Float);
	Durin::Tests::FTestTransactorOwner Transactions;
	const auto Created = FMaterialGraphDocument(*Material).CreateCatalogNode(Entries.front(), 0, 0, {}, Transactions.Get());
	ASSERT_TRUE(Created) << FormatMaterialGraphCommandResult(Created);
	const auto NodeId = Created.GeneratedNodeIds.front();
	auto ResultType = EMaterialProgramValueType::Float;
	const auto ApplyValue = [&]() {
		FMaterialParameterValue Value;
		switch (ResultType)
		{
		case EMaterialProgramValueType::Float2: Value = FMaterialParameterValue::MakeVector2({0.2f, 0.4f}); break;
		case EMaterialProgramValueType::Float3: Value = FMaterialParameterValue::MakeVector({0.2f, 0.4f, 0.6f}); break;
		case EMaterialProgramValueType::Float4: Value = FMaterialParameterValue::MakeVector4({0.2f, 0.4f, 0.6f, 0.8f}); break;
		default: Value = FMaterialParameterValue::MakeScalar(0.2f); break;
		}
		return FMaterialGraphDocument(*Material).SetConstantValue(NodeId, Value, Transactions.Get());
	};
	ASSERT_TRUE(ApplyValue());
	for (auto Type : {EMaterialProgramValueType::Float2, EMaterialProgramValueType::Float3,
		EMaterialProgramValueType::Float4})
	{
		const auto Before = CaptureExpressions(*Material);
		ResultType = Type;
		ASSERT_TRUE(ApplyValue());
		const auto CheckValue = [&]() {
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
	ASSERT_TRUE(FMaterialGraphDocument(*Material).Connect(FMaterialGraphPinAddress::MaterialOutput(Material->GetOutputNode()->Id, EMaterialSurfaceOutput::Roughness),
		FMaterialGraphPinAddress::Output({NodeId}), true, Transactions.Get()));
	const auto Connected = CaptureExpressions(*Material);
	const auto Revision = Material->GetMaterialCompileStatus().AuthoredRevision;
	ResultType = EMaterialProgramValueType::Float4;
	ASSERT_TRUE(ApplyValue());
	EXPECT_TRUE(HasGraphDiagnostics(*Material));
	EXPECT_GT(Material->GetMaterialCompileStatus().AuthoredRevision, Revision);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Connected);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FALSE(Material->GetExpressionOutputs().Roughness.ExpressionId.IsValid());
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, ParameterSharingUsesIndependentNodesAndRenamePreservesIdentity)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "SharedParameter");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialParameterDefinition Definition;
	Definition.Name = "SharedValue";
	Definition.Value = FMaterialParameterValue::MakeScalar(0.4f);
	const auto Created = FMaterialGraphOperations::CreateParameter(*Material, Definition);
	ASSERT_TRUE(Created) << FormatMaterialGraphCommandResult(Created);
	ASSERT_EQ(Created.GeneratedNodeIds.size(), 1u);
	const auto OwnerId = Created.GeneratedNodeIds.front();
	const auto ParameterId = Created.AffectedParameterIds.front();
	Durin::Tests::FTestTransactorOwner Transactions;
	FMaterialGraphDocument Document(*Material);
	ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::MaterialOutput(Material->GetOutputNode()->Id, EMaterialSurfaceOutput::Metallic), FMaterialGraphPinAddress::Output({OwnerId}), true, Transactions.Get()));
	ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::MaterialOutput(Material->GetOutputNode()->Id, EMaterialSurfaceOutput::Roughness), FMaterialGraphPinAddress::Output({OwnerId}), true, Transactions.Get()));
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 1u);
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 2u);
	TStrongObjectPtr<DMaterialExpression> DuplicateOwner(DuplicateObject(Material->GetExpressionCollection().Expressions.front().Get(), nullptr, NAME_None).Object);
	DuplicateOwner->Id = FGuid::NewGuid();
	ASSERT_TRUE(Document.CreateExpression(*DuplicateOwner.Get()));
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 1u);
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 3u);
	ASSERT_TRUE(FMaterialGraphOperations::RenameParameter(*Material, ParameterId, "RenamedShared", Transactions.Get()));
	EXPECT_EQ(Material->FindParameterDefinition("RenamedShared")->Id, ParameterId);
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.front()->Id, OwnerId);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_NE(Material->FindParameterDefinition("SharedValue"), nullptr);
	ASSERT_TRUE(Transactions->Redo());
	const auto View = FMaterialGraphDocument(*Material).Inspect();
	EXPECT_EQ(FindViewNode(View, OwnerId)->PrimaryLabel, Material->FindParameterDefinition(ParameterId)->DisplayName);
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, GenericParametersCreateIndependentDeclarationsAndUndoAtomically)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "GenericParameterMaterial");
	ASSERT_NE(Material, nullptr);
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	auto Entries = FMaterialGraphOperations::EnumerateCatalog();
	const auto IsParameter = [](const FMaterialGraphCatalogEntry& Entry) {
		return Entry.Opcode == EMaterialProgramOpcode::Parameter
			|| Entry.Opcode == EMaterialProgramOpcode::TextureParameter;
	};
	std::erase_if(Entries, [&](const auto& Entry) { return !IsParameter(Entry); });
	ASSERT_EQ(Entries.size(), 3u);
	Durin::Tests::FTestTransactorOwner Transactions;
	for (const auto& Entry : Entries)
	{
		ASSERT_NE(Entry.ExpressionClass, nullptr);
		EXPECT_TRUE(Entry.ExpressionClass->IsChildOf(DMaterialExpressionParameter::StaticClass()));
		const auto Before = CaptureExpressions(*Material);
		const auto BeforePresentation = Material->GetMaterialGraphPresentation();
		const std::vector<FMaterialParameterDefinition> BeforeDefinitions(
			Material->GetParameterDefinitions().begin(), Material->GetParameterDefinitions().end());
		const auto Created = FMaterialGraphDocument(*Material).CreateCatalogNode(Entry, 400, 200, {}, Transactions.Get());
		ASSERT_TRUE(Created) << FormatMaterialGraphCommandResult(Created);
		ASSERT_EQ(Created.GeneratedNodeIds.size(), 1u);
		ASSERT_EQ(Created.AffectedParameterIds.size(), 1u);
		EXPECT_EQ(Material->GetParameterDefinitions().size(), BeforeDefinitions.size() + 1);
		const auto After = CaptureExpressions(*Material);
		const auto AfterPresentation = Material->GetMaterialGraphPresentation();
		const auto* Parameter = Cast<DMaterialExpressionParameter>(After.Expressions[After.Expressions.size() - 2].Get());
		ASSERT_NE(Parameter, nullptr);
		EXPECT_EQ(Parameter->Metadata.Id, Created.AffectedParameterIds.front());
		EXPECT_EQ(Parameter->GetClass(), Entry.ResultType == EMaterialProgramValueType::Float2 || Entry.ResultType == EMaterialProgramValueType::Float3
			? DMaterialExpressionVector4Parameter::StaticClass() : Entry.ExpressionClass);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(CaptureExpressions(*Material), Before);
		EXPECT_EQ(Material->GetMaterialGraphPresentation(), BeforePresentation);
		EXPECT_TRUE(std::ranges::equal(Material->GetParameterDefinitions(), BeforeDefinitions));
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(CaptureExpressions(*Material), After);
		EXPECT_EQ(Material->GetMaterialGraphPresentation(), AfterPresentation);

		const auto Second = FMaterialGraphDocument(*Material).CreateCatalogNode(Entry, 0, 0, {}, Transactions.Get());
		ASSERT_TRUE(Second) << FormatMaterialGraphCommandResult(Second);
		EXPECT_NE(Second.AffectedParameterIds.front(), Created.AffectedParameterIds.front());
		const auto* FirstDefinition = Material->FindParameterDefinition(Created.AffectedParameterIds.front());
		const auto* SecondDefinition = Material->FindParameterDefinition(Second.AffectedParameterIds.front());
		ASSERT_NE(FirstDefinition, nullptr);
		ASSERT_NE(SecondDefinition, nullptr);
		EXPECT_NE(FirstDefinition->Name, SecondDefinition->Name);
		TStrongObjectPtr<DMaterialExpression> SharedNode(DuplicateObject(Material->GetExpressionCollection().Expressions[Material->GetExpressionCollection().Expressions.size() - 2].Get(), nullptr, NAME_None).Object);
		Cast<DMaterialExpressionParameter>(SharedNode.Get())->Metadata.Id = FirstDefinition->Id;
		EXPECT_FALSE(FMaterialGraphDocument(*Material).ReplaceExpression(*SharedNode.Get(), Transactions.Get()));
		EXPECT_EQ(Cast<DMaterialExpressionParameter>(Material->GetExpressionCollection().Expressions[Material->GetExpressionCollection().Expressions.size() - 2].Get())->Metadata.Id, Second.AffectedParameterIds.front());
		EXPECT_EQ(std::ranges::count_if(FMaterialGraphOperations::SearchCatalog("parameter"), IsParameter), 3);
	}
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, TypedLayoutIncludesCallAndSurfaceDependenciesAndPreservesLabels)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "TypedLayout");
	auto* Function = NewObject<DMaterialFunction>(nullptr, "LayoutSurfaceFunction");
	auto* Base = NewObject<DMaterialExpressionMakeSurface>(nullptr, NAME_None);
	Base->Id = FGuid::NewGuid();
	Base->BaseColorDefault = {.5f, .5f, .5f}; Base->NormalDefault = {0, 0, 1};
	Base->MetallicDefault = {0}; Base->RoughnessDefault = {.5f}; Base->AmbientOcclusionDefault = {1};
	Base->EmissiveDefault = {0, 0, 0}; Base->OpacityDefault = {1}; Base->OpacityMaskDefault = {1};
	auto* Constant = NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None);
	Constant->Id = FGuid::NewGuid(); Constant->Value = .7f;
	auto* Override = NewObject<DMaterialExpressionSetSurfaceAttributes>(nullptr, NAME_None);
	Override->Id = FGuid::NewGuid(); Override->Surface = {Base->Id};
	Override->Attributes.push_back({EMaterialSurfaceOutput::Roughness, {Constant->Id}});
	auto* Call = NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None);
	Call->Id = FGuid::NewGuid(); Call->Function = Function;
	const auto InputPort = Function->GetFunctionSignature().Inputs.front().Id;
	const auto OutputPort = Function->GetFunctionSignature().Outputs.front().Id;
	Call->Inputs.push_back({InputPort, EMaterialProgramValueType::Surface, {Override->Id}});
	Call->Outputs.push_back({OutputPort, EMaterialProgramValueType::Surface});
	const auto BaseId = Base->Id, ConstantId = Constant->Id, OverrideId = Override->Id, CallId = Call->Id;
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 4>{Call, Override, Base, Constant},
		{.Surface = {CallId, 0, OutputPort}, .bUseMaterialAttributes = true}));
	ASSERT_TRUE(Material->SetMaterialGraphPresentation({.Nodes = {{ConstantId, 100, 100, "Retained label"}}}) != Durin::EMaterialGraphPresentationResult::Rejected);
	const auto Revision = Material->GetMaterialCompileStatus().AuthoredRevision;
	Durin::Tests::FTestTransactorOwner Transactions;
	ASSERT_TRUE(FMaterialGraphDocument(*Material).Layout({}, Transactions.Get()));
	const auto Layout = Material->GetMaterialGraphPresentation();
	const auto Position = [&](FGuid Id) -> const FMaterialGraphNodePresentation& {
		return *std::ranges::find(Layout.Nodes, Id, &FMaterialGraphNodePresentation::NodeId);
	};
	EXPECT_LT(Position(BaseId).X, Position(OverrideId).X);
	EXPECT_LT(Position(ConstantId).X, Position(OverrideId).X);
	EXPECT_LT(Position(OverrideId).X, Position(CallId).X);
	EXPECT_LT(Position(CallId).X, Testing::OutputPosition(*Material, Layout).X);
	EXPECT_EQ(Position(ConstantId).DisplayName, "Retained label");
	EXPECT_EQ(FMaterialGraphDocument(*Material).Layout().GetStatus(), EMaterialGraphCommandStatus::NoChange);
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision, Revision);
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), Layout);
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision, Revision);
	EXPECT_TRUE(Transactions->Reset());
	MarkAsGarbage(Material); MarkAsGarbage(Function); CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, MaximumGraphLayoutIsDeterministicAndPresentationOnly)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "MaximumLayoutMaterial");
	ASSERT_NE(Material, nullptr);
	Testing::FTestMaterialExpressionGraph Graph;
	while (Graph.Expressions.size() < MaterialProgramMaxNodeCount - 1)
		Graph.Expressions.emplace_back(Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>().Get());
	ASSERT_TRUE(Graph.Apply(*Material));
	const auto MaximumGraph = CaptureExpressions(*Material);
	const uint64 SemanticRevision =
		Material->GetMaterialCompileStatus().AuthoredRevision;

	const FMaterialGraphCommandResult First =
		FMaterialGraphDocument(*Material).Layout();
	ASSERT_TRUE(First) << FormatMaterialGraphCommandResult(First);
	EXPECT_EQ(Material->GetMaterialGraphPresentation().Nodes.size(),
		MaterialProgramMaxNodeCount);
	EXPECT_EQ(CaptureExpressions(*Material), MaximumGraph);
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision,
		SemanticRevision);
	const FMaterialGraphPresentation FirstLayout =
		Material->GetMaterialGraphPresentation();
	const FMaterialGraphView LayoutView = FMaterialGraphDocument(*Material).Inspect();
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
	const FMaterialGraphCommandResult Second =
		FMaterialGraphDocument(*Material).Layout();
	EXPECT_EQ(Second.GetStatus(), EMaterialGraphCommandStatus::NoChange);
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), FirstLayout);

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
	auto Validation = Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Material);
	ASSERT_TRUE(Validation);
	std::string Error;
	ASSERT_TRUE(PrepareNewMaterialForEditing(*Material, Error)) << Error;
	const FMaterialGraphPresentation& Presentation =
		Material->GetMaterialGraphPresentation();
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
	EXPECT_FALSE(Material->GetExpressionOutputs().Surface.ExpressionId.IsValid());
	EXPECT_EQ(Presentation.Nodes.size(),
		Material->GetExpressionCollection().Expressions.size());
	EXPECT_NE(Material->GetOutputNode(), nullptr);
	const FMaterialGraphView View = FMaterialGraphDocument(*Material).Inspect();
	EXPECT_EQ(View.Nodes.size(), Material->GetExpressionCollection().Expressions.size());
	EXPECT_EQ(FindViewNode(View, Material->GetOutputNode()->Id)->Presentation, Testing::OutputPosition(*Material, Presentation));

	MarkObjectHierarchyAsGarbage(Package);
	CollectGarbage();
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
		auto Consumer = Testing::MakeGraphExpression<DMaterialExpressionSaturate>(Consumers[Index]);
		Consumer->ResultType = EMaterialProgramValueType::Float;
		Consumer->Input = {Sources[Sources.size() - Index - 1]};
		Graph.Expressions.emplace_back(Consumer.Get());
	}
	auto Validation = Graph.Apply(*Material);
	ASSERT_TRUE(Validation);
	ASSERT_TRUE(FMaterialGraphDocument(*Material).Layout());
	const FMaterialGraphView View = FMaterialGraphDocument(*Material).Inspect();
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
	ASSERT_TRUE(FMaterialGraphDocument(*Material).MoveNodes(std::span(&Occupied, 1)));
	ASSERT_TRUE(FMaterialGraphDocument(*Material).Layout(std::span(&Selected, 1)));
	const FMaterialGraphView Relayout = FMaterialGraphDocument(*Material).Inspect();
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
	CollectGarbage();
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
	Saturate->Input = {Constant->Id};
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 2>{Constant.Get(), Saturate.Get()}, {.BaseColor = {Saturate->Id}}));
	ASSERT_TRUE(FMaterialGraphDocument(*Material).Layout());
	std::vector<FGuid> AllNodes;
	for (const auto& Node : Material->GetExpressionCollection().Expressions) AllNodes.push_back(Node->Id);
	FMaterialGraphClipboardPayload Payload;
	const FMaterialGraphCommandResult Copied =
		FMaterialGraphDocument(*Material).CopySelection(AllNodes, Payload);
	ASSERT_TRUE(Copied) << FormatMaterialGraphCommandResult(Copied);
	ASSERT_EQ(Payload.Nodes.size() + 1, AllNodes.size());
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
	Durin::Tests::FTestTransactorOwner Transactions;
	const FMaterialGraphCommandResult Pasted = FMaterialGraphDocument(*Material).Paste(Payload, 1200, 400, Transactions.Get());
	ASSERT_TRUE(Pasted) << FormatMaterialGraphCommandResult(Pasted);
	ASSERT_EQ(Pasted.GeneratedNodeIds.size(), Payload.Nodes.size());
	std::unordered_set<FGuid> OriginalIds(AllNodes.begin(), AllNodes.end());
	for (const FGuid& Id : Pasted.GeneratedNodeIds)
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
	const FMaterialGraphCommandResult Rejected = FMaterialGraphDocument(*Material).Paste(UnknownVersion, 0, 0, Transactions.Get());
	EXPECT_EQ(Rejected.GetStatus(), EMaterialGraphCommandStatus::Rejected);
	EXPECT_EQ(CaptureExpressions(*Material), BeforeRejected);

	const auto* Dependent = FindExpression<DMaterialExpressionSaturate>(*Material, Saturate->Id);
	ASSERT_NE(Dependent, nullptr);
	const FGuid RequiredSource = Dependent->Input.ExpressionId;
	const FGuid DependentId = Dependent->Id;
	const auto ExternalInput = Dependent->Input;
	const FMaterialGraphCommandResult RequiredRemoval =
		FMaterialGraphDocument(*Material).RemoveNodes(std::span(&RequiredSource, 1), Transactions.Get());
	ASSERT_TRUE(RequiredRemoval);
	EXPECT_TRUE(HasGraphDiagnostics(*Material));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), BeforeRejected);
	FMaterialGraphClipboardPayload Partial;
	ASSERT_TRUE(FMaterialGraphDocument(*Material).CopySelection(std::span(&DependentId, 1), Partial));
	ASSERT_EQ(Partial.Nodes.size(), 1u);
	const auto* CopiedExpression = Cast<DMaterialExpressionSaturate>(Partial.Nodes.front().Expression.Get());
	ASSERT_NE(CopiedExpression, nullptr);
	EXPECT_EQ(CopiedExpression->Input, ExternalInput);
	const FMaterialGraphCommandResult PartialPaste =
		FMaterialGraphDocument(*Material).Paste(Partial, 0, 0, Transactions.Get());
	ASSERT_TRUE(PartialPaste) << FormatMaterialGraphCommandResult(PartialPaste);
	ASSERT_EQ(PartialPaste.GeneratedNodeIds.size(), 1u);
	const auto* PastedDependent = FindExpression<DMaterialExpressionSaturate>(*Material, PartialPaste.GeneratedNodeIds.front());
	ASSERT_NE(PastedDependent, nullptr);
	EXPECT_EQ(PastedDependent->Input, ExternalInput);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), BeforeRejected);

	const FMaterialGraphCommandResult StandaloneCreated = Testing::CreateGraphConstant(
		FMaterialGraphDocument(*Material), 0.75f, 80, 120, Transactions.Get());
	ASSERT_TRUE(StandaloneCreated);
	const FGuid StandaloneId = StandaloneCreated.GeneratedNodeIds.front();
	const FMaterialGraphCommandResult Duplicated =
		FMaterialGraphDocument(*Material).DuplicateNodes(std::span(&StandaloneId, 1), 40, 40, Transactions.Get());
	ASSERT_TRUE(Duplicated) << FormatMaterialGraphCommandResult(Duplicated);
	ASSERT_EQ(Duplicated.GeneratedNodeIds.size(), 1u);
	EXPECT_NE(Duplicated.GeneratedNodeIds.front(), StandaloneId);
	FMaterialGraphClipboardPayload CutPayload;
	const FMaterialGraphCommandResult Cut = FMaterialGraphDocument(*Material).CutSelection(std::span(&StandaloneId, 1), CutPayload, Transactions.Get());
	ASSERT_TRUE(Cut) << FormatMaterialGraphCommandResult(Cut);
	ASSERT_EQ(CutPayload.Nodes.size(), 1u);
	EXPECT_EQ(CutPayload.Nodes.front().Expression->Id, StandaloneId);
	EXPECT_EQ(FindViewNode(FMaterialGraphDocument(*Material).Inspect(), StandaloneId),
		nullptr);
	ASSERT_TRUE(Transactions->Undo());
	const FMaterialGraphView RestoredCut = FMaterialGraphDocument(*Material).Inspect();
	EXPECT_NE(FindViewNode(RestoredCut, StandaloneId), nullptr);
	EXPECT_TRUE(Transactions->Reset());
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
	ASSERT_TRUE(SourceDocument.Connect(FMaterialGraphPinAddress::MaterialOutput(Source->GetOutputNode()->Id, std::nullopt), FMaterialGraphPinAddress::Output({Call.GeneratedNodeIds[0], 0, OutputId}), true));
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(SourceDocument.CopySelection(Call.GeneratedNodeIds, Payload));
	ASSERT_EQ(Payload.Nodes.size(), 1u);
	ASSERT_NE(Cast<DMaterialExpressionFunctionCall>(Payload.Nodes.front().Expression.Get()), nullptr);
	TWeakObjectPtr<DMaterialFunction> WeakFunction(Function);
	MarkAsGarbage(Source);
	CollectGarbage();
	ASSERT_TRUE(WeakFunction.IsValid());
	EXPECT_FALSE(Payload.SourceRoot.IsValid());
	Durin::Tests::FTestTransactorOwner Transactions;
	const auto Before = CaptureExpressions(*Target);
	const auto Pasted = TargetDocument.Paste(Payload, 100, 100, Transactions.Get());
	ASSERT_TRUE(Pasted) << FormatMaterialGraphCommandResult(Pasted);
	EXPECT_NE(Pasted.GeneratedNodeIds[0], Call.GeneratedNodeIds[0]);
	EXPECT_EQ(Target->GetExpressionOutputs().Surface.OutputId, OutputId);
	EXPECT_EQ(Target->GetExpressionOutputs().Surface.ExpressionId, Pasted.GeneratedNodeIds[0]);
	const auto* PastedCall = FindExpression<DMaterialExpressionFunctionCall>(*Target, Pasted.GeneratedNodeIds[0]);
	ASSERT_NE(PastedCall, nullptr); EXPECT_EQ(PastedCall->Function.Get(), WeakFunction.Get());
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Target), Before);
	EXPECT_EQ(Target->GetExpressionCollection().Expressions.size(), 1u);
	ASSERT_TRUE(Transactions->Redo());
	FMaterialGraphDocument FunctionDocument(*WeakFunction.Get());
	const auto FunctionBefore = CaptureExpressions(*WeakFunction.Get());
	EXPECT_FALSE(FunctionDocument.Paste(Payload, 0, 0));
	EXPECT_EQ(CaptureExpressions(*WeakFunction.Get()), FunctionBefore);
	EXPECT_TRUE(Transactions->Reset());
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
	const auto SurfaceId = Source->GetExpressionCollection().Expressions.front()->Id;
	auto SetExpression = Testing::MakeGraphExpression<DMaterialExpressionSetSurfaceAttributes>();
	SetExpression->Surface = {SurfaceId};
	SetExpression->Attributes = {{EMaterialSurfaceOutput::Metallic, {Input.GeneratedNodeIds[0]}}};
	const auto Set = Document.CreateExpression(*SetExpression.Get());
	ASSERT_TRUE(Set);
	ASSERT_TRUE(Document.AddPort(true, {.Type = EMaterialProgramValueType::Surface, .Name = "Modified"}, {Set.GeneratedNodeIds[0]}));
	std::vector<FGuid> Selection;
	for (const auto& Node : Source->GetExpressionCollection().Expressions) Selection.push_back(Node->Id);
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(Document.CopySelection(Selection, Payload));
	const auto Before = CaptureExpressions(*Target);
	Durin::Tests::FTestTransactorOwner Transactions;
	const auto Pasted = Destination.Paste(Payload, 400, 100, Transactions.Get());
	ASSERT_TRUE(Pasted) << FormatMaterialGraphCommandResult(Pasted);
	const auto& Graph = CaptureExpressions(*Target);
	EXPECT_EQ(Graph.Signature.Inputs.size(), 4u);
	const auto Follow = std::ranges::find(Graph.Signature.Inputs, std::string("Follow"), &FMaterialFunctionPort::Name);
	ASSERT_NE(Follow, Graph.Signature.Inputs.end());
	EXPECT_NE(Follow->Default.InputId, OriginalInputId);
	EXPECT_NE(std::ranges::find(Graph.Signature.Inputs, Follow->Default.InputId, &FMaterialFunctionPort::Id), Graph.Signature.Inputs.end());
	EXPECT_EQ(Graph.Signature.Outputs.size(), 3u);
	std::unordered_set<FGuid> NewIds(Pasted.GeneratedNodeIds.begin(), Pasted.GeneratedNodeIds.end());
	for (const auto& Node : Graph.Expressions)
		if (NewIds.contains(Node->Id))
			VisitMaterialExpressionInputs(*Node.Get(), [&](uint32, FMaterialExpressionInput& Link) {
				if (Link.ExpressionId.IsValid()) EXPECT_TRUE(NewIds.contains(Link.ExpressionId));
			});
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Target), Before);
	ASSERT_TRUE(Transactions->Redo());
	FMaterialGraphClipboardPayload Cut;
	ASSERT_TRUE(Destination.CutSelection(Pasted.GeneratedNodeIds, Cut, Transactions.Get()));
	EXPECT_EQ(CaptureExpressions(*Target), Before);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Target->GetFunctionSignature().Outputs.size(), 3u);
	EXPECT_TRUE(Transactions->Reset());
	MarkAsGarbage(Source); MarkAsGarbage(Target); CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, CommandsAreAtomicAndTransactionsRestoreSemanticAndPresentationState)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "GraphOperationsMaterial");
	ASSERT_NE(Material, nullptr);
	const auto OriginalGraph = CaptureExpressions(*Material);
	const uint64 OriginalRevision =
		Material->GetMaterialCompileStatus().AuthoredRevision;
	Durin::Tests::FTestTransactorOwner Transactions;

	const FMaterialGraphCommandResult Created = Testing::CreateGraphConstant(
		FMaterialGraphDocument(*Material), 0.25f, 120, -80, Transactions.Get());
	ASSERT_TRUE(Created) << FormatMaterialGraphCommandResult(Created);
	ASSERT_EQ(Created.GeneratedNodeIds.size(), 1u);
	const FGuid CreatedId = Created.GeneratedNodeIds.front();
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(),
		OriginalGraph.Expressions.size() + 1);
	EXPECT_GT(Material->GetMaterialCompileStatus().AuthoredRevision,
		OriginalRevision);
	const uint64 SemanticRevision =
		Material->GetMaterialCompileStatus().AuthoredRevision;

	const FMaterialGraphNodePresentation Moved{CreatedId, 320, 160};
	const FMaterialGraphCommandResult Move =
		FMaterialGraphDocument(*Material).MoveNodes(std::span(&Moved, 1), Transactions.Get());
	ASSERT_TRUE(Move) << FormatMaterialGraphCommandResult(Move);
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision,
		SemanticRevision);
	const FMaterialGraphView MovedGraph = FMaterialGraphDocument(*Material).Inspect();
	const FMaterialGraphNodeView* MovedView = FindViewNode(MovedGraph, CreatedId);
	ASSERT_NE(MovedView, nullptr);
	EXPECT_EQ(MovedView->Presentation.X, 320);

	const auto BeforeRejected = CaptureExpressions(*Material);
	ASSERT_NE(FindExpression<DMaterialExpressionScalarConstant>(*Material, CreatedId), nullptr);
	const FMaterialGraphCommandResult Rejected = FMaterialGraphDocument(*Material).SetConstantValue(CreatedId,
		FMaterialParameterValue::MakeScalar(std::numeric_limits<float>::quiet_NaN()), Transactions.Get());
	EXPECT_EQ(Rejected.GetStatus(), EMaterialGraphCommandStatus::Rejected);
	EXPECT_FALSE(Rejected.Diagnostics.empty());
	EXPECT_EQ(CaptureExpressions(*Material), BeforeRejected);
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision,
		SemanticRevision);

	ASSERT_TRUE(Transactions->Undo());
	const FMaterialGraphView UnmovedGraph = FMaterialGraphDocument(*Material).Inspect();
	const FMaterialGraphNodeView* UnmovedView = FindViewNode(UnmovedGraph, CreatedId);
	ASSERT_NE(UnmovedView, nullptr);
	EXPECT_EQ(UnmovedView->Presentation.X, 120);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), OriginalGraph);
	ASSERT_TRUE(Transactions->Redo());
	const FMaterialGraphView RecreatedGraph = FMaterialGraphDocument(*Material).Inspect();
	EXPECT_NE(FindViewNode(RecreatedGraph, CreatedId), nullptr);
	ASSERT_TRUE(Transactions->Redo());
	const FMaterialGraphView RemovedGraph = FMaterialGraphDocument(*Material).Inspect();
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
	const FMaterialGraphView CoalescedUndoGraph = FMaterialGraphDocument(*Material).Inspect();
	const FMaterialGraphNodeView* CoalescedUndo = FindViewNode(
		CoalescedUndoGraph, CreatedId);
	ASSERT_NE(CoalescedUndo, nullptr);
	EXPECT_EQ(CoalescedUndo->Presentation.X, 320);
	EXPECT_TRUE(Transactions->Reset());
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests,
	MovePreviewRejectsSemanticChangesAndRestoresOnlyItsPositions)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "StaleGraphMove");
	ASSERT_NE(Material, nullptr);

	const FMaterialGraphCommandResult Created = Testing::CreateGraphConstant(FMaterialGraphDocument(*Material), 0.f, 40, 80);
	ASSERT_TRUE(Created) << FormatMaterialGraphCommandResult(Created);
	ASSERT_EQ(Created.GeneratedNodeIds.size(), 1u);
	const FGuid NodeId = Created.GeneratedNodeIds.front();

	FMaterialGraphMoveSession MoveSession;
	ASSERT_TRUE(MoveSession.Begin(*Material, std::span(&NodeId, 1)));
	const FMaterialGraphNodePresentation Preview{NodeId, 400, 240};
	ASSERT_TRUE(MoveSession.Apply(std::span(&Preview, 1)));

	FMaterialGraphDocument Document(*Material);
	auto Validation = Document.SetInputDefault(Material->GetOutputNode()->Id,
		static_cast<uint32>(EMaterialOutputPin::Roughness), {.Kind = EMaterialInputDefaultKind::Literal, .Type = EMaterialProgramValueType::Float, .Literal = {.X = .37f}});
	ASSERT_TRUE(Validation);
	const uint64 SemanticRevision =
		Material->GetMaterialCompileStatus().AuthoredRevision;

	const FMaterialGraphNodePresentation StalePreview{NodeId, 640, 360};
	const FMaterialGraphCommandResult Rejected =
		MoveSession.Apply(std::span(&StalePreview, 1));
	EXPECT_EQ(Rejected.GetStatus(), EMaterialGraphCommandStatus::Rejected);
	EXPECT_FALSE(MoveSession.IsActive());
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision,
		SemanticRevision);
	EXPECT_FLOAT_EQ(Material->GetExpressionOutputs().RoughnessDefault,
		0.37f);
	const FMaterialGraphView RestoredView =
		FMaterialGraphDocument(*Material).Inspect();
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
	ASSERT_TRUE(Material->SetMaterialExpressions({}, {}));
	ASSERT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
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
	ASSERT_TRUE(Promoted) << FormatMaterialGraphCommandResult(Promoted);
	ASSERT_EQ(Promoted.GeneratedNodeIds.size(), 1u);
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 3u);
	EXPECT_TRUE(Material->GetExpressionOutputs().BaseColor.ExpressionId.IsValid());
	FVector4 BaseColor;
	ASSERT_TRUE(ReadVector4Parameter(*Material,
		Material->FindParameterDefinition(Promoted.AffectedParameterIds.front())->Name, BaseColor));
	EXPECT_NEAR(BaseColor.x, 0.2, 1.e-6);
	EXPECT_NEAR(BaseColor.y, 0.3, 1.e-6);
	EXPECT_NEAR(BaseColor.z, 0.4, 1.e-6);
	const auto PromotedGraph = Normalize(*Material);
	ASSERT_TRUE(PromotedGraph);
	const auto& PromotedDependencies = PromotedGraph.ActiveParameters;
	ASSERT_EQ(PromotedDependencies.size(), 1u);
	EXPECT_EQ(PromotedDependencies.front().Id,
		Promoted.AffectedParameterIds.front());
	const uint64 CompileGeneration =
		Material->GetMaterialCompileStatus().RequestGeneration;
	ASSERT_TRUE(FMaterialGraphOperations::SetParameterValue(
		*Material, Promoted.AffectedParameterIds.front(),
		FMaterialParameterValue::MakeVector4({0.7, 0.6, 0.5, 0}),
		Transactions.Get()));
	EXPECT_EQ(Material->GetMaterialCompileStatus().RequestGeneration,
		CompileGeneration);
	ASSERT_TRUE(ReadVector4Parameter(*Material,
		Material->FindParameterDefinition(Promoted.AffectedParameterIds.front())->Name, BaseColor));
	EXPECT_EQ(BaseColor, FVector4(0.7, 0.6, 0.5, 0));
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(ReadVector4Parameter(*Material,
		Material->FindParameterDefinition(Promoted.AffectedParameterIds.front())->Name, BaseColor));
	EXPECT_NEAR(BaseColor.x, 0.2, 1.e-6);
	EXPECT_NEAR(BaseColor.y, 0.3, 1.e-6);
	EXPECT_NEAR(BaseColor.z, 0.4, 1.e-6);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Transactions->Undo());

	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
	EXPECT_TRUE(Material->GetParameterDefinitions().empty());
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(FMaterialGraphDocument(*Material).Disconnect(FMaterialGraphPinAddress::MaterialOutput((*Material).GetOutputNode()->Id, EMaterialSurfaceOutput::BaseColor), Transactions.Get()));
	EXPECT_FALSE(Material->GetExpressionOutputs().BaseColor.ExpressionId.IsValid());
	EXPECT_EQ(Material->GetExpressionOutputs().BaseColorDefault,
		FVector3(EditedBaseColor.X, EditedBaseColor.Y, EditedBaseColor.Z));
	const auto DisconnectedGraph = Normalize(*Material);
	ASSERT_TRUE(DisconnectedGraph);
	EXPECT_TRUE(DisconnectedGraph.ActiveParameters.empty());

	ASSERT_TRUE(FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	const FMaterialGraphCommandResult Textured =
		FMaterialGraphOperations::AddTextureToSurfaceOutput(*Material, {
			.Output = EMaterialSurfaceOutput::Normal,
			.X = 400,
			.Y = 200}, Transactions.Get());
	ASSERT_TRUE(Textured) << FormatMaterialGraphCommandResult(Textured);
	ASSERT_EQ(Textured.GeneratedNodeIds.size(), 1u);
	EXPECT_EQ(Material->GetExpressionOutputs().Normal.OutputIndex, 1u);
	const MIR::FNormalizationResult Normalized = Normalize(*Material);
	ASSERT_TRUE(Normalized);
	EXPECT_EQ(std::ranges::count(Normalized.IR.Nodes, EMaterialProgramOpcode::TextureSample2D,
		&MIR::FNode::Opcode), 1);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FALSE(Material->GetExpressionOutputs().Normal.ExpressionId.IsValid());
	EXPECT_TRUE(Transactions->Reset());
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, TextureEditSessionRetainsOriginalAcrossCollection)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "TextureSessionMaterial"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	auto* Original = NewObject<DTexture2D>(nullptr, "TextureSessionOriginal");
	auto* Replacement = NewObject<DTexture2D>(nullptr, "TextureSessionReplacement");
	TWeakObjectPtr<DTexture2D> WeakOriginal(Original);
	const FGuid Id = FGuid::NewGuid();
	ASSERT_TRUE(FMaterialGraphOperations::CreateParameter(*Material, {.Id = Id,
		.Name = FName("Texture"), .Type = EMaterialParameterType::Texture,
		.Value = FMaterialParameterValue::MakeTexture(Original)}));
	FMaterialGraphParameterEditSession Session;
	ASSERT_TRUE(Session.Begin(*Material, Id, nullptr));
	ASSERT_TRUE(Session.Apply(FMaterialParameterValue::MakeTexture(Replacement)));
	CollectGarbage();
	ASSERT_TRUE(WeakOriginal.IsValid());
	ASSERT_TRUE(Session.Cancel());
	EXPECT_EQ(Material->FindParameterDefinition(Id)->Value.GetTexture().Texture.Get(), WeakOriginal.Get());
	Material.Reset();
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

	const FGuid ParameterId = Material->GetParameterDefinitions().front().Id;
	const uint64 CompileGeneration =
		Material->GetMaterialCompileStatus().RequestGeneration;
	FVector4 OriginalBaseColor;
	ASSERT_TRUE(ReadVector4Parameter(*Material,
		Material->FindParameterDefinition(ParameterId)->Name, OriginalBaseColor));
	Durin::Tests::FTestTransactorOwner Transactions;
	FMaterialGraphParameterEditSession Session;
	ASSERT_TRUE(Session.Begin(*Material, ParameterId, Transactions.Get()));
	ASSERT_TRUE(Session.Apply(FMaterialParameterValue::MakeVector4(
		{0.4, 0.5, 0.6, 0})));
	FVector4 BaseColor;
	ASSERT_TRUE(ReadVector4Parameter(*Material,
		Material->FindParameterDefinition(ParameterId)->Name, BaseColor));
	EXPECT_EQ(BaseColor, FVector4(0.4, 0.5, 0.6, 0));
	EXPECT_EQ(Material->GetMaterialCompileStatus().RequestGeneration,
		CompileGeneration);
	ASSERT_TRUE(Session.Apply(FMaterialParameterValue::MakeVector4(
		{0.7, 0.8, 0.9, 0})));
	ASSERT_TRUE(ReadVector4Parameter(*Material,
		Material->FindParameterDefinition(ParameterId)->Name, BaseColor));
	EXPECT_EQ(BaseColor, FVector4(0.7, 0.8, 0.9, 0));
	EXPECT_EQ(Material->GetMaterialCompileStatus().RequestGeneration,
		CompileGeneration);
	ASSERT_TRUE(Session.Commit());
	ASSERT_TRUE(FMaterialGraphDocument(*Material).MoveMaterialOutput(713, -91));

	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(ReadVector4Parameter(*Material,
		Material->FindParameterDefinition(ParameterId)->Name, BaseColor));
	EXPECT_EQ(BaseColor, OriginalBaseColor);
	EXPECT_EQ(Testing::OutputPosition(*Material, Material->GetMaterialGraphPresentation()).X, 713);
	EXPECT_EQ(Testing::OutputPosition(*Material, Material->GetMaterialGraphPresentation()).Y, -91);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(ReadVector4Parameter(*Material,
		Material->FindParameterDefinition(ParameterId)->Name, BaseColor));
	EXPECT_EQ(BaseColor, FVector4(0.7, 0.8, 0.9, 0));

	ASSERT_TRUE(Session.Begin(*Material, ParameterId, Transactions.Get()));
	ASSERT_TRUE(Session.Apply(FMaterialParameterValue::MakeVector4(
		{0.1, 0.1, 0.1, 0})));
	ASSERT_TRUE(Session.Cancel());
	ASSERT_TRUE(ReadVector4Parameter(*Material,
		Material->FindParameterDefinition(ParameterId)->Name, BaseColor));
	EXPECT_EQ(BaseColor, FVector4(0.7, 0.8, 0.9, 0));
	EXPECT_EQ(Material->GetMaterialCompileStatus().RequestGeneration,
		CompileGeneration);
	EXPECT_TRUE(Transactions->Reset());
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, MoveAndDefaultHistoryDoNotGrowWithUnrelatedNodes)
{
	InitializeDObjectSystem();
	std::optional<size_t> SmallMoveBytes, SmallDefaultBytes;
	for (uint32 UnrelatedCount : {0u, 128u})
	{
		TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		const auto Created = Testing::CreateGraphConstant(FMaterialGraphDocument(*Material), .5f, 10, 20);
		ASSERT_TRUE(Created);
		const auto NodeId = Created.GeneratedNodeIds.front();
		std::vector<DMaterialExpression*> Nodes;
		for (auto& E : Material->GetExpressionCollection().Expressions) Nodes.push_back(E.Get());
		for (uint32 I = 0; I < UnrelatedCount; ++I)
		{
			auto* E = NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None);
			E->Id = FGuid::NewGuid(); Nodes.push_back(E);
		}
		ASSERT_TRUE(Material->SetMaterialExpressions(Nodes));
		Durin::Tests::FTestTransactorOwner Transactions;
		const FMaterialGraphNodePresentation Moved{NodeId, 40, 80};
		ASSERT_TRUE(FMaterialGraphDocument(*Material).MoveNodes(std::span(&Moved, 1), Transactions.Get()));
		const auto MoveBytes = Transactions->GetOwnedBytes();
		EXPECT_GT(MoveBytes, 0u);
		if (SmallMoveBytes) EXPECT_EQ(MoveBytes, *SmallMoveBytes);
		else SmallMoveBytes = MoveBytes;
		ASSERT_TRUE(Transactions->Reset());
		ASSERT_TRUE(FMaterialGraphOperations::SetSurfaceDefault(*Material,
			{.Output = EMaterialSurfaceOutput::Roughness, .Value = {.41f, 0.f, 0.f, 0.f}}, Transactions.Get()));
		const auto DefaultBytes = Transactions->GetOwnedBytes();
		EXPECT_GT(DefaultBytes, 0u);
		if (SmallDefaultBytes) EXPECT_EQ(DefaultBytes, *SmallDefaultBytes);
		else SmallDefaultBytes = DefaultBytes;
		ASSERT_TRUE(Transactions->Undo());
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_FLOAT_EQ(Material->GetExpressionOutputs().RoughnessDefault, .41f);
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
	ASSERT_TRUE(FMaterialGraphDocument(*Source).Layout());
	ASSERT_TRUE(Target->SetMaterialExpressions({}, {}));
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(FMaterialGraphDocument(*Source).CopySelection(std::span(&Node->Id, 1), Payload));
	ASSERT_EQ(Payload.Nodes.size(), 1u);
	Durin::Tests::FTestTransactorOwner Transactions;
	const auto BeforeRevision = Target->GetMaterialCompileStatus().AuthoredRevision;
	const auto Pasted = FMaterialGraphDocument(*Target).Paste(Payload, 300, 200, Transactions.Get());
	ASSERT_TRUE(Pasted) << FormatMaterialGraphCommandResult(Pasted);
	ASSERT_EQ(Target->GetParameterDefinitions().size(), 1u);
	const auto LocalId = Target->GetParameterDefinitions().front().Id;
	EXPECT_NE(LocalId, Definition.Id);
	EXPECT_EQ(Cast<DMaterialExpressionParameter>(Target->GetExpressionCollection().Expressions.front().Get())->Metadata.Id, LocalId);
	EXPECT_EQ(Target->GetMaterialCompileStatus().AuthoredRevision, BeforeRevision + 1);
	const auto AfterPresentation = Target->GetMaterialGraphPresentation();
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_TRUE(Target->GetParameterDefinitions().empty());
	EXPECT_EQ(Target->GetExpressionCollection().Expressions.size(), 1u);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Target->GetParameterDefinitions().front().Id, LocalId);
	EXPECT_EQ(Target->GetMaterialGraphPresentation(), AfterPresentation);
	ASSERT_TRUE(FMaterialGraphDocument(*Target).Paste(Payload, 600, 200));
	EXPECT_EQ(Target->GetParameterDefinitions().size(), 1u);
	EXPECT_EQ(Cast<DMaterialExpressionParameter>(Target->GetExpressionCollection().Expressions[Target->GetExpressionCollection().Expressions.size() - 2].Get())->Metadata.Id, LocalId);

	const auto BeforeProgram = CaptureExpressions(*Target);
	const auto BeforePresentation = Target->GetMaterialGraphPresentation();
	const auto Revision = Target->GetMaterialCompileStatus().AuthoredRevision;
	Cast<DMaterialExpressionParameter>(Payload.Nodes.front().Expression.Get())->Metadata.Name = {};
	EXPECT_FALSE(FMaterialGraphDocument(*Target).Paste(Payload, 0, 0));
	EXPECT_EQ(CaptureExpressions(*Target), BeforeProgram);
	EXPECT_EQ(Target->GetMaterialGraphPresentation(), BeforePresentation);
	EXPECT_EQ(Target->GetMaterialCompileStatus().AuthoredRevision, Revision);
	Cast<DMaterialExpressionParameter>(Payload.Nodes.front().Expression.Get())->Metadata = {};
	EXPECT_FALSE(FMaterialGraphDocument(*Target).Paste(Payload, 0, 0));
	EXPECT_EQ(CaptureExpressions(*Target), BeforeProgram);
	MarkAsGarbage(Source);
	MarkAsGarbage(Target);
	CollectGarbage();
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
	ASSERT_TRUE(FMaterialGraphDocument(*Material).Layout());
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(FMaterialGraphDocument(*Material).CopySelection(std::span(&Node->Id, 1), Payload));
	ASSERT_TRUE(FMaterialGraphOperations::RenameParameter(*Material, Definition.Id, "RenamedAmount"));
	ASSERT_TRUE(FMaterialGraphDocument(*Material).Paste(Payload, 400, 0));
	EXPECT_NE(Cast<DMaterialExpressionParameter>(Material->GetExpressionCollection().Expressions[Material->GetExpressionCollection().Expressions.size() - 2].Get())->Metadata.Id, Definition.Id);
	EXPECT_EQ(Cast<DMaterialExpressionParameter>(Material->GetExpressionCollection().Expressions[Material->GetExpressionCollection().Expressions.size() - 2].Get())->Metadata.Name, Definition.Name);
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 2u);
	ASSERT_TRUE(Material->SetMaterialExpressions({}, {}));
	EXPECT_TRUE(FMaterialGraphDocument(*Material).Paste(Payload, 400, 0));
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 1u);
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, DeclarationCommandsAndConstantPromotionShareAtomicHistory)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "DeclarationCommands");
	ASSERT_TRUE(Material->SetMaterialExpressions({}, {}));
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
	ASSERT_TRUE(Reused);
	EXPECT_EQ(Reused.AffectedParameterIds.front(), Id);
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 1u);
	EXPECT_EQ(Material->FindParameterDefinition(Id)->Value.GetScalar(), 0.25f);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 2u);
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
	EXPECT_TRUE(Transactions->Reset());

	auto Constant = Testing::MakeGraphExpression<DMaterialExpressionVector4Constant>();
	Constant->Value = {1, 2, 3, 4};
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 1>{Constant.Get()}, {}));
	const auto Original = CaptureExpressions(*Material);
	const auto Revision = Material->GetMaterialCompileStatus().AuthoredRevision;
	const auto Promoted = FMaterialGraphOperations::PromoteConstantToParameter(
		*Material, Constant->Id, "Tint", Transactions.Get());
	ASSERT_TRUE(Promoted) << FormatMaterialGraphCommandResult(Promoted);
	ASSERT_EQ(Promoted.AffectedParameterIds.size(), 1u);
	const auto TintId = Promoted.AffectedParameterIds.front();
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision, Revision + 1);
	EXPECT_EQ(Material->FindParameterDefinition(TintId)->Value.GetVector4(), FVector4(1, 2, 3, 4));
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.front()->Id, Constant->Id);
	EXPECT_EQ(Cast<DMaterialExpressionParameter>(Material->GetExpressionCollection().Expressions.front().Get())->Metadata.Id, TintId);
	ASSERT_TRUE(FMaterialGraphOperations::DeleteParameter(*Material, TintId, Transactions.Get()));
	EXPECT_TRUE(Material->GetParameterDefinitions().empty());
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->FindParameterDefinition(TintId), nullptr);
	EXPECT_EQ(CaptureExpressions(*Material), Original);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Cast<DMaterialExpressionParameter>(Material->GetExpressionCollection().Expressions.front().Get())->Metadata.Id, TintId);
	EXPECT_TRUE(Transactions->Reset());
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
	auto Node = Testing::MakeGraphExpression<DMaterialExpressionTextureParameter>();
	Node->Metadata.Id = Definition.Id; Node->Metadata.Name = Definition.Name;
	Node->Metadata.DisplayName = Definition.DisplayName;
	Node->DefaultValue = Definition.Value.GetTexture();
	ASSERT_TRUE(Source->SetMaterialExpressions(std::array<DMaterialExpression*, 1>{Node.Get()}, {}));
	ASSERT_TRUE(FMaterialGraphDocument(*Source).Layout());
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(FMaterialGraphDocument(*Source).CopySelection(std::span(&Node->Id, 1), Payload));
	Node.Reset();
	MarkAsGarbage(Source);
	CollectGarbage();
	ASSERT_NE(WeakTexture.Get(), nullptr);
	EXPECT_EQ(Payload.SourceRoot.Get(), nullptr);
	DMaterial* Target = NewObject<DMaterial>(nullptr, "ClipboardTextureTarget");
	ASSERT_TRUE(Target->SetMaterialExpressions({}, {}));
	ASSERT_TRUE(FMaterialGraphDocument(*Target).Paste(Payload, 0, 0));
	EXPECT_EQ(Target->GetParameterDefinitions().front().Value.GetTexture().Texture.Get(), WeakTexture.Get());
	MarkAsGarbage(Target);
	CollectGarbage();
	EXPECT_NE(WeakTexture.Get(), nullptr);
	Payload = {};
	CollectGarbage();
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
	ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Material->GetExpressionOutputs()));
	const FGuid Id = Parameter->Id;
	TWeakObjectPtr<DMaterialExpression> Deleted(Material->GetExpressionCollection().Expressions[0].Get());
	Durin::Tests::FTestTransactorOwner Transactions;
	ASSERT_TRUE(FMaterialGraphDocument(*Material).RemoveNodes(std::span(&Id, 1), Transactions.Get()));
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
	Parameter.Reset();
	CollectGarbage();
	EXPECT_TRUE(Deleted.IsValid());
	EXPECT_EQ(Deleted.Get()->GetOuter(), nullptr);
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_EQ(Material->GetExpressionCollection().Expressions.size(), 2u);
	auto* Restored = Cast<DMaterialExpressionScalarParameter>(Material->GetExpressionCollection().Expressions[0].Get());
	ASSERT_NE(Restored, nullptr);
	EXPECT_EQ(Restored->GetOuter(), Material.Get());
	EXPECT_EQ(Restored->Id, Id);
	EXPECT_FLOAT_EQ(Restored->DefaultValue, 0.37f);
	EXPECT_FLOAT_EQ(Restored->MinimumValue, 0.1f);
	EXPECT_FLOAT_EQ(Restored->MaximumValue, 0.9f);
	// Direct live edits must not mutate the serialized history payload.
	Restored->DefaultValue = 0.72f;
	Restored->Metadata.DisplayName = "Live edit";
	ASSERT_TRUE(Transactions->Redo());
	CollectGarbage();
	ASSERT_TRUE(Transactions->Undo());
	Restored = Cast<DMaterialExpressionScalarParameter>(Material->GetExpressionCollection().Expressions[0].Get());
	ASSERT_NE(Restored, nullptr);
	EXPECT_FLOAT_EQ(Restored->DefaultValue, 0.37f);
	EXPECT_TRUE(Restored->Metadata.DisplayName.empty());
	EXPECT_TRUE(Transactions->Reset());
	CollectGarbage();
	EXPECT_EQ(Material->GetExpressionCollection().Expressions[0].Get(), Restored);
	Material.Reset();
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, DeletingSurfaceOverrideSourceRestoresBaseAndSupportsUndo)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "SurfaceDelete"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialExpressionMakeSurface> Base(NewObject<DMaterialExpressionMakeSurface>(nullptr, NAME_None));
	Base->Id = FGuid::NewGuid();
	Base->BaseColorDefault = {.5f, .5f, .5f}; Base->NormalDefault = {0, 0, 1};
	Base->MetallicDefault = {0}; Base->RoughnessDefault = {.5f}; Base->AmbientOcclusionDefault = {1};
	Base->EmissiveDefault = {0, 0, 0}; Base->OpacityDefault = {1}; Base->OpacityMaskDefault = {1};
	TStrongObjectPtr<DMaterialExpressionScalarConstant> Value(NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None));
	Value->Id = FGuid::NewGuid(); Value->Value = .7f;
	TStrongObjectPtr<DMaterialExpressionSetSurfaceAttributes> Surface(NewObject<DMaterialExpressionSetSurfaceAttributes>(nullptr, NAME_None));
	Surface->Id = FGuid::NewGuid(); Surface->Surface = {Base->Id};
	Surface->Attributes = {{EMaterialSurfaceOutput::Roughness, {Value->Id}}};
	const std::array<DMaterialExpression*, 3> Expressions{Base.Get(), Value.Get(), Surface.Get()};
	FMaterialExpressionSurfaceOutputs Outputs; Outputs.Surface = {Surface->Id}; Outputs.bUseMaterialAttributes = true;
	ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
	Tests::FTestTransactorOwner Transactions;
	const auto DeletedId = Value->Id;
	ASSERT_TRUE(FMaterialGraphDocument(*Material).RemoveNodes(std::span(&DeletedId, 1), Transactions.Get()));
	const auto* Remaining = Cast<DMaterialExpressionSetSurfaceAttributes>(Material->GetExpressionCollection().Expressions[Material->GetExpressionCollection().Expressions.size() - 2].Get());
	ASSERT_NE(Remaining, nullptr);
	EXPECT_TRUE(Remaining->Attributes.empty());
	EXPECT_EQ(Remaining->Surface.ExpressionId, Base->Id);
	ASSERT_TRUE(Transactions->Undo());
	Remaining = Cast<DMaterialExpressionSetSurfaceAttributes>(Material->GetExpressionCollection().Expressions[Material->GetExpressionCollection().Expressions.size() - 2].Get());
	ASSERT_EQ(Remaining->Attributes.size(), 1u);
	EXPECT_EQ(Remaining->Attributes.front().Source.ExpressionId, DeletedId);
	EXPECT_TRUE(Transactions->Reset()); Material.Reset();
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, SharedParametersSynchronizeRebindAndUndo)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "SharedParameterCommands");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	Durin::Tests::FTestTransactorOwner Transactions;
	FMaterialGraphDocument Document(*Material);
	FMaterialParameterDefinition Definition;
	Definition.Name = "Amount"; Definition.Value = FMaterialParameterValue::MakeScalar(.25f);
	const auto A = FMaterialGraphOperations::CreateParameter(*Material, Definition);
	ASSERT_TRUE(A);
	const auto Id = A.AffectedParameterIds.front();
	const auto B = FMaterialGraphDocument(*Material).DuplicateNodes(A.GeneratedNodeIds, 100, 0);
	ASSERT_TRUE(B) << FormatMaterialGraphCommandResult(B);
	ASSERT_EQ(Material->GetParameterDefinitions().size(), 1u);
	ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::MaterialOutput(Material->GetOutputNode()->Id, EMaterialSurfaceOutput::Metallic), FMaterialGraphPinAddress::Output({A.GeneratedNodeIds.front()}), true));
	ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::MaterialOutput(Material->GetOutputNode()->Id, EMaterialSurfaceOutput::Roughness), FMaterialGraphPinAddress::Output({B.GeneratedNodeIds.front()}), true));
	std::vector<DMaterialExpression*> Expressions;
	for (const auto& E : Material->GetExpressionCollection().Expressions) Expressions.push_back(E.Get());
	const auto Built = MIR::FGraphBuilder(Expressions).FinishSurface(Material->GetExpressionOutputs());
	ASSERT_TRUE(Built.Diagnostics.empty());
	ASSERT_EQ(Built.Parameters.size(), 1u);
	EXPECT_EQ(Built.Parameters.front().Id, Id);
	auto* Instance = NewObject<DMaterialInstance>(nullptr, "SharedParameterInstance");
	ASSERT_TRUE(Instance->SetParent(Material));
	ASSERT_TRUE(Instance->SetParameterValue(Id, FMaterialParameterValue::MakeScalar(.8f)));
	ASSERT_TRUE(FMaterialGraphOperations::SetParameterValue(*Material, Id, FMaterialParameterValue::MakeScalar(.6f), Transactions.Get()));
	for (const auto& E : Material->GetExpressionCollection().Expressions)
		if (const auto* Parameter = Cast<DMaterialExpressionScalarParameter>(E.Get())) EXPECT_FLOAT_EQ(Parameter->DefaultValue, .6f);
	ASSERT_TRUE(Transactions->Undo());
	for (const auto& E : Material->GetExpressionCollection().Expressions)
		if (const auto* Parameter = Cast<DMaterialExpressionScalarParameter>(E.Get())) EXPECT_FLOAT_EQ(Parameter->DefaultValue, .25f);
	ASSERT_TRUE(Transactions->Redo());

	TStrongObjectPtr<DMaterialExpressionScalarParameter> Edit(Cast<DMaterialExpressionScalarParameter>(
		DuplicateObject(Material->GetExpressionCollection().Expressions[Material->GetExpressionCollection().Expressions.size() - 2].Get(), nullptr, NAME_None).Object));
	Edit->Metadata.GroupName = "Shared Group";
	ASSERT_TRUE(Document.ReplaceExpression(*Edit.Get(), Transactions.Get()));
	EXPECT_EQ(Cast<DMaterialExpressionParameter>(Material->GetExpressionCollection().Expressions.front().Get())->Metadata.GroupName, FName("Shared Group"));
	Edit->Metadata.Name = "OtherAmount";
	ASSERT_TRUE(Document.ReplaceExpression(*Edit.Get(), Transactions.Get()));
	ASSERT_EQ(Material->GetParameterDefinitions().size(), 2u);
	const auto OtherId = Material->FindParameterDefinition("OtherAmount")->Id;
	EXPECT_NE(OtherId, Id);
	Edit->Metadata.Name = "Amount";
	Edit->DefaultValue = .1f;
	ASSERT_TRUE(Document.ReplaceExpression(*Edit.Get(), Transactions.Get()));
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 1u);
	EXPECT_FLOAT_EQ(Cast<DMaterialExpressionScalarParameter>(Material->GetExpressionCollection().Expressions[Material->GetExpressionCollection().Expressions.size() - 2].Get())->DefaultValue, .6f);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_NE(Material->FindParameterDefinition(OtherId), nullptr);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(FMaterialGraphOperations::RenameParameter(*Material, Id, "RenamedAmount", Transactions.Get()));
	FResolvedMaterialParameter Resolved;
	ASSERT_TRUE(Instance->ResolveParameterValue(Id, Resolved));
	EXPECT_FLOAT_EQ(Resolved.Value.GetScalar(), .8f);
	EXPECT_EQ(Resolved.Definition->Name, FName("RenamedAmount"));
	ASSERT_TRUE(Document.RemoveNodes(B.GeneratedNodeIds, Transactions.Get()));
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 1u);
	ASSERT_TRUE(Document.RemoveNodes(A.GeneratedNodeIds, Transactions.Get()));
	EXPECT_TRUE(Material->GetParameterDefinitions().empty());
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_NE(Material->FindParameterDefinition(Id), nullptr);
	EXPECT_TRUE(Transactions->Reset());
	MarkAsGarbage(Instance); MarkAsGarbage(Material); CollectGarbage();
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
	ASSERT_TRUE(FMaterialGraphDocument(*Source).Layout());
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
	ASSERT_TRUE(FMaterialGraphDocument(*Source).CopySelection(Ids, Payload));
	ASSERT_TRUE(FMaterialGraphDocument(*Target).Paste(Payload, 0, 0));
	ASSERT_EQ(Target->GetParameterDefinitions().size(), 1u);
	const auto TargetId = Target->GetParameterDefinitions().front().Id;
	EXPECT_NE(TargetId, A->Metadata.Id);
	ASSERT_TRUE(Target->SetParameterValue(TargetId, FMaterialParameterValue::MakeScalar(.9f)));
	ASSERT_TRUE(FMaterialGraphDocument(*Target).Paste(Payload, 100, 0));
	EXPECT_EQ(Target->GetParameterDefinitions().size(), 1u);
	for (const auto& E : Target->GetExpressionCollection().Expressions)
		if (const auto* Parameter = Cast<DMaterialExpressionScalarParameter>(E.Get())) EXPECT_FLOAT_EQ(Parameter->DefaultValue, .9f);
	FMaterialParameterDefinition Conflict;
	Conflict.Name = "Amount"; Conflict.Type = EMaterialParameterType::Vector;
	Conflict.Value = FMaterialParameterValue::MakeVector({1, 1, 1});
	const auto TargetBefore = CaptureExpressions(*Target);
	const auto Rejected = FMaterialGraphOperations::CreateParameter(*Target, Conflict);
	EXPECT_FALSE(Rejected);
	EXPECT_EQ(Rejected.Message.find("A parameter with this name already exists with a different type."), 0u);
	EXPECT_EQ(CaptureExpressions(*Target), TargetBefore);
	Conflict.Name = "IndependentAmount";
	ASSERT_TRUE(FMaterialGraphOperations::CreateParameter(*Target, Conflict));
	EXPECT_EQ(Rejected.Message.find("A parameter with this name already exists with a different type."), 0u);

	const auto* Existing = Cast<DMaterialExpressionParameter>(Target->GetExpressionCollection().Expressions.front().Get());
	ASSERT_NE(Existing, nullptr);
	TStrongObjectPtr<DMaterialExpressionParameter> Invalid(DuplicateObject(Existing, nullptr, NAME_None).Object);
	Invalid->Metadata.Name = NAME_None;
	const auto BeforeInvalid = CaptureExpressions(*Target);
	const auto InvalidResult = FMaterialGraphDocument(*Target).ReplaceExpression(*Invalid.Get());
	EXPECT_FALSE(InvalidResult);
	EXPECT_EQ(InvalidResult.Message.find("The parameter definition is invalid."), 0u);
	EXPECT_EQ(CaptureExpressions(*Target), BeforeInvalid);
	Invalid->Metadata.Name = "ValidRetry";
	ASSERT_TRUE(FMaterialGraphDocument(*Target).ReplaceExpression(*Invalid.Get()));
	MarkAsGarbage(Source); MarkAsGarbage(Target); CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, NarrowPromotionUsesAndReusesFourComponentOwnerThroughMask)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, NAME_None);
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	Durin::Tests::FTestTransactorOwner Transactions;
	const auto Color = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float3);
	ASSERT_TRUE(Color);
	const auto Id = Color.GeneratedNodeIds.front();
	ASSERT_TRUE(Document.SetConstantValue(Id, FMaterialParameterValue::MakeVector({.2, .4, .6})));
	const auto Before = CaptureExpressions(*Material);
	const auto Promoted = FMaterialGraphOperations::PromoteConstantToParameter(*Material, Id, "Tint", Transactions.Get());
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
	const auto UV = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float2);
	ASSERT_TRUE(UV);
	const auto Reused = FMaterialGraphOperations::PromoteConstantToParameter(*Material, UV.GeneratedNodeIds.front(), "Tint");
	ASSERT_TRUE(Reused) << FormatMaterialGraphCommandResult(Reused);
	EXPECT_EQ(Reused.AffectedParameterIds.front(), ParameterId);
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 1u);
	const auto* UVMask = FindExpression<DMaterialExpressionSwizzle>(*Material, UV.GeneratedNodeIds.front());
	ASSERT_NE(UVMask, nullptr);
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
	Durin::Tests::FTestTransactorOwner Transactions;
	FMaterialGraphDocument Document(*Material.Get());
	std::vector<FMaterialGraphChangeSet> Events;
	const auto Handle = Material->GetGraphChanges().Subscribe(*Material.Get(), [&](const auto& Change) {
		Events.push_back(Change);
		for (const auto& Node : Change.Nodes)
			if ((Node.Flags & EMaterialGraphNodeChange::Added) != EMaterialGraphNodeChange::None)
				EXPECT_NE(std::ranges::find(Material->GetMaterialGraphPresentation().Nodes, Node.NodeId,
					&FMaterialGraphNodePresentation::NodeId), Material->GetMaterialGraphPresentation().Nodes.end());
	});
	const auto Created = Testing::CreateGraphConstant(Document, .25f, 123, 456, Transactions.Get());
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
	EXPECT_TRUE(Transactions->Reset());
}

TEST(FMaterialGraphOperationsTests, ReadModelObservesDirectAndReflectedEditsWithoutCompilePolling)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "GraphChangeDirect"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	auto Constant = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
	Constant->Value = .25f;
	const std::array<DMaterialExpression*, 1> Expressions{Constant.Get()};
	ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, {}));
	FMaterialGraphReadModel Model;
	const auto Catalog = FMaterialGraphOperations::EnumerateCatalog();
	EXPECT_TRUE(Model.Refresh(*Material.Get(), Catalog).Has(EMaterialGraphChange::Reset));
	const auto* Storage = Model.GetView().Nodes.data();
	ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, {}));
	EXPECT_TRUE(Model.Refresh(*Material.Get(), Catalog).IsEmpty());
	Constant->Value = .75f;
	ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, {}));
	const auto Change = Model.Refresh(*Material.Get(), Catalog);
	ASSERT_EQ(Change.Nodes.size(), 1u);
	EXPECT_EQ(Change.Nodes[0].Flags, EMaterialGraphNodeChange::Content);
	EXPECT_EQ(Model.GetView().Nodes.data(), Storage);
	EXPECT_FLOAT_EQ(FindViewNode(Model.GetView(), Constant->Id)->Node.GetConstantLiteral().X, .75f);
	auto* Owned = Cast<DMaterialExpressionScalarConstant>(Material->GetExpressionCollection().Expressions[0].Get());
	Owned->Value = .5f;
	Owned->PostEditChangeProperty({.MemberProperty = Owned->GetClass()->FindPropertyByName("Value")});
	EXPECT_FALSE(Model.Refresh(*Material.Get(), Catalog).IsEmpty());
	EXPECT_FLOAT_EQ(FindViewNode(Model.GetView(), Constant->Id)->Node.GetConstantLiteral().X, .5f);
	Owned->PostEditChangeProperty({.MemberProperty = Owned->GetClass()->FindPropertyByName("Value")});
	EXPECT_TRUE(Model.Refresh(*Material.Get(), Catalog).IsEmpty());
	const auto Invalid = Material->SetMaterialExpressions(Expressions, {.Roughness = {FGuid::NewGuid()}});
	EXPECT_FALSE(Invalid);
	EXPECT_TRUE(Model.Refresh(*Material.Get(), Catalog).IsEmpty());
	ASSERT_TRUE(Material->SetStaticProperties(Material->GetStaticProperties()));
	EXPECT_TRUE(Model.Refresh(*Material.Get(), Catalog).IsEmpty());
}

TEST(FMaterialGraphOperationsTests, ReadModelSubscriptionsTrackReplacementDependenciesAndOwnerSwitch)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "GraphObserverMaterial"));
	TStrongObjectPtr<DMaterialFunction> First(NewObject<DMaterialFunction>(nullptr, "GraphObserverFirst"));
	TStrongObjectPtr<DMaterialFunction> Second(NewObject<DMaterialFunction>(nullptr, "GraphObserverSecond"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto Call = FMaterialGraphDocument(*Material.Get()).InsertFunctionCall(*First.Get(), 0, 0);
	ASSERT_TRUE(Call);
	FMaterialGraphReadModel Model;
	const auto Catalog = FMaterialGraphOperations::EnumerateCatalog();
	Model.Refresh(*Material.Get(), Catalog);
	// Emulate the external-reference rewrite performed by committed package replacement.
	auto* CallNode = Cast<DMaterialExpressionFunctionCall>(Material->GetExpressionCollection().Expressions[0].Get());
	ASSERT_NE(CallNode, nullptr);
	CallNode->Function = Second.Get();
	RefreshMaterialGraphObservers();
	EXPECT_FALSE(Model.Refresh(*Material.Get(), Catalog).IsEmpty());
	auto Signature = First->GetFunctionSignature();
	Signature.Outputs[0].Name = "Old dependency";
	ASSERT_TRUE(FMaterialGraphDocument(*First.Get()).SetPort(true, Signature.Outputs[0]));
	EXPECT_TRUE(Model.Refresh(*Material.Get(), Catalog).IsEmpty());
	Signature = Second->GetFunctionSignature();
	Signature.Outputs[0].Name = "New dependency";
	ASSERT_TRUE(FMaterialGraphDocument(*Second.Get()).SetPort(true, Signature.Outputs[0]));
	EXPECT_FALSE(Model.Refresh(*Material.Get(), Catalog).IsEmpty());
	EXPECT_EQ(FindViewNode(Model.GetView(), Call.GeneratedNodeIds[0])->Outputs[0].Name, "New dependency");
	EXPECT_TRUE(Model.Refresh(*First.Get(), Catalog).Has(EMaterialGraphChange::Reset));
	Signature.Outputs[0].Name = "Detached dependency";
	ASSERT_TRUE(FMaterialGraphDocument(*Second.Get()).SetPort(true, Signature.Outputs[0]));
	EXPECT_TRUE(Model.Refresh(*First.Get(), Catalog).IsEmpty());
}

TEST(FMaterialGraphOperationsTests, ReadModelPositionUpdatesAndReentrantPublicationAreCoherent)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "GraphReentrantChanges"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto Created = Testing::CreateGraphConstant(FMaterialGraphDocument(*Material.Get()), .25f);
	ASSERT_TRUE(Created);
	FMaterialGraphReadModel Model;
	const auto Catalog = FMaterialGraphOperations::EnumerateCatalog();
	Model.Refresh(*Material.Get(), Catalog);
	const auto* Storage = Model.GetView().Nodes.data();
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
	const auto CompileRevision = Material->GetMaterialCompileStatus().AuthoredRevision;
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(Position) != Durin::EMaterialGraphPresentationResult::Rejected);
	EXPECT_EQ(Events, 2);
	EXPECT_EQ(MaximumDepth, 1);
	const auto Change = Model.Refresh(*Material.Get(), Catalog);
	ASSERT_EQ(Change.Nodes.size(), 1u);
	EXPECT_EQ(Change.Nodes[0].Flags, EMaterialGraphNodeChange::Position);
	EXPECT_EQ(Model.GetView().Nodes.data(), Storage);
	EXPECT_EQ(FindViewNode(Model.GetView(), Position.Nodes[0].NodeId)->Presentation.X, 222);
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision, CompileRevision);
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

TEST(FMaterialGraphOperationsTests, MaterialOutputUsesStablePinsAndOrdinaryNodeChanges)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "OutputNodeContract"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material.Get());
	Durin::Tests::FTestTransactorOwner Transactions;
	const auto OutputId = Material->GetOutputNode()->Id;
	const auto Created = Testing::CreateGraphConstant(Document, .4f, 0, 0);
	ASSERT_TRUE(Created);
	const auto View = Document.Inspect();
	const auto* Terminal = FindViewNode(View, OutputId);
	ASSERT_NE(Terminal, nullptr);
	EXPECT_TRUE(Terminal->Node.bMaterialOutput);
	EXPECT_TRUE(Terminal->Outputs.empty());
	ASSERT_EQ(Terminal->Inputs.size(), 8u);
	EXPECT_EQ(Terminal->Inputs.front().InputIndex, static_cast<uint32>(EMaterialOutputPin::BaseColor));
	EXPECT_GT(GraphNodeHeight(*Terminal), GraphNodePinOffset(*Terminal) + 7 * FMaterialGraphGeometry::GetMetrics().PinRowHeight);
	EXPECT_TRUE(GetMaterialDomainOutputPins(static_cast<EMaterialDomain>(255)).empty());
	std::vector<FMaterialGraphChangeSet> Events;
	const auto Handle = Material->GetGraphChanges().Subscribe(*Material.Get(), [&](const auto& Change) { Events.push_back(Change); });
	ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::Input(OutputId, static_cast<uint32>(EMaterialOutputPin::Roughness)), FMaterialGraphPinAddress::Output({Created.GeneratedNodeIds.front()}), false, Transactions.Get()));
	EXPECT_EQ(Material->GetOutputNode()->Outputs.Roughness.ExpressionId, Created.GeneratedNodeIds.front());
	EXPECT_FALSE(Material->GetOutputNode()->Outputs.BaseColor.ExpressionId.IsValid());
	ASSERT_EQ(Events.size(), 1u);
	ASSERT_EQ(Events.back().Nodes.size(), 1u);
	EXPECT_EQ(Events.back().Nodes.front().NodeId, OutputId);
	EXPECT_NE(Events.back().Nodes.front().Flags & EMaterialGraphNodeChange::Inputs, EMaterialGraphNodeChange::None);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FALSE(Material->GetOutputNode()->Outputs.Roughness.ExpressionId.IsValid());
	ASSERT_TRUE(Transactions->Redo());
	const auto Revision = Material->GetMaterialCompileStatus().AuthoredRevision;
	const FMaterialGraphNodePresentation Position{OutputId, 600, 120};
	ASSERT_TRUE(FMaterialGraphDocument(*Material.Get()).MoveNodes(std::span(&Position, 1), Transactions.Get()));
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision, Revision);
	ASSERT_EQ(Events.back().Nodes.size(), 1u);
	EXPECT_EQ(Events.back().Nodes.front().Flags, EMaterialGraphNodeChange::Position);
	const FMaterialInputDefault Default{.Kind = EMaterialInputDefaultKind::Literal,
		.Type = EMaterialProgramValueType::Float, .Literal = {.X = .7f}};
	ASSERT_TRUE(Document.SetInputDefault(OutputId, static_cast<uint32>(EMaterialOutputPin::Metallic), Default, {}, Transactions.Get()));
	EXPECT_FLOAT_EQ(Material->GetOutputNode()->Outputs.MetallicDefault, .7f);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FLOAT_EQ(Material->GetOutputNode()->Outputs.MetallicDefault, 0.f);
	ASSERT_TRUE(Transactions->Redo());
	const auto Extracted = Document.ExtractInputDefault(OutputId, static_cast<uint32>(EMaterialOutputPin::Metallic), {}, Transactions.Get());
	ASSERT_TRUE(Extracted) << FormatMaterialGraphCommandResult(Extracted);
	ASSERT_TRUE(Document.InlineInputNode(OutputId, static_cast<uint32>(EMaterialOutputPin::Metallic), {}, Transactions.Get()));
	EXPECT_FLOAT_EQ(Material->GetOutputNode()->Outputs.MetallicDefault, .7f);
	EXPECT_FALSE(Document.SetInputDefault(OutputId, 256, Default));
	const auto BeforeRejected = Events.size();
	EXPECT_FALSE(Document.RemoveNodes(std::span(&OutputId, 1), Transactions.Get()));
	FMaterialGraphClipboardPayload Clipboard;
	EXPECT_FALSE(Document.CopySelection(std::span(&OutputId, 1), Clipboard));
	{
		GraphEditInternals::FGraphEditSession Missing(*Material);
		std::erase_if(Missing.Expressions, [&](const auto& E) { return E->Id == OutputId; });
		EXPECT_FALSE(Missing.Commit("Reject missing output", nullptr));
	}
	{
		GraphEditInternals::FGraphEditSession Duplicate(*Material);
		auto Extra = Testing::MakeGraphExpression<DMaterialExpressionMaterialOutput>();
		Duplicate.Expressions.emplace_back(Extra.Get());
		EXPECT_FALSE(Duplicate.Commit("Reject duplicate output", nullptr));
	}
	EXPECT_EQ(Events.size(), BeforeRejected);
	EXPECT_EQ(Material->GetOutputNode()->Id, OutputId);
	Material->GetGraphChanges().Unsubscribe(Handle);
	EXPECT_TRUE(Transactions->Reset());
}

TEST(FMaterialGraphOperationsTests, ConnectionsAndReplayPreserveObjectsWithoutGraphCopies)
{
	InitializeDObjectSystem();
	std::optional<size_t> SmallHistoryBytes;
	for (const uint32 UnrelatedCount : {0u, 128u})
	{
		TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		FMaterialGraphDocument Document(*Material);
		const auto Source = Testing::CreateGraphConstant(Document, .5f);
		const auto Destination = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::Add, EMaterialProgramValueType::Float);
		ASSERT_TRUE(Source); ASSERT_TRUE(Destination);
		std::vector<DMaterialExpression*> Nodes;
		for (auto& E : Material->GetExpressionCollection().Expressions) Nodes.push_back(E.Get());
		for (uint32 Index = 0; Index < UnrelatedCount; ++Index)
		{
			auto* E = NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None);
			E->Id = FGuid::NewGuid(); E->Value = float(Index);
			Nodes.push_back(E);
		}
		ASSERT_TRUE(Material->SetMaterialExpressions(Nodes));
		const auto Original = Material->GetExpressionCollection().Expressions;
		Durin::Tests::FTestTransactorOwner Transactions;
		const auto ObjectCount = GDObjectArray.GetNum();
		ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::Input(Destination.GeneratedNodeIds.front(), 0), FMaterialGraphPinAddress::Output({Source.GeneratedNodeIds.front()}), false, Transactions.Get()));
		EXPECT_EQ(Material->GetExpressionCollection().Expressions, Original);
		EXPECT_EQ(GDObjectArray.GetNum(), ObjectCount);
		const auto HistoryBytes = Transactions->GetOwnedBytes();
		if (SmallHistoryBytes) EXPECT_EQ(HistoryBytes, *SmallHistoryBytes);
		else SmallHistoryBytes = HistoryBytes;
		EXPECT_EQ(Document.Connect(FMaterialGraphPinAddress::Input(Destination.GeneratedNodeIds.front(), 0), FMaterialGraphPinAddress::Output({Source.GeneratedNodeIds.front()}), false, Transactions.Get()).GetStatus(),
			EMaterialGraphCommandStatus::NoChange);
		EXPECT_EQ(Transactions->GetOwnedBytes(), HistoryBytes);
		ASSERT_TRUE(Transactions->Undo());
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(Material->GetExpressionCollection().Expressions, Original);
		EXPECT_EQ(GDObjectArray.GetNum(), ObjectCount);
		ASSERT_TRUE(Document.Disconnect(FMaterialGraphPinAddress::Input(Destination.GeneratedNodeIds.front(), 0)));
		EXPECT_EQ(Material->GetExpressionCollection().Expressions, Original);
		EXPECT_EQ(GDObjectArray.GetNum(), ObjectCount);
	}
}

TEST(FMaterialGraphOperationsTests, InvalidTypesRemainEditableAndUndoRestoresDiagnostics)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	const auto Vector = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float2);
	const auto Scalar = Testing::CreateGraphConstant(Document, .5f);
	ASSERT_TRUE(Vector); ASSERT_TRUE(Scalar);
	const auto Original = Material->GetExpressionCollection().Expressions;
	Durin::Tests::FTestTransactorOwner Transactions;
	ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::MaterialOutput(Material->GetOutputNode()->Id, EMaterialSurfaceOutput::Roughness), FMaterialGraphPinAddress::Output({Vector.GeneratedNodeIds.front()}), true, Transactions.Get()));
	EXPECT_TRUE(HasGraphDiagnostics(*Material));
	ASSERT_TRUE(Document.SetConstantValue(Scalar.GeneratedNodeIds.front(), FMaterialParameterValue::MakeScalar(.75f), Transactions.Get()));
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

TEST(FMaterialGraphOperationsTests, BusyTransactorLeavesLiveObjectsAndNotificationsUnchanged)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	const auto Source = Testing::CreateGraphConstant(Document, .5f);
	const auto Destination = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::Add, EMaterialProgramValueType::Float);
	ASSERT_TRUE(Source); ASSERT_TRUE(Destination);
	const auto Before = CaptureExpressions(*Material);
	const auto Original = Material->GetExpressionCollection().Expressions;
	const auto Revision = Material->GetMaterialProgramRevision();
	uint32 Notifications = 0;
	const auto Observer = Material->GetGraphChanges().Subscribe(*Material, [&](const auto&) { ++Notifications; });
	Durin::Tests::FTestTransactorOwner Transactions;
	const auto Pending = Transactions->Begin({.Description = "Other operation"});
	ASSERT_TRUE(Pending);
	EXPECT_FALSE(Document.Connect(FMaterialGraphPinAddress::Input(Destination.GeneratedNodeIds.front(), 0), FMaterialGraphPinAddress::Output({Source.GeneratedNodeIds.front()}), false, Transactions.Get()));
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_EQ(Material->GetExpressionCollection().Expressions, Original);
	EXPECT_EQ(Material->GetMaterialProgramRevision(), Revision);
	EXPECT_EQ(Notifications, 0u);
	EXPECT_EQ(Transactions->GetHistoryCount(), 0u);
	(void)Transactions->Cancel(Pending.ScopeId);
	Material->GetGraphChanges().Unsubscribe(Observer);
}

TEST(FMaterialGraphOperationsTests, ReplayRejectsParticipantsReplacedOutsideHistory)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	const auto Created = Testing::CreateGraphConstant(Document, .25f);
	ASSERT_TRUE(Created);
	Durin::Tests::FTestTransactorOwner Transactions;
	ASSERT_TRUE(Document.SetConstantValue(Created.GeneratedNodeIds.front(), FMaterialParameterValue::MakeScalar(.5f), Transactions.Get()));
	std::vector<DMaterialExpression*> Nodes;
	for (auto& E : Material->GetExpressionCollection().Expressions) Nodes.push_back(E.Get());
	ASSERT_TRUE(Material->SetMaterialExpressions(Nodes));
	const auto Before = CaptureExpressions(*Material);
	const auto Revision = Material->GetMaterialProgramRevision();
	const auto Failed = Transactions->Undo();
	ASSERT_FALSE(Failed);
	ASSERT_TRUE(Failed.ApplyCause);
	ASSERT_TRUE(Failed.ApplyCause->Error.RecordCause.CustomCause);
	EXPECT_EQ(Failed.ApplyCause->Error.RecordCause.CustomCause->Code, ETransactionCustomError::MembershipChanged);
	EXPECT_EQ(Failed.ApplyCause->Error.RecordCause.CustomCause->NodeCount, Nodes.size());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_EQ(Material->GetMaterialProgramRevision(), Revision);
	EXPECT_TRUE(Transactions->CanUndo());
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
	Add->A = {Scalar->Id}; Multiply->A = {Add->Id};
	{
		GraphEditInternals::FGraphEditSession Setup(*Material);
		for (auto* E : std::array<DMaterialExpression*, 5>{Scalar.Get(), Vector.Get(), OtherVector.Get(), Add.Get(), Multiply.Get()})
			Setup.Expressions.emplace_back(E);
		for (uint32 I = 0; I < 64; ++I)
			Setup.Expressions.emplace_back(Testing::MakeGraphExpression<DMaterialExpressionAdd>().Get());
		ASSERT_TRUE(Setup.Commit("Build branches", nullptr));
	}
	Tests::FTestTransactorOwner Transactions;
	{
		GraphEditInternals::FGraphEditSession Edit(*Material);
		Edit.Modify(*Scalar); Scalar->Value = .25f;
		ASSERT_TRUE(Edit.Commit("Change scalar value", Transactions.Get()));
		EXPECT_EQ(Edit.InferredNumericNodes, 0u);
	}
	{
		GraphEditInternals::FGraphEditSession Edit(*Material);
		Edit.Modify(*Add); Add->A = {Vector->Id};
		ASSERT_TRUE(Edit.Commit("Connect vector", Transactions.Get()));
		EXPECT_EQ(Edit.InferredNumericNodes, 2u);
		EXPECT_EQ(Add->ResultType, EMaterialProgramValueType::Float3);
		EXPECT_EQ(Multiply->ResultType, EMaterialProgramValueType::Float3);
	}
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Add->ResultType, EMaterialProgramValueType::Float);
	EXPECT_EQ(Multiply->ResultType, EMaterialProgramValueType::Float);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Multiply->ResultType, EMaterialProgramValueType::Float3);
	{
		GraphEditInternals::FGraphEditSession Edit(*Material);
		Edit.Modify(*Add); Add->A = {OtherVector->Id};
		ASSERT_TRUE(Edit.Commit("Reconnect same width", Transactions.Get()));
		EXPECT_EQ(Edit.InferredNumericNodes, 1u);
		EXPECT_EQ(Multiply->ResultType, EMaterialProgramValueType::Float3);
	}
	FMaterialGraphDocument Document(*Material);
	ASSERT_TRUE(Document.SetConstantValue(OtherVector->Id, FMaterialParameterValue::MakeVector2({.2f, .3f}), Transactions.Get()));
	EXPECT_EQ(Add->ResultType, EMaterialProgramValueType::Float2);
	EXPECT_EQ(Multiply->ResultType, EMaterialProgramValueType::Float2);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Multiply->ResultType, EMaterialProgramValueType::Float3);
	{
		// A same-class draft may explicitly author the result width before inference.
		GraphEditInternals::FGraphEditSession Edit(*Material);
		Edit.Modify(*Add); Add->A = {}; Add->ResultType = EMaterialProgramValueType::Float4;
		ASSERT_TRUE(Edit.Commit("Author unconstrained width", Transactions.Get()));
		EXPECT_EQ(Edit.InferredNumericNodes, 2u);
		EXPECT_EQ(Multiply->ResultType, EMaterialProgramValueType::Float4);
	}
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Multiply->ResultType, EMaterialProgramValueType::Float3);
}

TEST(FMaterialGraphOperationsTests, ExtractOutputDefaultPreservesMaterialAttributesConnection)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "ExtractOutputDefault"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	auto Graph = Testing::MakePBRMaterialExpressionsForTest();
	auto Packed = Testing::MakeGraphExpression<DMaterialExpressionMakeSurface>();
	Packed->BaseColor = Graph.Outputs.BaseColor; Packed->Normal = Graph.Outputs.Normal;
	Packed->Metallic = Graph.Outputs.Metallic; Packed->Roughness = Graph.Outputs.Roughness;
	Packed->AmbientOcclusion = Graph.Outputs.AmbientOcclusion; Packed->Emissive = Graph.Outputs.Emissive;
	Packed->Opacity = Graph.Outputs.Opacity; Packed->OpacityMask = Graph.Outputs.OpacityMask;
	Graph.Expressions.emplace_back(Packed.Get());
	Graph.Outputs.Surface = {Packed->Id};
	Graph.Outputs.Metallic = {};
	Graph.Outputs.MetallicDefault = .7f;
	ASSERT_TRUE(Graph.Apply(*Material));
	const FMaterialGraphNodePresentation Position{Material->GetOutputNode()->Id, 600, 120};
	ASSERT_TRUE(FMaterialGraphDocument(*Material).MoveNodes(std::span(&Position, 1)));
	FMaterialGraphDocument Document(*Material);
	Durin::Tests::FTestTransactorOwner Transactions;
	const auto Before = Material->GetExpressionOutputs();
	const auto Extracted = Document.ExtractInputDefault(Material->GetOutputNode()->Id,
		static_cast<uint32>(EMaterialOutputPin::Metallic), {}, Transactions.Get());
	ASSERT_TRUE(Extracted) << FormatMaterialGraphCommandResult(Extracted);
	ASSERT_EQ(Extracted.GeneratedNodeIds.size(), 1u);
	const auto After = Material->GetExpressionOutputs();
	auto Expected = Before;
	Expected.Metallic = {Extracted.GeneratedNodeIds.front()};
	EXPECT_EQ(After, Expected);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->GetExpressionOutputs(), Before);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Material->GetExpressionOutputs(), Expected);
	ASSERT_TRUE(Document.SetUseMaterialAttributes(true));
	EXPECT_EQ(Material->GetExpressionOutputs().Surface, Before.Surface);
	std::vector<DMaterialExpression*> Expressions;
	for (const auto& Expression : Material->GetExpressionCollection().Expressions) Expressions.push_back(Expression.Get());
	const auto Built = MIR::FGraphBuilder(Expressions).FinishSurface(Material->GetExpressionOutputs());
	ASSERT_TRUE(Built);
	EXPECT_TRUE(Built.IR.SurfaceRoot.bAggregate);
}

TEST(FMaterialGraphOperationsTests, OutputModeRetainsConnectionsAndUndoRestoresVisiblePins)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "OutputModes");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	auto Graph = Testing::MakePBRMaterialExpressionsForTest();
	auto Packed = Testing::MakeGraphExpression<DMaterialExpressionMakeSurface>();
	Packed->BaseColor = Graph.Outputs.BaseColor; Packed->Normal = Graph.Outputs.Normal;
	Packed->Metallic = Graph.Outputs.Metallic; Packed->Roughness = Graph.Outputs.Roughness;
	Packed->AmbientOcclusion = Graph.Outputs.AmbientOcclusion; Packed->Emissive = Graph.Outputs.Emissive;
	Packed->Opacity = Graph.Outputs.Opacity; Packed->OpacityMask = Graph.Outputs.OpacityMask;
	Graph.Expressions.emplace_back(Packed.Get());
	Graph.Outputs.Surface = {Packed->Id};
	ASSERT_TRUE(Graph.Apply(*Material));
	FMaterialGraphDocument Document(*Material);
	Durin::Tests::FTestTransactorOwner Transactions;
	const auto Before = Material->GetExpressionOutputs();
	const auto Pins = [&] {
		auto View = Document.Inspect();
		return std::ranges::find_if(View.Nodes, [](const auto& Node) { return Node.Node.bMaterialOutput; })->Inputs;
	};
	ASSERT_EQ(Pins().size(), 8u);
	ASSERT_TRUE(Document.SetUseMaterialAttributes(true, Transactions.Get()));
	ASSERT_EQ(Pins().size(), 1u);
	EXPECT_EQ(Pins()[0].Name, "Material Attributes");
	EXPECT_EQ(Material->GetExpressionOutputs().BaseColor, Before.BaseColor);
	EXPECT_EQ(Material->GetExpressionOutputs().Surface, Before.Surface);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->GetExpressionOutputs(), Before);
	EXPECT_EQ(Pins().size(), 8u);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Pins().size(), 1u);
	// Editing either stored connection set never switches the mode or clears the other.
	ASSERT_TRUE(Document.Disconnect(FMaterialGraphPinAddress::MaterialOutput(Material->GetOutputNode()->Id, EMaterialSurfaceOutput::BaseColor)));
	EXPECT_TRUE(Material->GetExpressionOutputs().bUseMaterialAttributes);
	EXPECT_EQ(Material->GetExpressionOutputs().Surface, Before.Surface);
	ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::MaterialOutput(Material->GetOutputNode()->Id, EMaterialSurfaceOutput::BaseColor), FMaterialGraphPinAddress::Output({Before.BaseColor.ExpressionId, Before.BaseColor.OutputIndex, Before.BaseColor.OutputId}), true));
	ASSERT_TRUE(Document.Disconnect(FMaterialGraphPinAddress::MaterialOutput(Material->GetOutputNode()->Id, std::nullopt)));
	EXPECT_EQ(Material->GetExpressionOutputs().BaseColor, Before.BaseColor);
	// An empty packed input uses standard defaults, independently of retained individual values.
	std::vector<DMaterialExpression*> Expressions;
	for (const auto& Expression : Material->GetExpressionCollection().Expressions) Expressions.push_back(Expression.Get());
	const auto Built = MIR::FGraphBuilder(Expressions).FinishSurface(Material->GetExpressionOutputs());
	ASSERT_TRUE(Built);
	EXPECT_FALSE(Built.IR.SurfaceRoot.Inputs[0].bExpression);
	EXPECT_EQ(Built.IR.SurfaceRoot.Inputs[0].Literal.X, 0.5f);
	ASSERT_TRUE(Document.SetUseMaterialAttributes(false));
	auto Properties = Material->GetStaticProperties();
	Properties.ShadingModel = EMaterialShadingModel::Unlit;
	Properties.BlendMode = EMaterialBlendMode::Masked;
	ASSERT_TRUE(Material->SetStaticProperties(Properties));
	const auto Visible = Pins();
	EXPECT_FALSE(Visible[1].bActive);
	EXPECT_FALSE(Visible[6].bActive);
	EXPECT_TRUE(Visible[7].bActive);
	EXPECT_EQ(Material->GetExpressionOutputs().BaseColor, Before.BaseColor);
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, CreationRequestConnectsAndUndoesAsOneEdit)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	const auto Scalar = Testing::CreateGraphConstant(Document, .25f);
	ASSERT_TRUE(Scalar);
	const auto Catalog = FMaterialGraphOperations::EnumerateCatalog();
	const auto Entry = std::ranges::find_if(Catalog, [](const auto& E) {
		return E.Opcode == EMaterialProgramOpcode::Add && E.ResultType == EMaterialProgramValueType::Float;
	});
	ASSERT_NE(Entry, Catalog.end());
	Durin::Tests::FTestTransactorOwner Transactions;
	const auto Before = CaptureExpressions(*Material);
	const auto Revision = Material->GetMaterialCompileStatus().AuthoredRevision;
	const auto Action = MakeCreationAction(*Entry);
	EXPECT_TRUE(Document.CanCreate(Action, EMaterialProgramValueType::Float));
	EXPECT_FALSE(Document.CanCreate(Action, EMaterialProgramValueType::Surface));
	const auto Created = Document.Create({Action, 120, 240,
		FMaterialGraphPinAddress::Output({Scalar.GeneratedNodeIds.front()})}, Transactions.Get());
	ASSERT_TRUE(Created) << FormatMaterialGraphCommandResult(Created);
	const auto View = Document.Inspect();
	const auto* Node = FindViewNode(View, Created.GeneratedNodeIds.front());
	ASSERT_NE(Node, nullptr);
	EXPECT_EQ(Node->Inputs.front().Link.SourceNodeId, Scalar.GeneratedNodeIds.front());
	EXPECT_EQ(Node->Presentation.X, 120);
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision, Revision + 1);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_FALSE(Transactions->CanUndo());
	ASSERT_TRUE(Transactions->Redo());
	const auto After = CaptureExpressions(*Material);
	ASSERT_TRUE(Transactions->Reset());
	EXPECT_FALSE(Document.Create({Action, 0, 0, FMaterialGraphPinAddress::Output({FGuid::NewGuid()})}, Transactions.Get()));
	EXPECT_EQ(CaptureExpressions(*Material), After);
	EXPECT_FALSE(Transactions->CanUndo());
}

TEST(FMaterialGraphOperationsTests, PinAddressesPreserveDefaultsAndRejectReplacementWithoutHistory)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	const auto A = Testing::CreateGraphConstant(Document, .2f);
	const auto B = Testing::CreateGraphConstant(Document, .8f);
	ASSERT_TRUE(A); ASSERT_TRUE(B);
	const auto View = Document.Inspect();
	const auto Output = std::ranges::find_if(View.Nodes, [](const auto& N) { return N.Node.bMaterialOutput; });
	ASSERT_NE(Output, View.Nodes.end());
	const FMaterialGraphPinAddress Target{Output->Node.Id, EMaterialGraphPinKind::MaterialAttribute,
		static_cast<uint32>(EMaterialSurfaceOutput::Roughness)};
	const auto Default = Material->GetExpressionOutputs().RoughnessDefault;
	ASSERT_TRUE(Document.Connect(Target, FMaterialGraphPinAddress::Output({A.GeneratedNodeIds[0]})));
	Durin::Tests::FTestTransactorOwner Transactions;
	EXPECT_FALSE(Document.Connect(Target, FMaterialGraphPinAddress::Output({B.GeneratedNodeIds[0]}), false, Transactions.Get()));
	EXPECT_EQ(Document.Connect(Target, FMaterialGraphPinAddress::Output({A.GeneratedNodeIds[0]}), false, Transactions.Get()).GetStatus(),
		EMaterialGraphCommandStatus::NoChange);
	EXPECT_FALSE(Transactions->CanUndo());
	const auto Replaced = Document.Connect(Target, FMaterialGraphPinAddress::Output({B.GeneratedNodeIds[0]}), true, Transactions.Get());
	ASSERT_TRUE(Replaced);
	std::vector ExpectedIds{Target.NodeId, A.GeneratedNodeIds[0], B.GeneratedNodeIds[0]};
	std::ranges::sort(ExpectedIds);
	EXPECT_EQ(Replaced.AffectedNodeIds, ExpectedIds);
	EXPECT_TRUE(Replaced.GeneratedNodeIds.empty());
	const auto Unchanged = Document.Connect(Target, FMaterialGraphPinAddress::Output({B.GeneratedNodeIds[0]}), true, Transactions.Get());
	EXPECT_EQ(Unchanged.GetStatus(), EMaterialGraphCommandStatus::NoChange);
	EXPECT_TRUE(Unchanged.AffectedNodeIds.empty());
	EXPECT_EQ(Material->GetExpressionOutputs().RoughnessDefault, Default);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->GetExpressionOutputs().Roughness.ExpressionId, A.GeneratedNodeIds[0]);
	const auto Disconnected = Document.Disconnect(Target, Transactions.Get());
	ASSERT_TRUE(Disconnected);
	ExpectedIds = {Target.NodeId, A.GeneratedNodeIds[0]};
	std::ranges::sort(ExpectedIds);
	EXPECT_EQ(Disconnected.AffectedNodeIds, ExpectedIds);
	EXPECT_FALSE(Material->GetExpressionOutputs().Roughness.ExpressionId.IsValid());
	EXPECT_EQ(Material->GetExpressionOutputs().RoughnessDefault, Default);
	EXPECT_FALSE(Document.Connect(Target, {A.GeneratedNodeIds[0], EMaterialGraphPinKind::Input}, true));
}

TEST(FMaterialGraphOperationsTests, SharedMoveDraftPreservesIndependentPresentationEdits)
{
	InitializeDObjectSystem();
	for (const bool bFunction : {false, true})
	{
		SCOPED_TRACE(bFunction);
		TStrongObjectPtr<DObject> Owner(bFunction ? static_cast<DObject*>(NewObject<DMaterialFunction>(nullptr, NAME_None))
			: static_cast<DObject*>(NewObject<DMaterial>(nullptr, NAME_None)));
		FMaterialGraphDocument Document(*Owner.Get());
		const auto Created = Testing::CreateGraphConstant(Document, .5f, 40, 80);
		const auto Other = Testing::CreateGraphConstant(Document, .7f, 200, 80);
		ASSERT_TRUE(Created); ASSERT_TRUE(Other);
		const auto Id = Created.GeneratedNodeIds.front(), OtherId = Other.GeneratedNodeIds.front();
		const auto Read = [&] { return GraphEditInternals::ReadGraphPresentation(*Owner.Get()); };
		const auto Write = [&](FMaterialGraphPresentation P) {
			ASSERT_NE(GraphEditInternals::WriteGraphPresentation(*Owner.Get(), std::move(P)), EMaterialGraphPresentationResult::Rejected);
		};
		auto Initial = Read();
		std::ranges::find(Initial.Nodes, Id, &FMaterialGraphNodePresentation::NodeId)->DisplayName = "Original";
		Write(Initial);
		Durin::Tests::FTestTransactorOwner Transactions;
		FMaterialGraphMoveSession Move;
		ASSERT_TRUE(Move.Begin(*Owner.Get(), std::array{Id}, Transactions.Get()));
		ASSERT_TRUE(Move.Apply(std::array{FMaterialGraphNodePresentation{Id, 400, 240, "Ignored"}}));
		EXPECT_EQ(Read(), Initial); // Preview cannot dirty or publish the owner.
		EXPECT_EQ(Transactions->GetUndoCount(), 0u);
		auto External = Read();
		std::ranges::find(External.Nodes, Id, &FMaterialGraphNodePresentation::NodeId)->DisplayName = "Renamed";
		std::ranges::find(External.Nodes, OtherId, &FMaterialGraphNodePresentation::NodeId)->X = 900;
		Write(External);
		ASSERT_TRUE(Move.Cancel());
		EXPECT_EQ(Read(), External);
		ASSERT_TRUE(Move.Begin(*Owner.Get(), std::array{Id}, Transactions.Get()));
		ASSERT_TRUE(Move.Apply(std::array{FMaterialGraphNodePresentation{Id, 400, 240}}));
		ASSERT_TRUE(Move.Commit());
		EXPECT_EQ(Transactions->GetUndoCount(), 1u);
		EXPECT_EQ(FindViewNode(Document.Inspect(), Id)->Presentation.DisplayName, "Renamed");
		auto Later = Read();
		std::ranges::find(Later.Nodes, Id, &FMaterialGraphNodePresentation::NodeId)->DisplayName = "After move";
		std::ranges::find(Later.Nodes, OtherId, &FMaterialGraphNodePresentation::NodeId)->X = 1100;
		Write(Later);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(FindViewNode(Document.Inspect(), Id)->Presentation.X, 40);
		EXPECT_EQ(FindViewNode(Document.Inspect(), Id)->Presentation.DisplayName, "After move");
		EXPECT_EQ(FindViewNode(Document.Inspect(), OtherId)->Presentation.X, 1100);
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(Read(), Later);
		ASSERT_TRUE(Transactions->Reset());
		ASSERT_TRUE(Move.Begin(*Owner.Get(), std::array{Id}, Transactions.Get()));
		ASSERT_TRUE(Move.Apply(std::array{FMaterialGraphNodePresentation{Id, 400, 240}}));
		EXPECT_EQ(Move.Commit().GetStatus(), EMaterialGraphCommandStatus::NoChange);
		EXPECT_EQ(Transactions->GetUndoCount(), 0u);
		// Direct position commands must also ignore the presentation label argument.
		ASSERT_TRUE(Document.MoveNodes(std::array{FMaterialGraphNodePresentation{Id, 450, 240, "Ignored"}}, Transactions.Get()));
		EXPECT_EQ(FindViewNode(Document.Inspect(), Id)->Presentation.DisplayName, "After move");
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(Read(), Later);
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
		FMaterialGraphDocument Document(*Owner.Get());
		const auto Created = Testing::CreateGraphConstant(Document, .5f, 40, 80);
		ASSERT_TRUE(Created);
		const auto Id = Created.GeneratedNodeIds.front();
		Durin::Tests::FTestTransactorOwner Transactions;
		FMaterialGraphMoveSession Move;
		ASSERT_TRUE(Move.Begin(*Owner.Get(), std::array{Id}, Transactions.Get()));
		const FMaterialGraphNodePresentation Position{Id, 400, 240};
		ASSERT_TRUE(Move.Apply(std::array{Position}));
		const auto Duplicate = Move.Apply(std::array{Position, Position});
		EXPECT_FALSE(Duplicate);
		EXPECT_EQ(Duplicate.Message.find("The graph move preview contains a duplicate node GUID."), 0u);
		const auto OutsideId = FGuid::NewGuid();
		const auto Outside = Move.Apply(std::array{FMaterialGraphNodePresentation{OutsideId, 0, 0}});
		EXPECT_FALSE(Outside);
		EXPECT_EQ(Outside.Message.find("The graph move preview addresses a node outside the selection."), 0u);
		const auto Coordinate = Move.Apply(std::array{FMaterialGraphNodePresentation{Id, MaterialGraphPresentationCoordinateLimit + 1, 0}});
		EXPECT_FALSE(Coordinate);
		EXPECT_EQ(Coordinate.Message.find("The graph move preview is outside the supported coordinate range."), 0u);
		ASSERT_EQ(Move.GetPositions().size(), 1u);
		EXPECT_EQ(Move.GetPositions().front().X, 400);
		ASSERT_TRUE(Document.SetConstantValue(Id, FMaterialParameterValue::MakeScalar(.8f)));
		const auto Stale = Move.Commit();
		EXPECT_FALSE(Stale);
		EXPECT_EQ(Stale.Message.find("The graph changed semantically during the move."), 0u);
		EXPECT_FALSE(Move.IsActive());
		EXPECT_EQ(Transactions->GetUndoCount(), 0u);
		EXPECT_EQ(FindViewNode(Document.Inspect(), Id)->Presentation.X, 40);
		// A node without an authored presentation can still be moved and undone.
		auto P = GraphEditInternals::ReadGraphPresentation(*Owner.Get());
		std::erase_if(P.Nodes, [&](const auto& N) { return N.NodeId == Id; });
		ASSERT_NE(GraphEditInternals::WriteGraphPresentation(*Owner.Get(), P), EMaterialGraphPresentationResult::Rejected);
		ASSERT_TRUE(Move.Begin(*Owner.Get(), std::array{Id}, Transactions.Get()));
		ASSERT_TRUE(Move.Apply(std::array{Position})); ASSERT_TRUE(Move.Commit());
		EXPECT_EQ(FindViewNode(Document.Inspect(), Id)->Presentation.X, 400);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(GraphEditInternals::ReadGraphPresentation(*Owner.Get()), P);
		const auto Fallback = FindViewNode(Document.Inspect(), Id)->Presentation;
		ASSERT_TRUE(Transactions->Redo());
		auto Labeled = GraphEditInternals::ReadGraphPresentation(*Owner.Get());
		std::ranges::find(Labeled.Nodes, Id, &FMaterialGraphNodePresentation::NodeId)->DisplayName = "New label";
		ASSERT_NE(GraphEditInternals::WriteGraphPresentation(*Owner.Get(), Labeled), EMaterialGraphPresentationResult::Rejected);
		ASSERT_TRUE(Transactions->Undo());
		const auto Restored = FindViewNode(Document.Inspect(), Id)->Presentation;
		EXPECT_EQ(Restored.X, Fallback.X); EXPECT_EQ(Restored.Y, Fallback.Y);
		EXPECT_EQ(Restored.DisplayName, "New label");
	}
}

TEST(FMaterialGraphOperationsTests, FunctionLayoutUsesPositionHistoryAndPreservesLabels)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, NAME_None));
	FMaterialGraphDocument Document(*Function.Get());
	const auto First = Testing::CreateGraphConstant(Document, .5f, 0, 0);
	const auto Second = Testing::CreateGraphConstant(Document, .7f, 0, 0);
	ASSERT_TRUE(First); ASSERT_TRUE(Second);
	auto Original = Function->GetFunctionPresentation();
	Original.Nodes.front().DisplayName = "Keep label";
	ASSERT_TRUE(Function->SetFunctionPresentation(Original));
	const auto Revision = Function->GetFunctionRevision();
	FMaterialGraphPresentation A, B;
	ASSERT_TRUE(Document.CalculateLayout({}, A)); ASSERT_TRUE(Document.CalculateLayout({}, B));
	EXPECT_EQ(A, B);
	EXPECT_EQ(Function->GetFunctionPresentation(), Original);
	Durin::Tests::FTestTransactorOwner Transactions;
	ASSERT_TRUE(Document.Layout({}, Transactions.Get()));
	EXPECT_EQ(Function->GetFunctionPresentation().Nodes, A.Nodes);
	ASSERT_TRUE(Transactions->Undo()); EXPECT_EQ(Function->GetFunctionPresentation(), Original);
	ASSERT_TRUE(Transactions->Redo()); EXPECT_EQ(Function->GetFunctionPresentation().Nodes, A.Nodes);
	EXPECT_EQ(Function->GetFunctionRevision(), Revision);
}

TEST(FMaterialGraphOperationsTests, MoveHistorySizeDoesNotGrowWithUnchangedNodes)
{
	InitializeDObjectSystem();
	for (const bool bFunction : {false, true})
	{
		SCOPED_TRACE(bFunction);
		TStrongObjectPtr<DObject> Owner(bFunction ? static_cast<DObject*>(NewObject<DMaterialFunction>(nullptr, NAME_None))
			: static_cast<DObject*>(NewObject<DMaterial>(nullptr, NAME_None)));
		FMaterialGraphDocument Document(*Owner.Get());
		const auto Created = Testing::CreateGraphConstant(Document, .5f, 40, 80);
		ASSERT_TRUE(Created);
		const auto Id = Created.GeneratedNodeIds.front();
		Durin::Tests::FTestTransactorOwner Transactions;
		FMaterialGraphMoveSession Move;
		ASSERT_TRUE(Move.Begin(*Owner.Get(), std::array{Id}, Transactions.Get()));
		ASSERT_TRUE(Move.Apply(std::array{FMaterialGraphNodePresentation{Id, 400, 240}}));
		ASSERT_TRUE(Move.Commit());
		const auto SmallBytes = Transactions->GetOwnedBytes();
		ASSERT_TRUE(Transactions->Reset());
		for (int Index = 0; Index < 32; ++Index)
			ASSERT_TRUE(Testing::CreateGraphConstant(Document, .7f, Index * 100, 500));
		ASSERT_TRUE(Move.Begin(*Owner.Get(), std::array{Id}, Transactions.Get()));
		ASSERT_TRUE(Move.Apply(std::array{FMaterialGraphNodePresentation{Id, 600, 240}}));
		ASSERT_TRUE(Move.Commit());
		EXPECT_EQ(Transactions->GetOwnedBytes(), SmallBytes);
	}
}

TEST(FMaterialGraphOperationsTests, GraphSessionRetainsHistoryFailureAcrossRollbackAndRetry)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material.Get());
	const auto Created = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float);
	ASSERT_TRUE(Created);
	const auto NodeId = Created.GeneratedNodeIds.front();
	const auto Before = CaptureExpressions(*Material.Get());
	const auto Revision = Material->GetMaterialCompileStatus().AuthoredRevision;
	Durin::Tests::FTestTransactorOwner Transactions;
	ASSERT_TRUE(Transactions->SetLimits({.MaximumEntries = 256, .MaximumOwnedBytes = 1}));
	const auto Rejected = Document.SetConstantValue(NodeId, FMaterialParameterValue::MakeScalar(.75f), Transactions.Get());
	EXPECT_FALSE(Rejected);
	EXPECT_EQ(Rejected.GetStatus(), EMaterialGraphCommandStatus::Rejected);
	EXPECT_NE(Rejected.Message.find("The transaction exceeded the owned-byte limit."), std::string::npos);
	EXPECT_EQ(Rejected.Message.find("Unable to record the graph edit."), 0u);
	EXPECT_EQ(CaptureExpressions(*Material.Get()), Before);
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision, Revision);
	EXPECT_FALSE(Transactions->CanUndo());
	ASSERT_TRUE(Transactions->SetLimits({}));
	ASSERT_TRUE(Document.SetConstantValue(NodeId, FMaterialParameterValue::MakeScalar(.75f), Transactions.Get()));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material.Get()), Before);

	EXPECT_NE(Rejected.Message.find("The transaction exceeded the owned-byte limit."), std::string::npos);
}

TEST(FMaterialGraphOperationsTests, GraphAssignmentReportsClassMismatchAndAllowsRetry)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	auto Scalar = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
	auto Vector = Testing::MakeGraphExpression<DMaterialExpressionVector3Constant>();
	Scalar->Value = .25f;
	{
		GraphEditInternals::FGraphEditSession Setup(*Material);
		Setup.Expressions.emplace_back(Scalar.Get());
		ASSERT_TRUE(Setup.Commit("Create scalar", nullptr));
	}
	FMaterialGraphCommandResult Rejected;
	{
		GraphEditInternals::FGraphEditSession Edit(*Material);
		Rejected = Edit.Assign(*Scalar, *Vector);
		EXPECT_FALSE(Rejected);
		EXPECT_EQ(Rejected.Message, "Expression assignment requires matching classes.");
		EXPECT_FLOAT_EQ(Scalar->Value, .25f);
		auto Draft = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
		Draft->Id = Scalar->Id;
		Draft->Value = .75f;
		ASSERT_TRUE(Edit.Assign(*Scalar, *Draft));
		EXPECT_FLOAT_EQ(Scalar->Value, .75f);
		// An unfinished session still restores all successful writes after the retry.
	}
	EXPECT_FLOAT_EQ(Scalar->Value, .25f);
	EXPECT_EQ(Rejected.Message, "Expression assignment requires matching classes.");
}

TEST(FMaterialGraphOperationsTests, GestureHistoryFailuresRemainRetryableAndCancellable)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialParameterDefinition Definition;
	Definition.Name = "GestureValue";
	Definition.Type = EMaterialParameterType::Scalar;
	Definition.Value = FMaterialParameterValue::MakeScalar(.25f);
	const auto Created = FMaterialGraphOperations::CreateParameter(*Material, Definition);
	ASSERT_TRUE(Created);
	const auto ParameterId = Created.AffectedParameterIds.front();
	Tests::FTestTransactorOwner Transactions;
	ASSERT_TRUE(Transactions->SetLimits({.MaximumOwnedBytes = 1}));
	FMaterialGraphParameterEditSession Edit;
	ASSERT_TRUE(Edit.Begin(*Material, ParameterId, Transactions.Get()));
	ASSERT_TRUE(Edit.Apply(FMaterialParameterValue::MakeScalar(.75f)));
	const auto Failed = Edit.Commit();
	EXPECT_FALSE(Failed);
	EXPECT_EQ(Failed.GetStatus(), EMaterialGraphCommandStatus::Rejected);
	EXPECT_NE(Failed.Message.find("The transaction exceeded the owned-byte limit."), std::string::npos);
	EXPECT_TRUE(Edit.IsActive());
	EXPECT_FALSE(Transactions->CanUndo());
	EXPECT_FLOAT_EQ(Material->FindParameterDefinition(ParameterId)->Value.GetScalar(), .75f);
	ASSERT_TRUE(Edit.Cancel());
	EXPECT_FLOAT_EQ(Material->FindParameterDefinition(ParameterId)->Value.GetScalar(), .25f);
	ASSERT_TRUE(Edit.Begin(*Material, ParameterId, Transactions.Get()));
	ASSERT_TRUE(Edit.Apply(FMaterialParameterValue::MakeScalar(.5f)));
	EXPECT_FALSE(Edit.Commit());
	ASSERT_TRUE(Transactions->SetLimits({}));
	ASSERT_TRUE(Edit.Commit());
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FLOAT_EQ(Material->FindParameterDefinition(ParameterId)->Value.GetScalar(), .25f);

	ASSERT_TRUE(Transactions->Reset());
	ASSERT_TRUE(Transactions->SetLimits({.MaximumOwnedBytes = 1}));
	const auto NodeId = Created.GeneratedNodeIds.front();
	const auto Before = Material->GetMaterialGraphPresentation();
	FMaterialGraphMoveSession Move;
	ASSERT_TRUE(Move.Begin(*Material, std::array{NodeId}, Transactions.Get()));
	ASSERT_TRUE(Move.Apply(std::array{FMaterialGraphNodePresentation{NodeId, 777, 222}}));
	const auto MoveFailure = Move.Commit();
	EXPECT_FALSE(MoveFailure);
	EXPECT_EQ(MoveFailure.GetStatus(), EMaterialGraphCommandStatus::Rejected);
	EXPECT_NE(MoveFailure.Message.find("The transaction exceeded the owned-byte limit."), std::string::npos);
	EXPECT_TRUE(Move.IsActive());
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), Before);
	ASSERT_TRUE(Transactions->SetLimits({}));
	ASSERT_TRUE(Move.Commit());
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), Before);
	EXPECT_NE(Failed.Message.find("The transaction exceeded the owned-byte limit."), std::string::npos);
	EXPECT_NE(MoveFailure.Message.find("The transaction exceeded the owned-byte limit."), std::string::npos);
}

TEST(FMaterialGraphOperationsTests, DirectParameterHistoryFailureRestoresValueAndReportsFailure)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialParameterDefinition Definition;
	Definition.Name = "DirectValue";
	Definition.Type = EMaterialParameterType::Scalar;
	Definition.Value = FMaterialParameterValue::MakeScalar(.25f);
	const auto Created = FMaterialGraphOperations::CreateParameter(*Material, Definition);
	ASSERT_TRUE(Created);
	const auto ParameterId = Created.AffectedParameterIds.front();
	Tests::FTestTransactorOwner Transactions;
	ASSERT_TRUE(Transactions->SetLimits({.MaximumOwnedBytes = 1}));
	const auto Failed = FMaterialGraphOperations::SetParameterValue(*Material, ParameterId,
		FMaterialParameterValue::MakeScalar(.75f), Transactions.Get());
	EXPECT_FALSE(Failed);
	EXPECT_EQ(Failed.GetStatus(), EMaterialGraphCommandStatus::Rejected);
	EXPECT_NE(Failed.Message.find("The transaction exceeded the owned-byte limit."), std::string::npos);
	EXPECT_TRUE(Failed.CleanupMessage.empty());
	EXPECT_FALSE(Transactions->CanUndo());
	EXPECT_FLOAT_EQ(Material->FindParameterDefinition(ParameterId)->Value.GetScalar(), .25f);
	ASSERT_TRUE(Transactions->SetLimits({}));
	const auto Retried = FMaterialGraphOperations::SetParameterValue(*Material, ParameterId,
		FMaterialParameterValue::MakeScalar(.75f), Transactions.Get());
	ASSERT_TRUE(Retried);
	EXPECT_EQ(Retried.AffectedNodeIds, Created.GeneratedNodeIds);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FLOAT_EQ(Material->FindParameterDefinition(ParameterId)->Value.GetScalar(), .25f);
	EXPECT_NE(Failed.Message.find("The transaction exceeded the owned-byte limit."), std::string::npos);
}

TEST(FMaterialGraphOperationsTests, ParameterSessionRejectionsReportFailureAndAllowRetry)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphParameterEditSession Edit;
	const auto Inactive = Edit.Commit();
	EXPECT_EQ(Inactive.Message.find("No material parameter edit is active."), 0u);
	const auto MissingId = FGuid::NewGuid();
	const auto Missing = Edit.Begin(*Material, MissingId);
	EXPECT_EQ(Missing.Message.find("The material parameter definition is unavailable."), 0u);
	FMaterialParameterDefinition Definition;
	Definition.Name = "TypedSession";
	Definition.Type = EMaterialParameterType::Scalar;
	Definition.Value = FMaterialParameterValue::MakeScalar(.25f);
	const auto Created = FMaterialGraphOperations::CreateParameter(*Material, Definition);
	ASSERT_TRUE(Created);
	const auto Id = Created.AffectedParameterIds.front();
	ASSERT_TRUE(Edit.Begin(*Material, Id));
	const auto Active = Edit.Begin(*Material, MissingId);
	EXPECT_EQ(Active.Message.find("A material parameter edit is already active."), 0u);
	const auto Invalid = Edit.Apply(FMaterialParameterValue::MakeVector4({1, 2, 3, 4}));
	EXPECT_EQ(Invalid.Message.find("The material rejected the parameter value."), 0u);
	EXPECT_FLOAT_EQ(Material->FindParameterDefinition(Id)->Value.GetScalar(), .25f);
	ASSERT_TRUE(Edit.Apply(FMaterialParameterValue::MakeScalar(.75f)));
	ASSERT_TRUE(Edit.Commit());
	ASSERT_TRUE(Edit.Begin(*Material, Id));
	ASSERT_TRUE(Edit.Apply(FMaterialParameterValue::MakeScalar(.5f)));
	ASSERT_TRUE(FMaterialGraphOperations::DeleteParameter(*Material, Id));
	const auto Restore = Edit.Cancel();
	EXPECT_EQ(Restore.Message.find("The material rejected the original parameter value."), 0u);
	EXPECT_FALSE(Edit.IsActive());
}

TEST(FMaterialGraphOperationsTests, BaseParameterMutationRetainsFailuresBeforePublication)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto Invalid = Material->SetParameterValue({}, FMaterialParameterValue::MakeScalar(.5f));
	EXPECT_FALSE(Invalid);
	EXPECT_EQ(std::get<EMaterialParameterError>(Invalid.Error.Code), EMaterialParameterError::InvalidId);
	const auto MissingId = FGuid::NewGuid();
	const auto Missing = Material->SetParameterValue(MissingId, FMaterialParameterValue::MakeScalar(.5f));
	EXPECT_FALSE(Missing);
	EXPECT_EQ(std::get<EMaterialParameterError>(Missing.Error.Code), EMaterialParameterError::NotFound);
	EXPECT_EQ(Missing.Error.ParameterId, MissingId);
	FMaterialParameterDefinition Definition;
	Definition.Name = "MutationCause";
	Definition.Type = EMaterialParameterType::Scalar;
	Definition.Value = FMaterialParameterValue::MakeScalar(.25f);
	const auto Created = FMaterialGraphOperations::CreateParameter(*Material, Definition);
	ASSERT_TRUE(Created);
	const auto Id = Created.AffectedParameterIds.front();
	auto* Owner = Cast<DMaterialExpressionScalarParameter>(Material->GetExpressionCollection().Expressions.front().Get());
	ASSERT_NE(Owner, nullptr);
	Owner->DefaultValue = .1f;
	const auto Mismatch = Material->SetScalarParameterValue("MutationCause", .75f);
	EXPECT_FALSE(Mismatch);
	EXPECT_EQ(std::get<EMaterialParameterError>(Mismatch.Error.Code), EMaterialParameterError::OwnerMismatch);
	EXPECT_EQ(Mismatch.Error.ParameterId, Id);
	EXPECT_EQ(Mismatch.Error.ParameterName, "MutationCause");
	EXPECT_FLOAT_EQ(Owner->DefaultValue, .1f);
	EXPECT_FLOAT_EQ(Material->FindParameterDefinition(Id)->Value.GetScalar(), .25f);
	Owner->DefaultValue = .25f;
	ASSERT_TRUE(Material->SetParameterValue(Id, FMaterialParameterValue::MakeScalar(.75f)));
	EXPECT_FLOAT_EQ(Owner->DefaultValue, .75f);
	EXPECT_EQ(std::get<EMaterialParameterError>(Mismatch.Error.Code), EMaterialParameterError::OwnerMismatch);
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
	EXPECT_FALSE(Rejected);
	EXPECT_EQ(std::get<EMaterialParameterError>(Rejected.Error.Code), EMaterialParameterError::InvalidDefault);
	EXPECT_EQ(Rejected.Error.ParameterId, Before.Id);
	EXPECT_EQ(Parameter->GetParameterDefinition(), Before);
	Invalid.Value = FMaterialParameterValue::MakeScalar(.75f);
	ASSERT_TRUE(Parameter->SetParameterDefinition(Invalid));
	EXPECT_FLOAT_EQ(Parameter->DefaultValue, .75f);
	EXPECT_EQ(std::get<EMaterialParameterError>(Rejected.Error.Code), EMaterialParameterError::InvalidDefault);
}

TEST(FMaterialGraphOperationsTests, ParameterFactoryReportsValidationAndNormalizesValidRetry)
{
	InitializeDObjectSystem();
	FMaterialParameterDefinition Definition;
	Definition.Id = FGuid::NewGuid();
	Definition.Type = EMaterialParameterType::Vector;
	Definition.Value = FMaterialParameterValue::MakeVector({.2f, .3f, .4f});
	const auto Rejected = GraphEditInternals::MakeParameterExpression(Definition);
	EXPECT_FALSE(Rejected);
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
	FMaterialGraphDocument Document(*Material);
	const auto Created = Testing::CreateGraphConstant(Document, .5f, 40, 80);
	ASSERT_TRUE(Created);
	const auto Id = Created.GeneratedNodeIds.front();
	const auto Before = Material->GetMaterialGraphPresentation();
	FMaterialGraphPresentation Candidate;
	const auto Duplicate = Document.CalculateLayout(std::array{Id, Id}, Candidate);
	EXPECT_FALSE(Duplicate);
	EXPECT_EQ(Duplicate.Message.find("The material graph layout request contains duplicate node GUIDs."), 0u);
	EXPECT_EQ(Candidate, Before);
	const auto MissingId = FGuid::NewGuid();
	const auto Missing = Document.CalculateLayout(std::array{MissingId}, Candidate);
	EXPECT_FALSE(Missing);
	EXPECT_EQ(Missing.Message.find("A material graph layout node does not exist."), 0u);
	const std::vector<FGuid> Oversized(MaterialProgramMaxNodeCount + 1u, Id);
	const auto Bounds = Document.CalculateLayout(Oversized, Candidate);
	EXPECT_FALSE(Bounds);
	EXPECT_EQ(Bounds.Message.find("The material graph layout request exceeds the node bound."), 0u);
	const auto Coordinate = Document.MoveNodes(std::array{FMaterialGraphNodePresentation{Id, MaterialGraphPresentationCoordinateLimit + 1, 0}});
	EXPECT_FALSE(Coordinate);
	EXPECT_EQ(Coordinate.Message.find("A material graph position is outside the supported coordinate range."), 0u);
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), Before);
	ASSERT_TRUE(Document.CalculateLayout(std::array{Id}, Candidate));
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), Before);
	ASSERT_TRUE(Document.MoveNodes(std::array{FMaterialGraphNodePresentation{Id, 100, 200}}));
}

TEST(FMaterialGraphOperationsTests, ClipboardRejectionsReportFailureAndAllowRetry)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	const auto Created = Testing::CreateGraphConstant(Document, .5f, 40, 80);
	ASSERT_TRUE(Created);
	const auto Id = Created.GeneratedNodeIds.front();
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(Document.CopySelection(std::array{Id}, Payload));
	const auto Empty = Document.CopySelection({}, Payload);
	EXPECT_FALSE(Empty);
	EXPECT_EQ(Empty.Message.find("The material graph copy selection is empty or exceeds the node bound."), 0u);
	EXPECT_TRUE(Payload.Nodes.empty());
	ASSERT_TRUE(Document.CopySelection(std::array{Id}, Payload));
	const auto Before = CaptureExpressions(*Material);
	const auto Presentation = Material->GetMaterialGraphPresentation();
	Tests::FTestTransactorOwner Transactions;
	++Payload.SchemaVersion;
	const auto Schema = Document.Paste(Payload, 0, 0, Transactions.Get());
	EXPECT_FALSE(Schema);
	EXPECT_EQ(Schema.Message.find("The material graph clipboard schema version is unsupported."), 0u);
	Payload.SchemaVersion = CurrentMaterialGraphClipboardSchemaVersion;
	Payload.Nodes.front().RelativeX = -1;
	const auto Position = Document.Paste(Payload, 0, 0, Transactions.Get());
	EXPECT_FALSE(Position);
	EXPECT_EQ(Position.Message.find("The material graph clipboard contains an invalid relative position."), 0u);
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), Presentation);
	EXPECT_FALSE(Transactions->CanUndo());
	Payload.Nodes.front().RelativeX = 0;
	ASSERT_TRUE(Document.Paste(Payload, 100, 200, Transactions.Get()));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
}

TEST(FMaterialGraphOperationsTests, InputDefaultRejectionsDoNotPublish)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	ASSERT_TRUE(Testing::CreateGraphConstant(Document, .5f, 40, 80));
	const auto OutputId = Material->GetOutputNode()->Id;
	const auto Pin = static_cast<uint32>(EMaterialOutputPin::Metallic);
	const auto Before = CaptureExpressions(*Material);
	Tests::FTestTransactorOwner Transactions;
	const auto MissingId = FGuid::NewGuid();
	const auto Missing = Document.SetInputDefault(MissingId, 3, {}, {}, Transactions.Get());
	EXPECT_FALSE(Missing);
	EXPECT_EQ(Missing.Message.find("The input owner is unavailable."), 0u);
	const auto NonNumeric = Document.SetInputDefault(OutputId, Pin,
		{.Kind = EMaterialInputDefaultKind::Literal, .Type = EMaterialProgramValueType::Texture2D}, {}, Transactions.Get());
	EXPECT_FALSE(NonNumeric);
	EXPECT_EQ(NonNumeric.Message.find("The input default is not numeric."), 0u);
	const auto Width = Document.SetInputDefault(OutputId, Pin,
		{.Kind = EMaterialInputDefaultKind::Literal, .Type = EMaterialProgramValueType::Float3, .Literal = {1, 2, 3}}, {}, Transactions.Get());
	EXPECT_FALSE(Width);
	EXPECT_EQ(Width.Message.find("The input default has an incompatible width."), 0u);
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_FALSE(Transactions->CanUndo());
	ASSERT_TRUE(Document.SetInputDefault(OutputId, Pin,
		{.Kind = EMaterialInputDefaultKind::Literal, .Type = EMaterialProgramValueType::Float, .Literal = {.X = .7f}}, {}, Transactions.Get()));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
}

TEST(FMaterialGraphOperationsTests, DocumentValueRejectionsReportFailureAndAllowRetry)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	const auto Created = Testing::CreateGraphConstant(Document, .5f, 40, 80);
	ASSERT_TRUE(Created);
	const auto Id = Created.GeneratedNodeIds.front();
	Durin::Tests::FTestTransactorOwner Transactions;
	const auto Before = CaptureExpressions(*Material);
	const auto BadValue = Document.SetConstantValue(Id,
		FMaterialParameterValue::MakeScalar(std::numeric_limits<float>::infinity()), Transactions.Get());
	ASSERT_FALSE(BadValue);
	EXPECT_EQ(BadValue.Message.find("Constant values must be finite."), 0u);
	ASSERT_EQ(BadValue.Diagnostics.size(), 1u);
	EXPECT_EQ(BadValue.Diagnostics.front().Error, EMaterialExpressionError::NonFiniteConstant);
	const auto BadComponents = Document.SetSwizzleComponents(Id, std::array<uint8, 2>{0, 7}, Transactions.Get());
	ASSERT_FALSE(BadComponents);
	EXPECT_EQ(BadComponents.Message.find("Swizzles require one to four valid components."), 0u);
	const auto WrongNode = Document.SetSwizzleComponents(Id, std::array<uint8, 1>{0}, Transactions.Get());
	EXPECT_EQ(WrongNode.Message.find("The selected expression is not a swizzle."), 0u);
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_FALSE(Transactions.Get()->CanUndo());
	ASSERT_TRUE(Document.SetConstantValue(Id, FMaterialParameterValue::MakeScalar(.75f), Transactions.Get()));
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_EQ(BadValue.Message.find("Constant values must be finite."), 0u);
}

TEST(FMaterialGraphOperationsTests, ConnectionRejectionsPreserveExistingBindings)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	const auto First = Testing::CreateGraphConstant(Document, .5f, 40, 80);
	const auto Second = Testing::CreateGraphConstant(Document, .75f, 40, 160);
	ASSERT_TRUE(First);
	ASSERT_TRUE(Second);
	const auto Target = FMaterialGraphPinAddress::Input(Material->GetOutputNode()->Id,
		static_cast<uint32>(EMaterialOutputPin::Metallic));
	const auto Source = FMaterialGraphPinAddress::Output({Second.GeneratedNodeIds.front()});
	ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::Input(Target.NodeId, Target.Index), FMaterialGraphPinAddress::Output({First.GeneratedNodeIds.front()}), true));
	const auto Before = CaptureExpressions(*Material);
	Tests::FTestTransactorOwner Transactions;
	const auto Occupied = Document.Connect(Target, Source, false, Transactions.Get());
	ASSERT_FALSE(Occupied);
	EXPECT_EQ(Occupied.Message.find("The graph input is already connected."), 0u);
	auto BadSource = Source;
	BadSource.Index = 256;
	const auto BadIndex = Document.Connect(Target, BadSource, true, Transactions.Get());
	EXPECT_EQ(BadIndex.Message.find("The source must be an output pin."), 0u);
	auto MissingTarget = Target;
	MissingTarget.NodeId = FGuid::NewGuid();
	const auto Missing = Document.Connect(MissingTarget, Source, true, Transactions.Get());
	EXPECT_EQ(Missing.Message.find("The graph input no longer exists."), 0u);
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_FALSE(Transactions->CanUndo());
	ASSERT_TRUE(Document.Connect(Target, Source, true, Transactions.Get()));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
}

TEST(FMaterialGraphOperationsTests, DocumentStructureRejectionsPreserveState)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	const auto Created = Testing::CreateGraphConstant(Document, .5f, 40, 80);
	ASSERT_TRUE(Created);
	Tests::FTestTransactorOwner Transactions;
	const auto Before = CaptureExpressions(*Material);
	const auto OutputId = Material->GetOutputNode()->Id;
	const auto Output = Document.RemoveNodes(std::array{OutputId}, Transactions.Get());
	EXPECT_EQ(Output.Message.find("The material output node cannot be removed."), 0u);
	const std::vector<FGuid> Oversized(MaterialProgramMaxNodeCount + 1);
	const auto Bounds = Document.RemoveNodes(Oversized, Transactions.Get());
	EXPECT_EQ(Bounds.Message.find("The removal selection exceeds the graph bound."), 0u);
	auto Replacement = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
	const auto Missing = Document.ReplaceExpression(*Replacement, Transactions.Get());
	EXPECT_EQ(Missing.Message.find("The expression no longer exists."), 0u);
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_FALSE(Transactions->CanUndo());
	Replacement->Id = Created.GeneratedNodeIds.front();
	Replacement->Value = .75f;
	ASSERT_TRUE(Document.ReplaceExpression(*Replacement, Transactions.Get()));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
}

TEST(FMaterialGraphOperationsTests, CatalogRejectionsReportStaleShapeAndAllowRetry)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	const auto Catalog = FMaterialGraphOperations::EnumerateCatalog();
	const auto Found = std::ranges::find_if(Catalog, [](const auto& Entry) {
		return Entry.Opcode == EMaterialProgramOpcode::MakeSurface;
	});
	ASSERT_NE(Found, Catalog.end());
	auto Invalid = *Found;
	Invalid.ExpressionClass = DMaterialExpressionScalarConstant::StaticClass();
	Tests::FTestTransactorOwner Transactions;
	const auto Before = CaptureExpressions(*Material);
	const auto Failure = Document.CreateCatalogNode(Invalid, 40, 80, {}, Transactions.Get());
	ASSERT_FALSE(Failure);
	EXPECT_EQ(Failure.Message.find("The catalog expression shape is stale."), 0u);
	Invalid.AcceptedInputTypes.clear();
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_FALSE(Transactions->CanUndo());
	ASSERT_TRUE(Document.CreateCatalogNode(*Found, 40, 80, {}, Transactions.Get()));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
}

TEST(FMaterialGraphOperationsTests, CreationErrorsReportPathFailureAndAllowRetry)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	const auto Constant = Testing::CreateGraphConstant(Document, .5f, 40, 80);
	ASSERT_TRUE(Constant);
	const auto Source = FMaterialGraphPinAddress::Output({Constant.GeneratedNodeIds.front()});
	Tests::FTestTransactorOwner Transactions;
	const auto Before = CaptureExpressions(*Material);
	auto Action = MakeFunctionCreationAction("invalid-relative-function");
	const auto Failed = Document.Create({Action, 0, 0, Source}, Transactions.Get());
	ASSERT_FALSE(Failed);
	EXPECT_FALSE(Failed.Message.empty());
	FTopLevelAssetPath Path;
	const auto Expected = FTopLevelAssetPath::TryCreate("invalid-relative-function", Path);
	EXPECT_NE(Failed.Message.find(FormatObjectError(Expected.Error)), std::string::npos);
	Action.Payload = std::string("changed-request");
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_FALSE(Transactions->CanUndo());
	const auto Catalog = FMaterialGraphOperations::EnumerateCatalog();
	const auto Found = std::ranges::find_if(Catalog, [](const auto& Entry) {
		return Entry.Opcode == EMaterialProgramOpcode::MakeSurface;
	});
	ASSERT_NE(Found, Catalog.end());
	ASSERT_TRUE(Document.Create({MakeCreationAction(*Found), 40, 160}, Transactions.Get()));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
}

TEST(FMaterialGraphOperationsTests, OperationRejectionsReportParameterSurfaceAndConnectionFailures)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	const auto Constant = Testing::CreateGraphConstant(Document, .5f, 40, 80);
	ASSERT_TRUE(Constant);
	Tests::FTestTransactorOwner Transactions;
	const auto Before = CaptureExpressions(*Material);
	const auto ParameterId = FGuid::NewGuid();
	const auto Missing = FMaterialGraphOperations::RenameParameter(*Material, ParameterId, "NewName", Transactions.Get());
	EXPECT_EQ(Missing.Message.find("Parameter owner is unavailable."), 0u);
	const auto InvalidOutput = static_cast<EMaterialSurfaceOutput>(255);
	const auto Surface = FMaterialGraphDocument(*Material).Disconnect(FMaterialGraphPinAddress::MaterialOutput((*Material).GetOutputNode()->Id, InvalidOutput), Transactions.Get());
	EXPECT_EQ(Surface.Message.find("The material attribute pin is invalid."), 0u);
	const auto Target = FMaterialGraphPinAddress::MaterialOutput(Material->GetOutputNode()->Id, EMaterialSurfaceOutput::Metallic);
	auto Source = FMaterialGraphPinAddress::Output({FGuid::NewGuid()});
	const auto MissingSource = Document.Connect(Target, Source, true, Transactions.Get());
	EXPECT_EQ(MissingSource.Message.find("The source output no longer exists."), 0u);
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_FALSE(Transactions->CanUndo());
	Source = FMaterialGraphPinAddress::Output({Constant.GeneratedNodeIds.front()});
	ASSERT_TRUE(Document.Connect(Target, Source, true, Transactions.Get()));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
}

TEST(FMaterialGraphOperationsTests, CommandStatusAndFailurePresentationAreIndependent)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	auto Rejected = Document.SetSwizzleComponents(FGuid::NewGuid(), {});
	EXPECT_FALSE(Rejected);
	EXPECT_EQ(Rejected.GetStatus(), EMaterialGraphCommandStatus::Rejected);
	Rejected.Message.clear();
	EXPECT_FALSE(Rejected);
	EXPECT_FALSE(FormatMaterialGraphCommandResult(Rejected).empty());
	const auto NoChange = Document.RemoveNodes({});
	EXPECT_TRUE(NoChange);
	EXPECT_FALSE(NoChange.HasError());
	EXPECT_EQ(NoChange.GetStatus(), EMaterialGraphCommandStatus::NoChange);
	EXPECT_TRUE(FormatMaterialGraphCommandResult(NoChange).empty());
	FMaterialGraphCommandResult CleanupFailed{
		.Status = EMaterialGraphCommandStatus::Rejected,
		.Message = "Commit failed.", .CleanupMessage = "Restore failed."};
	EXPECT_FALSE(CleanupFailed);
	EXPECT_EQ(FormatMaterialGraphCommandResult(CleanupFailed), "Commit failed. Cleanup: Restore failed.");
}

TEST(FMaterialGraphOperationsTests, ParameterReplayRetainsMaterialCauseAndAllowsRepair)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialParameterDefinition Definition;
	Definition.Id = FGuid::NewGuid();
	Definition.Name = "ReplayValue";
	Definition.Type = EMaterialParameterType::Scalar;
	Definition.Value = FMaterialParameterValue::MakeScalar(.25f);
	FTransactionRecord Record(GraphEditInternals::MakeMaterialGraphParameterTransaction(*Material,
		Definition.Id, Definition.Value, FMaterialParameterValue::MakeScalar(.75f)));
	const auto Failed = Record.Apply(false, EPropertyChangeOrigin::Redo);
	ASSERT_FALSE(Failed);
	EXPECT_EQ(Failed.Error.Code, ETransactionRecordError::CustomRejected);
	ASSERT_TRUE(Failed.Error.CustomCause);
	EXPECT_EQ(Failed.Error.CustomCause->Code, ETransactionCustomError::MaterialWrite);
	EXPECT_EQ(Failed.Error.CustomCause->ParameterId, Definition.Id);
	ASSERT_TRUE(Failed.Error.CustomCause->MaterialCause);
	EXPECT_EQ(Failed.Error.CustomCause->MaterialCause->Code,
		FMaterialError::FCode(EMaterialParameterError::NotFound));
	ASSERT_TRUE(FMaterialGraphOperations::CreateParameter(*Material, Definition));
	ASSERT_TRUE(Record.Apply(false, EPropertyChangeOrigin::Redo));
	EXPECT_EQ(Material->FindParameterDefinition(Definition.Id)->Value.GetScalar(), .75f);
	ASSERT_TRUE(Record.Apply(true, EPropertyChangeOrigin::Undo));
	EXPECT_EQ(Material->FindParameterDefinition(Definition.Id)->Value.GetScalar(), .25f);
	EXPECT_EQ(Failed.Error.CustomCause->MaterialCause->ParameterId, Definition.Id);
}

TEST(FMaterialGraphOperationsTests, PresentationReplayRetainsExpiredTargetIdentity)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "ExpiredPresentationOwner");
	const auto Path = Material->GetObjectPath();
	const auto NodeId = FGuid::NewGuid();
	const FMaterialGraphPresentation Before{.Nodes = {{NodeId, 10, 20}}};
	const FMaterialGraphPresentation After{.Nodes = {{NodeId, 30, 40}}};
	FTransactionRecord Record(GraphEditInternals::MakeMaterialGraphPresentationTransaction(
		*Material, Before, After, "Move expired owner"));
	MarkAsGarbage(Material);
	CollectGarbage();
	const auto Failed = Record.Apply(true, EPropertyChangeOrigin::Undo);
	ASSERT_FALSE(Failed);
	ASSERT_TRUE(Failed.Error.CustomCause);
	EXPECT_EQ(Failed.Error.CustomCause->Code, ETransactionCustomError::TargetUnavailable);
	EXPECT_EQ(Failed.Error.CustomCause->TargetPath, Path);
	EXPECT_EQ(Failed.Error.CustomCause->NodeCount, 1u);
	const auto Redo = Record.Apply(false, EPropertyChangeOrigin::Redo);
	ASSERT_TRUE(Redo.Error.CustomCause);
	EXPECT_EQ(Redo.Error.CustomCause->Code, ETransactionCustomError::TargetUnavailable);
	EXPECT_EQ(Failed.Error.CustomCause->TargetPath, Path);
}
