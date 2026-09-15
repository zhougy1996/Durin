#include "TypedMaterialGraphTestFixture.h"
#include "Graph/MaterialExpressionInputs.h"
#include "ExplicitMaterialProgramTestFixture.h"
#include "StandardMaterialFunctionTestFixture.h"
#include "Misc/MountPathTestSupport.h"
#include "MaterialGraphOperations.h"
#include "MaterialGraphDocument.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "MaterialAssetCreation.h"
#include "Graph/MaterialGraphCanvas.h"
#include "Workspace/MaterialEditorWorkspace.h"
#include "Widgets/MaterialPreviewFraming.h"

#include "MaterialTestSupport.h"

#include "Asset/AssetCompilingManager.h"
#include "Asset/Asset.h"
#include "AssetRegistry/Scan.h"
#include "DObject/DefaultObjectGraph.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Editor/Transaction.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Materials/MaterialExpressionBuild.h"
#include "Texture/Texture2D.h"

#include <gtest/gtest.h>
#include <chrono>

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
		static auto ShowAdvanced(FMaterialGraphCanvas& Canvas) -> void
		{ Canvas.bShowAdvancedInputs = true; Canvas.CachedMaterial = nullptr; }
		static auto HideAdvanced(FMaterialGraphCanvas& Canvas) -> void
		{ Canvas.bShowAdvancedInputs = false; Canvas.CachedMaterial = nullptr; }
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

	struct FExpressionGraphSnapshot
	{
		std::vector<TStrongObjectPtr<DMaterialExpression>> Expressions;
		FMaterialExpressionSurfaceOutputs Outputs;
		FMaterialFunctionSignature Signature;
		auto operator==(const FExpressionGraphSnapshot& Other) const -> bool
		{
			if (Signature != Other.Signature || Outputs != Other.Outputs || Expressions.size() != Other.Expressions.size()) return false;
			for (size_t Index = 0; Index < Expressions.size(); ++Index)
			{
				if (Expressions[Index]->GetClass() != Other.Expressions[Index]->GetClass()) return false;
				bool Identical = true;
				Expressions[Index]->GetClass()->ForEachProperty([&](FProperty* Property) {
					Identical &= ArePropertyValuesIdentical(Property, Expressions[Index].Get(), 0, Other.Expressions[Index].Get(), 0);
				});
				if (!Identical) return false;
			}
			return true;
		}
	};
	auto CaptureExpressions(const DMaterial& Material) -> FExpressionGraphSnapshot
	{
		FExpressionGraphSnapshot Result;
		Result.Outputs = Material.GetExpressionOutputs();
		for (const auto& Expression : Material.GetExpressionCollection().Expressions)
			Result.Expressions.emplace_back(DuplicateObject(Expression.Get(), nullptr, NAME_None));
		return Result;
	}

	auto CaptureExpressions(const DMaterialFunction& Function) -> FExpressionGraphSnapshot
	{
		FExpressionGraphSnapshot Result;
		Result.Signature = Function.GetFunctionSignature();
		for (const auto& Expression : Function.GetExpressionCollection().Expressions)
			Result.Expressions.emplace_back(DuplicateObject(Expression.Get(), nullptr, NAME_None));
		return Result;
	}

	template<class T, class TOwner> auto FindExpression(const TOwner& Material, FGuid Id) -> const T*
	{
		for (const auto& Expression : Material.GetExpressionCollection().Expressions)
			if (Expression->Id == Id) return Cast<T>(Expression.Get());
		return nullptr;
	}

	auto FindViewNode(const FMaterialGraphView& View, const FGuid& Id)
		-> const FMaterialGraphNodeView*
	{
		const auto It = std::ranges::find(View.Nodes, Id,
			[](const FMaterialGraphNodeView& Node) { return Node.Node.Id; });
		return It == View.Nodes.end() ? nullptr : &*It;
	}

	auto Normalize(const DMaterial& Material) -> FMaterialNormalizationResult
	{
		FMaterialIRCompilerInput Input;
		FMaterialCompilerEnvironment Environment;
		Environment.CompilerIdentity = "material-graph-operations-test";
		Environment.Target = "vulkan-spirv-1.5";
		if (!SnapshotMaterialCompilerInput(Material, Environment, Input)) return {};
		return NormalizeMaterialIR(Input);
	}

	auto MakeExpandedGraphMaterial(const char* Name) -> DMaterial*
	{
		DMaterial* Material = NewObject<DMaterial>(nullptr, Name);
		if (!Material || !Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Material)
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
	auto Parameter = Testing::MakeGraphExpression<DMaterialExpressionVector4Parameter>();
	Parameter->Metadata.Id = Definition.Id; Parameter->Metadata.Name = Definition.Name;
	Parameter->DefaultValue = Definition.Value.GetVector4();
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 1>{Parameter.Get()}, {}));
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

TEST(FMaterialGraphOperationsTests, LargeGraphRoundTripsWithoutLoadedOverrideLedger)
{
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture MountRegistry;
	const auto Root = Testing::GetTestWorkDirectory() / "GraphWithoutLedger";
	Testing::RemoveTestWorkDirectory(Root);
	Testing::RegisterMountPointForTests("/GraphWithoutLedger/", Root.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/GraphWithoutLedger/Base", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	Testing::FTestMaterialExpressionGraph Graph;
	for (int Index = 0; Index < 65; ++Index)
	{
		FMaterialParameterDefinition Definition;
		Definition.Id = FGuid::NewGuid();
		Definition.Name = std::format("Tint{}", Index);
		Definition.Type = EMaterialParameterType::Vector4;
		Definition.Value = FMaterialParameterValue::MakeVector4({0.1, 0.0, 0.3, 1.0});
		auto Parameter = Testing::MakeGraphExpression<DMaterialExpressionVector4Parameter>();
		Parameter->Metadata.Id = Definition.Id; Parameter->Metadata.Name = Definition.Name;
		Parameter->DefaultValue = Definition.Value.GetVector4();
		Graph.Expressions.emplace_back(Parameter.Get());
	}
	ASSERT_TRUE(Graph.Apply(*Material));
	const auto Expected = CaptureExpressions(*Material);
	ASSERT_TRUE(SavePackage(Material->GetPackage(), EAssetPackageSaveMode::Complete));
	const auto CompleteBytes = std::filesystem::file_size(Root / "Base.dasset");
	for (int Round = 0; Round < 2; ++Round)
	{
		ASSERT_TRUE(SavePackage(Material->GetPackage()));
		const auto DeltaBytes = std::filesystem::file_size(Root / "Base.dasset");
		EXPECT_LE(DeltaBytes, CompleteBytes);
		ASSERT_TRUE(UnloadPackage(Path));
		const auto LoadResult = LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Path), Material);
		ASSERT_TRUE(LoadResult) << LoadResult.Message;
		ASSERT_NE(Material, nullptr);
		EXPECT_FALSE(Material->HasAllocatedAuthoredOverrideLedger());
		EXPECT_EQ(CaptureExpressions(*Material), Expected);
		auto* Copy = Cast<DMaterial>(DuplicateObject(Material, nullptr, "GraphWithoutLedgerCopy"));
		ASSERT_NE(Copy, nullptr);
		EXPECT_FALSE(Copy->HasAllocatedAuthoredOverrideLedger());
		EXPECT_EQ(CaptureExpressions(*Copy), Expected);
		MarkObjectHierarchyAsGarbage(Copy);
	}
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
	auto Parameter = Testing::MakeGraphExpression<DMaterialExpressionVector4Parameter>();
	Parameter->Metadata.Id = Definition.Id;
	Parameter->Metadata.Name = Definition.Name;
	Parameter->DefaultValue = Definition.Value.GetVector4();
	FMaterialGraphDocument Document(*Material);
	FMaterialGraphDocumentState Candidate;
	Candidate.Expressions.emplace_back(Parameter.Get());
	const auto Original = CaptureExpressions(*Material);
	const uint64 Revision = Material->GetMaterialCompileStatus().AuthoredRevision;
	ASSERT_TRUE(Document.Commit(Candidate, "Replace Graph", Transactions.Get()));
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
	const auto NoChange = Document.Commit(Candidate, "Replace Graph", Transactions.Get());
	EXPECT_EQ(NoChange.Status, EMaterialGraphCommandStatus::NoChange);
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests,
	GraphViewRevisionsTrackRelevantAuthoredState)
{
	InitializeDObjectSystem();
	DMaterial* Material = MakeExpandedGraphMaterial("GraphViewRevisions");
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

	FMaterialGraphDocumentState Candidate;
	FMaterialGraphDocument Document(*Material);
	ASSERT_TRUE(Document.Capture(Candidate));
	Candidate.Outputs.RoughnessDefault = 0.75f;
	auto Validation = Document.Commit(std::move(Candidate), "Change roughness default");
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
	EXPECT_TRUE(Material->GetExpressionCollection().Expressions.empty());
	EXPECT_TRUE(Material->GetParameterDefinitions().empty());
	EXPECT_TRUE(Material->GetMaterialGraphPresentation().bHasMaterialOutputPosition);
	EXPECT_EQ(Material->GetExpressionOutputs().BaseColorDefault,
		(Durin::FVector3{0.5f, 0.5f, 0.5f}));

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
		"/Engine/Materials/DefaultMaterial"})
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
			Material->GetExpressionCollection().Expressions.size());
		EXPECT_TRUE(Presentation.bHasMaterialOutputPosition);
		const FMaterialGraphView View = FMaterialGraphOperations::Inspect(*Material);
		EXPECT_EQ(View.Nodes.size(), Material->GetExpressionCollection().Expressions.size());
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
	AggregateGraph.Outputs = {.Surface = {Surface->Id}};
	ASSERT_TRUE(AggregateGraph.Apply(*Material));
	const FGuid SurfaceId = Material->GetExpressionOutputs().Surface.ExpressionId;
	ASSERT_TRUE(SurfaceId.IsValid());
	FMaterialGraphPresentation AggregatePresentation;
	AggregatePresentation.Nodes.push_back({SurfaceId, 100, 100});
	AggregatePresentation.bHasMaterialOutputPosition = true;
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(AggregatePresentation));
	ASSERT_TRUE(FMaterialGraphOperations::DisconnectAggregateSurface(*Material));
	ASSERT_TRUE(FMaterialGraphOperations::AssignAggregateSurface(
		*Material, SurfaceId));
	EXPECT_EQ(Material->GetExpressionOutputs().Surface.ExpressionId,
		SurfaceId);
	EXPECT_FALSE(Material->GetExpressionOutputs().BaseColor.ExpressionId.IsValid());
	const auto Normalized = Normalize(*Material);
	ASSERT_TRUE(Normalized);
	EXPECT_TRUE(Normalized.IR.SurfaceRoot.bAggregate);
	EXPECT_EQ(Normalized.IR.Nodes.size(), AggregateGraph.Expressions.size());
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(FMaterialGraphOperations::CopySelection(
		*Material, std::array{SurfaceId}, Payload));
	EXPECT_TRUE(Payload.bConnectAggregateSurface);
	ASSERT_TRUE(FMaterialGraphOperations::Paste(*Material, Payload, 300, 100));
	EXPECT_NE(Material->GetExpressionOutputs().Surface.ExpressionId,
		SurfaceId);
	ASSERT_TRUE(FMaterialGraphOperations::DisconnectAggregateSurface(*Material));
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
	Presentation.bHasMaterialOutputPosition = true;
	Presentation.MaterialOutputX = 640;
	Presentation.MaterialOutputY = -120;

	const FMaterialGraphPresentation Sanitized =
		SanitizeMaterialGraphPresentation(Presentation, Ids);
	EXPECT_EQ(Sanitized.SchemaVersion,
		CurrentMaterialGraphPresentationSchemaVersion);
	ASSERT_EQ(Sanitized.Nodes.size(), 1u);
	EXPECT_EQ(Sanitized.Nodes.front().NodeId, Ids[1]);
	EXPECT_EQ(Sanitized.Nodes.front().X, 20);
	EXPECT_EQ(Sanitized.Nodes.front().Y, 40);
	EXPECT_TRUE(Sanitized.bHasMaterialOutputPosition);
	EXPECT_EQ(Sanitized.MaterialOutputX, 640);
	EXPECT_EQ(Sanitized.MaterialOutputY, -120);
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

	EXPECT_TRUE(Transactions->Reset());
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

	EXPECT_TRUE(Transactions->Reset());
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
				return static_cast<uint8>(Entry.Opcode) == Value;
			})) << "Missing opcode " << static_cast<uint32>(Value);
	}
	const FMaterialGraphView View = FMaterialGraphOperations::Inspect(*Material);
	EXPECT_EQ(View.Nodes.size(), Material->GetExpressionCollection().Expressions.size());
	for (const FMaterialGraphNodeView& Node : View.Nodes)
	{
		EXPECT_FALSE(Node.PrimaryLabel.empty());
		const auto Expression = std::ranges::find_if(Material->GetExpressionCollection().Expressions,
			[&](const auto& Value) { return Value->Id == Node.Node.Id; });
		ASSERT_NE(Expression, Material->GetExpressionCollection().Expressions.end());
		EXPECT_EQ(Node.Inputs.size(), (*Expression)->GetAuthoredInputCount());
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
				ASSERT_TRUE(Prerequisite) << Prerequisite.Message;
				Source = {Prerequisite.GeneratedNodeIds.front()};
			}
		}
		const auto Created = Document.CreateCatalogNode(Entry, 400, 200, Source);
		ASSERT_TRUE(Created) << Created.Message;
		ASSERT_EQ(Created.GeneratedNodeIds.size(), 1u);
		const auto& Expressions = Material->GetExpressionCollection().Expressions;
		const auto It = std::ranges::find(Expressions, Created.GeneratedNodeIds.front(), [](const auto& E) { return E->Id; });
		ASSERT_NE(It, Expressions.end());
		EXPECT_EQ((*It)->GetClass(), Entry.ExpressionClass);
		EXPECT_EQ((*It)->GetOuter(), Material);
		const auto ObjectRevision = GDObjectArray.GetRevision();
		const auto View = Document.Inspect(Catalog);
		const auto* Viewed = FindViewNode(View, (*It)->Id);
		ASSERT_NE(Viewed, nullptr);
		EXPECT_EQ(Viewed->Node.Opcode, Entry.Opcode);
		EXPECT_EQ(Viewed->Node.ResultType, Entry.ResultType);
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
		ASSERT_TRUE(Created) << Created.Message;
		const auto* Original = FindExpression<DMaterialExpression>(*Material, Created.GeneratedNodeIds.front());
		ASSERT_NE(Original, nullptr);
		ASSERT_EQ(Original->GetAuthoredInputCount(), Entry.AcceptedInputTypes.size());
		for (uint32 Pin = 0; Pin < Entry.AcceptedInputTypes.size(); ++Pin)
			for (Type SourceType : {Type::Float, Type::Float2, Type::Float3, Type::Float4, Type::Texture2D, Type::Surface})
			{
				SCOPED_TRACE(std::format("{} result {} pin {} source {}", Entry.OperationName,
					static_cast<uint8>(Entry.ResultType), Pin, static_cast<uint8>(SourceType)));
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
				TStrongObjectPtr<DMaterialExpression> Target(DuplicateObject(Original, nullptr, NAME_None));
				if (auto* Swizzle = Cast<DMaterialExpressionSwizzle>(Target.Get()))
					std::ranges::fill(Swizzle->Components, 0);
				VisitMaterialExpressionInputs(*Target, [&](uint32 Index, FMaterialExpressionInput& Input) {
					Input = AddSource(Index == Pin ? SourceType : Entry.AcceptedInputTypes[Index].front());
				});
				Graph.Expressions.emplace_back(Target.Get());
				std::vector<DMaterialExpression*> Expressions;
				for (const auto& Expression : Graph.Expressions) Expressions.push_back(Expression.Get());
				const auto& Accepted = Entry.AcceptedInputTypes[Pin];
				const bool bAccepted = std::ranges::find(Accepted, SourceType) != Accepted.end();
				EXPECT_EQ(static_cast<bool>(FMaterialExpressionBuildContext::ValidateSurface(Expressions, {})), bAccepted);
				// Fixed concrete pins cannot disappear; a dangling source is rejected instead.
				VisitMaterialExpressionInputs(*Target, [&](uint32 Index, FMaterialExpressionInput& Input) {
					if (Index == Pin) Input.ExpressionId = FGuid::NewGuid();
				});
				EXPECT_FALSE(FMaterialExpressionBuildContext::ValidateSurface(Expressions, {}));
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
			EXPECT_FALSE(FMaterialExpressionBuildContext::ValidateSurface(std::array<DMaterialExpression*, 2>{Source.Get(), Target.Get()}, {}));
		}
	EXPECT_FALSE(GetMaterialProgramNodeSignature(static_cast<EMaterialProgramOpcode>(3), Type::Float));
	EXPECT_FALSE(GetMaterialProgramNodeSignature(static_cast<EMaterialProgramOpcode>(255), Type::Float));
	EXPECT_FALSE(GetMaterialProgramNodeSignature(EMaterialProgramOpcode::Constant, static_cast<Type>(255)));
	EXPECT_FALSE(GetMaterialProgramNodeSignature(EMaterialProgramOpcode::Normalize, Type::Float));
	auto Swizzle = Testing::MakeGraphExpression<DMaterialExpressionSwizzle>();
	Swizzle->Input = {Source->Id}; Swizzle->Components = {0};
	const std::array<DMaterialExpression*, 2> Expressions{Source.Get(), Swizzle.Get()};
	ASSERT_TRUE(FMaterialExpressionBuildContext::ValidateSurface(Expressions, {}));
	Swizzle->Components = {1};
	EXPECT_FALSE(FMaterialExpressionBuildContext::ValidateSurface(Expressions, {}));
	Swizzle->Components = {};
	EXPECT_FALSE(FMaterialExpressionBuildContext::ValidateSurface(Expressions, {}));
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
				&& Entry.ResultType == EMaterialProgramValueType::Float3;
		});
	ASSERT_NE(Multiply, Catalog.end());

	const auto Before = CaptureExpressions(*Material);
	Durin::Tests::FTestTransactorOwner Transactions;
	const FMaterialGraphCommandResult Created =
		FMaterialGraphDocument(*Material).CreateCatalogNode(*Multiply, 400, 200, {}, Transactions.Get());
	ASSERT_TRUE(Created) << Created.Message;
	ASSERT_EQ(Created.GeneratedNodeIds.size(), 1u);
	const FMaterialGraphView View = FMaterialGraphOperations::Inspect(*Material);
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

TEST(FMaterialGraphOperationsTests, HiddenAdvancedPinsRetainStableIdentitiesAndRevealBindings)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "AdvancedPins");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	auto* Function = NewObject<DMaterialFunction>(nullptr, "AdvancedFunction");
	FMaterialGraphDocument FunctionDocument(*Function), Document(*Material);
	const FGuid Advanced = FGuid::NewGuid(), Visible = FGuid::NewGuid();
	ASSERT_TRUE(FunctionDocument.AddPort(false, {.Id = Advanced, .Name = "Advanced", .bAdvanced = true,
		.Default = {.Kind = EMaterialFunctionDefaultKind::Numeric}}));
	ASSERT_TRUE(FunctionDocument.AddPort(false, {.Id = Visible, .Name = "Visible", .DisplayOrder = 1,
		.Default = {.Kind = EMaterialFunctionDefaultKind::Numeric}}));
	const auto Call = Document.InsertFunctionCall(*Function, 0, 0);
	ASSERT_TRUE(Call);
	EXPECT_TRUE(std::ranges::find(Function->GetFunctionSignature().Inputs, Advanced, &FMaterialFunctionPort::Id)->bAdvanced);
	FMaterialGraphCanvas Canvas;
	FMaterialGraphCanvasTestAccess::HideAdvanced(Canvas);
	const auto& Hidden = FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material);
	const auto* CallView = FindViewNode(Hidden, Call.GeneratedNodeIds[0]);
	ASSERT_NE(CallView, nullptr);
	ASSERT_EQ(CallView->Inputs.size(), 2u); // The default Surface input remains visible.
	EXPECT_EQ(CallView->Inputs.back().PortId, Visible);
	EXPECT_EQ(CallView->Inputs.back().InputIndex, 2u);
	ASSERT_TRUE(Document.SetInputDefault(Call.GeneratedNodeIds[0], 0,
		{.Kind = EMaterialInputDefaultKind::Literal, .Literal = {.X = .7f}}, Advanced));
	const auto& Revealed = FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material);
	CallView = FindViewNode(Revealed, Call.GeneratedNodeIds[0]);
	ASSERT_EQ(CallView->Inputs.size(), 3u);
	EXPECT_NE(std::ranges::find(CallView->Inputs, Advanced, &FMaterialGraphPinView::PortId), CallView->Inputs.end());
	EXPECT_EQ(CallView->Inputs.back().PortId, Visible);
	MarkAsGarbage(Material);
	MarkAsGarbage(Function);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, TextureOutputsHideUnusedAdvancedPinsWithoutChangingLinks)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "CompactTextureOutputs");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto Added = FMaterialGraphOperations::AddTextureToSurfaceOutput(*Material, {});
	ASSERT_TRUE(Added);
	const auto SampleId = Added.GeneratedNodeIds.front();
	FMaterialGraphCanvas Canvas;
	const auto* Sample = FindViewNode(FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material), SampleId);
	ASSERT_NE(Sample, nullptr);
	ASSERT_EQ(Sample->Outputs.size(), 6u);
	constexpr std::array Names{"RGB", "R", "G", "B", "A", "RGBA"};
	constexpr std::array<uint8, 6> Indices{1, 2, 3, 4, 5, 0};
	for (size_t Index = 0; Index < Names.size(); ++Index)
	{
		EXPECT_EQ(Sample->Outputs[Index].Name, Names[Index]);
		EXPECT_EQ(Sample->Outputs[Index].OutputIndex, Indices[Index]);
	}
	FMaterialGraphCanvasTestAccess::ShowAdvanced(Canvas);
	Sample = FindViewNode(FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material), SampleId);
	ASSERT_EQ(Sample->Outputs.size(), 8u);
	EXPECT_EQ(Sample->Outputs[6].OutputIndex, 7u);
	EXPECT_EQ(Sample->Outputs[7].OutputIndex, 8u);
	FMaterialGraphCanvasTestAccess::HideAdvanced(Canvas);
	FMaterialGraphDocument Document(*Material);
	const auto SecondSample = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::TextureSample2D, EMaterialProgramValueType::Float4, {SampleId, 7});
	ASSERT_TRUE(SecondSample);
	const auto RG = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::Swizzle, EMaterialProgramValueType::Float2, {SampleId, 0});
	ASSERT_TRUE(RG);
	EXPECT_EQ(FindViewNode(Document.Inspect(), RG.GeneratedNodeIds.front())->PrimaryLabel, "Swizzle RG");
	const auto Decode = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::DecodeNormalRG, EMaterialProgramValueType::Float3, {RG.GeneratedNodeIds.front()});
	ASSERT_TRUE(Decode);
	const auto NormalizeCatalog = FMaterialGraphOperations::EnumerateCatalog();
	const auto NormalizeEntry = std::ranges::find_if(NormalizeCatalog, [](const auto& Entry) {
		return Entry.Opcode == EMaterialProgramOpcode::Normalize && Entry.ResultType == EMaterialProgramValueType::Float3;
	});
	ASSERT_NE(NormalizeEntry, NormalizeCatalog.end());
	const auto NormalConsumer = Document.CreateCatalogNode(*NormalizeEntry, 0, 0, {SampleId, 8});
	ASSERT_TRUE(NormalConsumer);
	const auto NormalView = Document.Inspect();
	ASSERT_EQ(FindViewNode(NormalView, NormalConsumer.GeneratedNodeIds.front())->Inputs.front().SourceType, EMaterialProgramValueType::Float3);
	ASSERT_TRUE(Document.RemoveNodes(NormalConsumer.GeneratedNodeIds));
	Sample = FindViewNode(FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material), SampleId);
	ASSERT_EQ(Sample->Outputs.size(), 7u);
	EXPECT_EQ(Sample->Outputs[6].OutputIndex, 7u);
	ASSERT_TRUE(Document.AssignMaterialOutput(EMaterialSurfaceOutput::Normal, {SampleId, 8}));
	Sample = FindViewNode(FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material), SampleId);
	ASSERT_EQ(Sample->Outputs.size(), 8u);
	EXPECT_EQ(Sample->Outputs.back().Name, "Normal");
	EXPECT_EQ(Sample->Outputs.back().OutputIndex, 8u);
	ASSERT_TRUE(Document.AssignMaterialOutput(EMaterialSurfaceOutput::Normal, {}));
	const std::array Consumers{SecondSample.GeneratedNodeIds.front(), Decode.GeneratedNodeIds.front()};
	ASSERT_TRUE(FMaterialGraphOperations::RemoveNodes(*Material, Consumers));
	Sample = FindViewNode(FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material), SampleId);
	EXPECT_EQ(Sample->Outputs.size(), 6u);
	EXPECT_EQ(Material->GetExpressionOutputs().BaseColor.OutputIndex, 1u);
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
	ASSERT_TRUE(Created) << Created.Message;
	const auto Id = Created.GeneratedNodeIds[0];
	const auto Before = CaptureExpressions(*Material);
	const auto Extracted = Document.ExtractInputDefault(Id, 1, {}, Transactions.Get());
	ASSERT_TRUE(Extracted) << Extracted.Message;
	const auto ExtractedId = Extracted.GeneratedNodeIds[0];
	ASSERT_TRUE(Document.ConnectInput(Id, 0, {ExtractedId}, false, Transactions.Get()));
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
		ASSERT_TRUE(Material->SetMaterialGraphPresentation({.Nodes = {{Id, 400, 100}}}));
		const FMaterialInputDefault Value{.Kind = EMaterialInputDefaultKind::Literal,
			.Type = Add->ResultType, .Literal = {2, 3, 4, 5}};
		ASSERT_TRUE(Document.SetInputDefault(Id, 1, Value));
		EXPECT_EQ(Document.SetInputDefault(Id, 1, Value).Status, EMaterialGraphCommandStatus::NoChange);
		const auto Extracted = Document.ExtractInputDefault(Id, 1);
		ASSERT_TRUE(Extracted) << Extracted.Message;
		ASSERT_EQ(Material->GetExpressionCollection().Expressions.size(), 2u);
		EXPECT_EQ(Material->GetExpressionCollection().Expressions.back()->GetClass(),
			(std::array{DMaterialExpressionScalarConstant::StaticClass(), DMaterialExpressionVector2Constant::StaticClass(),
				DMaterialExpressionVector3Constant::StaticClass(), DMaterialExpressionVector4Constant::StaticClass()})[Width - 1]);
		ASSERT_TRUE(Document.InlineInputNode(Id, 1));
		ASSERT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
		const auto* Result = Cast<DMaterialExpressionAdd>(Material->GetExpressionCollection().Expressions.front().Get());
		ASSERT_NE(Result, nullptr);
		ASSERT_EQ(Result->BDefault.size(), Width);
		for (uint32 Index = 0; Index < Width; ++Index) EXPECT_FLOAT_EQ(Result->BDefault[Index], 2.f + Index);
	}
	auto* Coordinates = NewObject<DMaterialExpressionTextureCoordinates>(nullptr, NAME_None);
	Coordinates->Id = FGuid::NewGuid();
	const auto CoordinatesId = Coordinates->Id;
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 1>{Coordinates}, {}));
	ASSERT_TRUE(Material->SetMaterialGraphPresentation({.Nodes = {{CoordinatesId, 400, 100}}}));
	EXPECT_FALSE(Document.SetInputDefault(CoordinatesId, 0, {.Kind = EMaterialInputDefaultKind::Literal,
		.Type = EMaterialProgramValueType::Float3, .Literal = {2, 3, 4}}));
	ASSERT_TRUE(Document.SetInputDefault(CoordinatesId, 0, {.Kind = EMaterialInputDefaultKind::Literal,
		.Type = EMaterialProgramValueType::Float, .Literal = {2}}, {}, Transactions.Get()));
	ASSERT_TRUE(Document.ExtractInputDefault(CoordinatesId, 0, {}, Transactions.Get()));
	ASSERT_TRUE(Document.InlineInputNode(CoordinatesId, 0, {}, Transactions.Get()));
	EXPECT_EQ(Cast<DMaterialExpressionTextureCoordinates>(Material->GetExpressionCollection().Expressions.front().Get())->ChannelDefault, (std::vector<float>{2}));
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
	Input->Id = FGuid::NewGuid(); Input->PortId = InputId;
	auto* Output = NewObject<DMaterialExpressionFunctionOutput>(nullptr, NAME_None);
	Output->Id = FGuid::NewGuid(); Output->PortId = OutputId; Output->Source = {Input->Id};
	ASSERT_TRUE(Function->SetFunctionExpressions(Signature, std::array<DMaterialExpression*, 2>{Input, Output}));
	ASSERT_TRUE(Material->SetMaterialExpressions({}, {}));
	const auto Created = Document.InsertFunctionCall(*Function, 400, 100);
	ASSERT_TRUE(Created) << Created.Message;
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
	ASSERT_TRUE(Created) << Created.Message;
	const auto Id = Created.GeneratedNodeIds[0];
	const auto CreatedUV = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::TextureCoordinates,
		EMaterialProgramValueType::Float2, {}, 0, 0, Transactions.Get());
	ASSERT_TRUE(CreatedUV) << CreatedUV.Message;
	const auto CoordinatesId = CreatedUV.GeneratedNodeIds[0];
	ASSERT_TRUE(Document.SetInputDefault(CoordinatesId, 0, {.Kind = EMaterialInputDefaultKind::Literal,
		.Type = EMaterialProgramValueType::Float, .Literal = {1}}, {}, Transactions.Get()));
	const auto Before = CaptureExpressions(*Material);
	ASSERT_TRUE(Document.ConnectInput(Id, 0, {CoordinatesId}, false, Transactions.Get()));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	ASSERT_TRUE(Transactions->Redo());
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(Document.CopySelection(std::array{Id, CoordinatesId}, Payload));
	EXPECT_EQ(Payload.Nodes.size(), 2u);
	const auto Pasted = FMaterialGraphDocument(*Target).Paste(Payload, 0, 0);
	ASSERT_TRUE(Pasted) << Pasted.Message;
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

TEST(FMaterialGraphOperationsTests, PublicTypedCandidatesCannotMutateOwnerOrHistoryAfterCommit)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	FMaterialGraphDocument Document(*Material.Get());
	const auto Created = Testing::CreateGraphConstant(Document, 0.25f);
	ASSERT_TRUE(Created);
	FMaterialGraphDocumentState Candidate;
	ASSERT_TRUE(Document.Capture(Candidate));
	auto* Constant = Cast<DMaterialExpressionScalarConstant>(Candidate.Expressions.front().Get());
	ASSERT_NE(Constant, nullptr);
	EXPECT_NE(Constant, Material->GetExpressionCollection().Expressions.front().Get());
	Constant->Value = 0.5f;
	Durin::Tests::FTestTransactorOwner Transactions;
	ASSERT_TRUE(Document.Commit(Candidate, "Edit captured constant", Transactions.Get()));
	Constant->Value = 0.9f;
	const auto Read = [&]() { return Cast<DMaterialExpressionScalarConstant>(Material->GetExpressionCollection().Expressions.front().Get())->Value; };
	EXPECT_FLOAT_EQ(Read(), 0.5f);
	CollectGarbage();
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FLOAT_EQ(Read(), 0.25f);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_FLOAT_EQ(Read(), 0.5f);
	const auto Revision = Material->GetMaterialCompileStatus().AuthoredRevision;
	Candidate.bFunction = true;
	EXPECT_FALSE(Document.Commit(Candidate, "Invalid document kind", Transactions.Get()));
	Candidate.bFunction = false;
	Candidate.Expressions.emplace_back();
	EXPECT_FALSE(Document.Commit(Candidate, "Missing expression", Transactions.Get()));
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
	ASSERT_TRUE(Inserted) << Inserted.Message;
	const auto Revision = Caller->GetFunctionRevision();
	TStrongObjectPtr<DMaterialExpressionFunctionCall> Replacement(NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None));
	Replacement->Id = Inserted.GeneratedNodeIds.front();
	Replacement->Function = Caller.Get();
	for (const auto& Port : Caller->GetFunctionSignature().Outputs) Replacement->Outputs.push_back({Port.Id, Port.Type});
	const auto Rejected = Document.ReplaceExpression(*Replacement.Get(), Transactions.Get());
	EXPECT_FALSE(Rejected);
	EXPECT_NE(Rejected.Message.find("recursive"), std::string::npos);
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
	ASSERT_TRUE(Created) << Created.Message;
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
	ASSERT_TRUE(FMaterialGraphOperations::AssignSurfaceOutput(*Material,
		{.Output = EMaterialSurfaceOutput::Roughness, .SourceNodeId = NodeId}, Transactions.Get()));
	const auto Connected = CaptureExpressions(*Material);
	const auto Revision = Material->GetMaterialCompileStatus().AuthoredRevision;
	ResultType = EMaterialProgramValueType::Float4;
	EXPECT_FALSE(ApplyValue());
	EXPECT_EQ(CaptureExpressions(*Material), Connected);
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision, Revision);
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
	ASSERT_TRUE(Created) << Created.Message;
	ASSERT_EQ(Created.GeneratedNodeIds.size(), 1u);
	const auto OwnerId = Created.GeneratedNodeIds.front();
	const auto ParameterId = Created.AffectedParameterIds.front();
	Durin::Tests::FTestTransactorOwner Transactions;
	FMaterialGraphDocument Document(*Material);
	ASSERT_TRUE(Document.AssignMaterialOutput(EMaterialSurfaceOutput::Metallic, {OwnerId}, Transactions.Get()));
	ASSERT_TRUE(Document.AssignMaterialOutput(EMaterialSurfaceOutput::Roughness, {OwnerId}, Transactions.Get()));
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 1u);
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
	TStrongObjectPtr<DMaterialExpression> DuplicateOwner(DuplicateObject(Material->GetExpressionCollection().Expressions.front().Get(), nullptr, NAME_None));
	DuplicateOwner->Id = FGuid::NewGuid();
	ASSERT_TRUE(Document.CreateExpression(*DuplicateOwner.Get()));
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 1u);
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 2u);
	ASSERT_TRUE(FMaterialGraphOperations::RenameParameter(*Material, ParameterId, "RenamedShared", Transactions.Get()));
	EXPECT_EQ(Material->FindParameterDefinition("RenamedShared")->Id, ParameterId);
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.front()->Id, OwnerId);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_NE(Material->FindParameterDefinition("SharedValue"), nullptr);
	ASSERT_TRUE(Transactions->Redo());
	const auto View = FMaterialGraphOperations::Inspect(*Material);
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
	auto Entries = FMaterialGraphOperations::SearchCatalog("parameter");
	const auto IsParameter = [](const FMaterialGraphCatalogEntry& Entry) {
		return Entry.Opcode == EMaterialProgramOpcode::Parameter
			|| Entry.Opcode == EMaterialProgramOpcode::TextureParameter;
	};
	std::erase_if(Entries, [&](const auto& Entry) { return !IsParameter(Entry); });
	ASSERT_EQ(Entries.size(), 5u);
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
		ASSERT_TRUE(Created) << Created.Message;
		ASSERT_EQ(Created.GeneratedNodeIds.size(), 1u);
		ASSERT_EQ(Created.AffectedParameterIds.size(), 1u);
		EXPECT_EQ(Material->GetParameterDefinitions().size(), BeforeDefinitions.size() + 1);
		const auto After = CaptureExpressions(*Material);
		const auto AfterPresentation = Material->GetMaterialGraphPresentation();
		const auto* Parameter = Cast<DMaterialExpressionParameter>(After.Expressions.back().Get());
		ASSERT_NE(Parameter, nullptr);
		EXPECT_EQ(Parameter->Metadata.Id, Created.AffectedParameterIds.front());
		EXPECT_EQ(Parameter->GetClass(), Entry.ExpressionClass);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(CaptureExpressions(*Material), Before);
		EXPECT_EQ(Material->GetMaterialGraphPresentation(), BeforePresentation);
		EXPECT_TRUE(std::ranges::equal(Material->GetParameterDefinitions(), BeforeDefinitions));
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(CaptureExpressions(*Material), After);
		EXPECT_EQ(Material->GetMaterialGraphPresentation(), AfterPresentation);

		const auto Second = FMaterialGraphDocument(*Material).CreateCatalogNode(Entry, 0, 0, {}, Transactions.Get());
		ASSERT_TRUE(Second) << Second.Message;
		EXPECT_NE(Second.AffectedParameterIds.front(), Created.AffectedParameterIds.front());
		const auto* FirstDefinition = Material->FindParameterDefinition(Created.AffectedParameterIds.front());
		const auto* SecondDefinition = Material->FindParameterDefinition(Second.AffectedParameterIds.front());
		ASSERT_NE(FirstDefinition, nullptr);
		ASSERT_NE(SecondDefinition, nullptr);
		EXPECT_NE(FirstDefinition->Name, SecondDefinition->Name);
		TStrongObjectPtr<DMaterialExpression> SharedNode(DuplicateObject(Material->GetExpressionCollection().Expressions.back().Get(), nullptr, NAME_None));
		Cast<DMaterialExpressionParameter>(SharedNode.Get())->Metadata.Id = FirstDefinition->Id;
		EXPECT_FALSE(FMaterialGraphDocument(*Material).ReplaceExpression(*SharedNode.Get(), Transactions.Get()));
		EXPECT_EQ(Cast<DMaterialExpressionParameter>(Material->GetExpressionCollection().Expressions.back().Get())->Metadata.Id, Second.AffectedParameterIds.front());
		EXPECT_EQ(std::ranges::count_if(FMaterialGraphOperations::SearchCatalog("parameter"), IsParameter), 5);
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
		{.Surface = {CallId, 0, OutputPort}}));
	ASSERT_TRUE(Material->SetMaterialGraphPresentation({.Nodes = {{ConstantId, 100, 100, "Retained label"}}}));
	const auto Revision = Material->GetMaterialCompileStatus().AuthoredRevision;
	Durin::Tests::FTestTransactorOwner Transactions;
	ASSERT_TRUE(FMaterialGraphOperations::Layout(*Material, {}, Transactions.Get()));
	const auto Layout = Material->GetMaterialGraphPresentation();
	const auto Position = [&](FGuid Id) -> const FMaterialGraphNodePresentation& {
		return *std::ranges::find(Layout.Nodes, Id, &FMaterialGraphNodePresentation::NodeId);
	};
	EXPECT_LT(Position(BaseId).X, Position(OverrideId).X);
	EXPECT_LT(Position(ConstantId).X, Position(OverrideId).X);
	EXPECT_LT(Position(OverrideId).X, Position(CallId).X);
	EXPECT_LT(Position(CallId).X, Layout.MaterialOutputX);
	EXPECT_EQ(Position(ConstantId).DisplayName, "Retained label");
	EXPECT_EQ(FMaterialGraphOperations::Layout(*Material).Status, EMaterialGraphCommandStatus::NoChange);
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
	while (Graph.Expressions.size() < MaterialProgramMaxNodeCount)
		Graph.Expressions.emplace_back(Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>().Get());
	ASSERT_TRUE(Graph.Apply(*Material));
	const auto MaximumGraph = CaptureExpressions(*Material);
	const uint64 SemanticRevision =
		Material->GetMaterialCompileStatus().AuthoredRevision;

	const FMaterialGraphCommandResult First =
		FMaterialGraphOperations::Layout(*Material);
	ASSERT_TRUE(First) << First.Message;
	EXPECT_EQ(Material->GetMaterialGraphPresentation().Nodes.size(),
		MaterialProgramMaxNodeCount);
	EXPECT_EQ(CaptureExpressions(*Material), MaximumGraph);
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
	EXPECT_TRUE(Material->GetExpressionCollection().Expressions.empty());
	EXPECT_FALSE(Material->GetExpressionOutputs().Surface.ExpressionId.IsValid());
	EXPECT_EQ(Presentation.Nodes.size(),
		Material->GetExpressionCollection().Expressions.size());
	EXPECT_TRUE(Presentation.bHasMaterialOutputPosition);
	const FMaterialGraphView View = FMaterialGraphOperations::Inspect(*Material);
	EXPECT_EQ(View.Nodes.size(), Material->GetExpressionCollection().Expressions.size());
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
	auto Constant = Testing::MakeGraphExpression<DMaterialExpressionVector3Constant>();
	Constant->Value = {.2f, .4f, .6f};
	auto Saturate = Testing::MakeGraphExpression<DMaterialExpressionSaturate>();
	Saturate->ResultType = EMaterialProgramValueType::Float3;
	Saturate->Input = {Constant->Id};
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 2>{Constant.Get(), Saturate.Get()}, {.BaseColor = {Saturate->Id}}));
	ASSERT_TRUE(FMaterialGraphOperations::Layout(*Material));
	std::vector<FGuid> AllNodes;
	for (const auto& Node : Material->GetExpressionCollection().Expressions) AllNodes.push_back(Node->Id);
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

	const auto BeforeProgram = CaptureExpressions(*Material);
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
	EXPECT_EQ(CaptureExpressions(*Material), BeforeProgram);
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), BeforePresentation);
	ASSERT_TRUE(Transactions->Redo());

	FMaterialGraphClipboardPayload UnknownVersion = Payload;
	UnknownVersion.SchemaVersion = 99;
	const auto BeforeRejected = CaptureExpressions(*Material);
	const FMaterialGraphCommandResult Rejected = FMaterialGraphOperations::Paste(
		*Material, UnknownVersion, 0, 0, Transactions.Get());
	EXPECT_EQ(Rejected.Status, EMaterialGraphCommandStatus::Rejected);
	EXPECT_EQ(CaptureExpressions(*Material), BeforeRejected);

	const auto* Dependent = FindExpression<DMaterialExpressionSaturate>(*Material, Saturate->Id);
	ASSERT_NE(Dependent, nullptr);
	const FGuid RequiredSource = Dependent->Input.ExpressionId;
	const FGuid DependentId = Dependent->Id;
	const auto ExternalInput = Dependent->Input;
	const FMaterialGraphCommandResult RequiredRemoval =
		FMaterialGraphOperations::RemoveNodes(
			*Material, std::span(&RequiredSource, 1), Transactions.Get());
	EXPECT_EQ(RequiredRemoval.Status, EMaterialGraphCommandStatus::Rejected);
	EXPECT_EQ(CaptureExpressions(*Material), BeforeRejected);
	FMaterialGraphClipboardPayload Partial;
	ASSERT_TRUE(FMaterialGraphOperations::CopySelection(
		*Material, std::span(&DependentId, 1), Partial));
	ASSERT_EQ(Partial.Nodes.size(), 1u);
	const auto* CopiedExpression = Cast<DMaterialExpressionSaturate>(Partial.Nodes.front().Expression.Get());
	ASSERT_NE(CopiedExpression, nullptr);
	EXPECT_EQ(CopiedExpression->Input, ExternalInput);
	const FMaterialGraphCommandResult PartialPaste =
		FMaterialGraphOperations::Paste(*Material, Partial, 0, 0, Transactions.Get());
	ASSERT_TRUE(PartialPaste) << PartialPaste.Message;
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
	EXPECT_EQ(CutPayload.Nodes.front().Expression->Id, StandaloneId);
	EXPECT_EQ(FindViewNode(FMaterialGraphOperations::Inspect(*Material), StandaloneId),
		nullptr);
	ASSERT_TRUE(Transactions->Undo());
	const FMaterialGraphView RestoredCut = FMaterialGraphOperations::Inspect(*Material);
	EXPECT_NE(FindViewNode(RestoredCut, StandaloneId), nullptr);

	EXPECT_TRUE(Transactions->Reset());
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
	auto Parameter = Testing::MakeGraphExpression<DMaterialExpressionScalarParameter>(ParameterId);
	Parameter->Metadata.Id = Definition.Id; Parameter->Metadata.Name = Definition.Name;
	Parameter->DefaultValue = .5f;
	auto Saturate = Testing::MakeGraphExpression<DMaterialExpressionSaturate>();
	Saturate->Input = {ParameterId};
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 2>{Parameter.Get(), Saturate.Get()}, {}));
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
		[](const auto& Node) { return Node.Node.GetParameterId().IsValid(); });
	ASSERT_NE(ParameterNode, View.Nodes.end());
	ASSERT_TRUE(FMaterialGraphOperations::RenameParameter(*Material, ParameterNode->Node.GetParameterId(),
		FName("RenamedCanvasParameter")));
	FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material);
	EXPECT_EQ(FindViewNode(View, ParameterId)->PrimaryLabel,
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
	ASSERT_TRUE(Pasted) << Pasted.Message;
	EXPECT_NE(Pasted.GeneratedNodeIds[0], Call.GeneratedNodeIds[0]);
	EXPECT_EQ(Target->GetExpressionOutputs().Surface.OutputId, OutputId);
	EXPECT_EQ(Target->GetExpressionOutputs().Surface.ExpressionId, Pasted.GeneratedNodeIds[0]);
	const auto* PastedCall = FindExpression<DMaterialExpressionFunctionCall>(*Target, Pasted.GeneratedNodeIds[0]);
	ASSERT_NE(PastedCall, nullptr); EXPECT_EQ(PastedCall->Function.Get(), WeakFunction.Get());
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Target), Before);
	EXPECT_TRUE(Target->GetExpressionCollection().Expressions.empty());
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
	ASSERT_TRUE(Pasted) << Pasted.Message;
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

TEST(FMaterialGraphOperationsTests, FunctionCanvasConnectsAndMovesNodesWithUndo)
{
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "InteractiveFunction");
	FMaterialGraphDocument Document(*Function);
	const auto Constant = Testing::CreateGraphConstant(Document, 0.5f, 0, 300);
	ASSERT_TRUE(Constant);
	const auto Other = Testing::CreateGraphConstant(Document, 0.9f, 0, 550);
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
	const auto* Output = FindExpression<DMaterialExpressionFunctionOutput>(*Function, Port.GeneratedNodeIds[0]);
	ASSERT_NE(Output, nullptr);
	EXPECT_EQ(Output->Source.ExpressionId, Other.GeneratedNodeIds[0]);
	ASSERT_TRUE(Transactions->Undo());
	const auto Before = Function->GetFunctionPresentation();
	const auto Revision = Function->GetFunctionRevision();
	const ImVec2 Header{Origin.x + 20, Origin.y + 300 + 12};
	Frame(Header, false); Frame(Header, true); Frame({Header.x + 60, Header.y + 40}, true); Frame({Header.x + 60, Header.y + 40}, false);
	EXPECT_NE(Function->GetFunctionPresentation(), Before);
	EXPECT_EQ(Function->GetFunctionRevision(), Revision);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Function->GetFunctionPresentation(), Before);
	EXPECT_EQ(Function->GetFunctionRevision(), Revision);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_NE(Function->GetFunctionPresentation(), Before);
	EXPECT_EQ(Function->GetFunctionRevision(), Revision);
	ImGui::DestroyContext(Context);
	EXPECT_TRUE(Transactions->Reset()); MarkAsGarbage(Function); CollectGarbage();
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
	const auto Constant = Testing::CreateGraphConstant(Document, 0.f, 0, 300);
	ASSERT_TRUE(Constant);
	auto SurfaceExpression = Testing::MakeGraphExpression<DMaterialExpressionSetSurfaceAttributes>();
	SurfaceExpression->Surface = {Call.GeneratedNodeIds[0], 0, Function->GetFunctionSignature().Outputs[0].Id};
	SurfaceExpression->Attributes = {{EMaterialSurfaceOutput::Metallic, {Constant.GeneratedNodeIds[0]}}};
	const auto Surface = Document.CreateExpression(*SurfaceExpression.Get(), 0, 450);
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
	const auto Destination = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::Saturate, EMaterialProgramValueType::Float, {Constant.GeneratedNodeIds[0]}, 350);
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
	const auto Link = FindViewNode(FMaterialGraphOperations::Inspect(*Material), Destination.GeneratedNodeIds[0])->Inputs[0].Link;
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
	auto Previous = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>(PreviousSource);
	auto Current = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>(Source);
	auto Consumer = Testing::MakeGraphExpression<DMaterialExpressionSaturate>(Destination);
	Consumer->Input = {PreviousSource};
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 3>{Previous.Get(), Current.Get(), Consumer.Get()}, {}));
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
		->Inputs.front().Link.SourceNodeId, Source);
	Drop({Origin.x + 700, Origin.y + FMaterialGraphGeometry::GetSurfacePinOffset(3)}, false);
	EXPECT_EQ(Material->GetExpressionOutputs().Roughness.ExpressionId, Source);
	Drop({Origin.x + 700, Origin.y + FMaterialGraphGeometry::GetSurfacePinOffset(0)}, false);
	EXPECT_EQ(Errors, 2);
	Drop({Origin.x + 380, Origin.y + 10}, false);
	Frame(Output, false);
	Frame(Output, true);
	Frame({500, 500}, true);
	Frame({500, 500}, false);
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::Menu(Canvas));
	Canvas.CancelInteraction();
	EXPECT_TRUE(Transactions->Reset());
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

	Testing::FTestMaterialExpressionGraph Graph;
	std::array<FGuid, 8> DenseSources;
	for (uint32 Index = 0; Index < DenseSources.size(); ++Index)
		DenseSources[Index] = FGuid(300 + Index, 0, 0, 1);
	for (uint32 Index = 0; Index < DenseSources.size(); ++Index)
	{
		Graph.Expressions.emplace_back(Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>(DenseSources[Index]).Get());
		auto Consumer = Testing::MakeGraphExpression<DMaterialExpressionSaturate>(FGuid(400 + Index, 0, 0, 1));
		Consumer->Input = {DenseSources[DenseSources.size() - Index - 1]};
		Graph.Expressions.emplace_back(Consumer.Get());
	}
	Graph.Presentation.Nodes = {{DenseSources[0], 0, 0, "Ambient Occlusion Texture With A Deliberately Long Authored Name"}};
	ASSERT_TRUE(Graph.Apply(*Material));
	ASSERT_TRUE(FMaterialGraphOperations::Layout(*Material));
	const int DenseVertices = DrawAtZoom(0.55f);
	EXPECT_GT(DenseVertices, 100);
	EXPECT_LT(DenseVertices, 100000);

	uint32 MaximumIndex = 1000;
	while (Graph.Expressions.size() < MaterialProgramMaxNodeCount)
		Graph.Expressions.emplace_back(Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>(FGuid(MaximumIndex++, 0, 0, 1)).Get());
	ASSERT_TRUE(Graph.Apply(*Material));
	ASSERT_TRUE(FMaterialGraphOperations::Layout(*Material));
	const int MaximumVertices = DrawAtZoom(0.30f);
	EXPECT_GT(MaximumVertices, 100);
	EXPECT_LT(MaximumVertices, 100000);

	EXPECT_TRUE(Transactions->Reset());
	ImGui::DestroyContext(Context);
	MarkAsGarbage(Material);
	CollectGarbage();
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
	ASSERT_TRUE(Created) << Created.Message;
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
		FMaterialGraphOperations::MoveNodes(*Material, std::span(&Moved, 1), Transactions.Get());
	ASSERT_TRUE(Move) << Move.Message;
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision,
		SemanticRevision);
	const FMaterialGraphView MovedGraph = FMaterialGraphOperations::Inspect(*Material);
	const FMaterialGraphNodeView* MovedView = FindViewNode(MovedGraph, CreatedId);
	ASSERT_NE(MovedView, nullptr);
	EXPECT_EQ(MovedView->Presentation.X, 320);

	const auto BeforeRejected = CaptureExpressions(*Material);
	ASSERT_NE(FindExpression<DMaterialExpressionScalarConstant>(*Material, CreatedId), nullptr);
	const FMaterialGraphCommandResult Rejected = FMaterialGraphDocument(*Material).SetConstantValue(CreatedId,
		FMaterialParameterValue::MakeScalar(std::numeric_limits<float>::quiet_NaN()), Transactions.Get());
	EXPECT_EQ(Rejected.Status, EMaterialGraphCommandStatus::Rejected);
	EXPECT_FALSE(Rejected.Diagnostics.empty());
	EXPECT_EQ(CaptureExpressions(*Material), BeforeRejected);
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision,
		SemanticRevision);

	ASSERT_TRUE(Transactions->Undo());
	const FMaterialGraphView UnmovedGraph = FMaterialGraphOperations::Inspect(*Material);
	const FMaterialGraphNodeView* UnmovedView = FindViewNode(UnmovedGraph, CreatedId);
	ASSERT_NE(UnmovedView, nullptr);
	EXPECT_EQ(UnmovedView->Presentation.X, 120);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), OriginalGraph);
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
	ASSERT_TRUE(Created) << Created.Message;
	ASSERT_EQ(Created.GeneratedNodeIds.size(), 1u);
	const FGuid NodeId = Created.GeneratedNodeIds.front();

	FMaterialGraphMoveSession MoveSession;
	ASSERT_TRUE(MoveSession.Begin(*Material, std::span(&NodeId, 1)));
	const FMaterialGraphNodePresentation Preview{NodeId, 400, 240};
	ASSERT_TRUE(MoveSession.Apply(std::span(&Preview, 1)));

	FMaterialGraphDocument Document(*Material);
	FMaterialGraphDocumentState Candidate;
	ASSERT_TRUE(Document.Capture(Candidate));
	Candidate.Outputs.RoughnessDefault = 0.37f;
	auto Validation = Document.Commit(std::move(Candidate), "Change roughness default");
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
	EXPECT_FLOAT_EQ(Material->GetExpressionOutputs().RoughnessDefault,
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
	ASSERT_TRUE(Material->SetMaterialExpressions({}, {}));
	ASSERT_TRUE(Material->GetExpressionCollection().Expressions.empty());
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
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
	EXPECT_TRUE(Material->GetExpressionOutputs().BaseColor.ExpressionId.IsValid());
	FVector3 BaseColor;
	ASSERT_TRUE(Material->GetVectorParameterValue(
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
		FMaterialParameterValue::MakeVector({0.7, 0.6, 0.5}),
		Transactions.Get()));
	EXPECT_EQ(Material->GetMaterialCompileStatus().RequestGeneration,
		CompileGeneration);
	ASSERT_TRUE(Material->GetVectorParameterValue(
		Material->FindParameterDefinition(Promoted.AffectedParameterIds.front())->Name, BaseColor));
	EXPECT_EQ(BaseColor, FVector3(0.7, 0.6, 0.5));
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Material->GetVectorParameterValue(
		Material->FindParameterDefinition(Promoted.AffectedParameterIds.front())->Name, BaseColor));
	EXPECT_NEAR(BaseColor.x, 0.2, 1.e-6);
	EXPECT_NEAR(BaseColor.y, 0.3, 1.e-6);
	EXPECT_NEAR(BaseColor.z, 0.4, 1.e-6);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Transactions->Undo());

	ASSERT_TRUE(Transactions->Undo());
	EXPECT_TRUE(Material->GetExpressionCollection().Expressions.empty());
	EXPECT_TRUE(Material->GetParameterDefinitions().empty());
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(FMaterialGraphOperations::DisconnectSurfaceOutput(
		*Material, EMaterialSurfaceOutput::BaseColor, Transactions.Get()));
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
	ASSERT_TRUE(Textured) << Textured.Message;
	ASSERT_EQ(Textured.GeneratedNodeIds.size(), 1u);
	EXPECT_EQ(Material->GetExpressionOutputs().Normal.OutputIndex, 8u);
	const FMaterialNormalizationResult Normalized = Normalize(*Material);
	ASSERT_TRUE(Normalized);
	EXPECT_EQ(std::ranges::count(Normalized.IR.Nodes, EMaterialProgramOpcode::TextureSample2D,
		&FMaterialIRNode::Opcode), 1);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FALSE(Material->GetExpressionOutputs().Normal.ExpressionId.IsValid());

	EXPECT_TRUE(Transactions->Reset());
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, SurfaceTexturesUseCompactSamplesAndPreserveUndo)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	auto* Material = NewObject<DMaterial>(nullptr, "CompactSurfaceTextures");
	ASSERT_NE(Material, nullptr);
	// Keep every output and Undo/Redo assertion without compiling each intermediate graph.
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	Durin::Tests::FTestTransactorOwner Transactions;
	constexpr std::array<uint8, 8> Channels{1, 8, 4, 3, 2, 1, 5, 2};
	for (uint32 Index = 0; Index < Channels.size(); ++Index)
	{
		SCOPED_TRACE(Index);
		const auto Role = static_cast<EMaterialSurfaceOutput>(Index);
		const bool bNormal = Role == EMaterialSurfaceOutput::Normal;
		const auto Result = FMaterialGraphOperations::AddTextureToSurfaceOutput(*Material,
			{.Output = Role, .X = 400, .Y = 200}, Transactions.Get());
		ASSERT_TRUE(Result) << Result.Message;
		ASSERT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
		ASSERT_EQ(Material->GetParameterDefinitions().size(), 1u);
		const auto* Sample = Cast<DMaterialExpressionTextureSampleParameter2D>(Material->GetExpressionCollection().Expressions.front().Get());
		ASSERT_NE(Sample, nullptr);
		EXPECT_FALSE(Sample->UV.ExpressionId.IsValid());
		const FGuid SampleId = Sample->Id;
		const FGuid ParameterId = Sample->Metadata.Id;
		const auto ReadOutput = [&]() {
			const auto& O = Material->GetExpressionOutputs();
			return (std::array{O.BaseColor, O.Normal, O.Metallic, O.Roughness, O.AmbientOcclusion, O.Emissive, O.Opacity, O.OpacityMask})[Index];
		};
		EXPECT_EQ(ReadOutput().ExpressionId, SampleId);
		EXPECT_EQ(ReadOutput().OutputIndex, Channels[Index]);
		if (bNormal)
		{
			EXPECT_EQ(Sample->TextureUsage, ETextureUsage::Normal);
			EXPECT_EQ(Sample->DefaultValue.TextureFallback, EMaterialTextureFallback::FlatRGNormal);
		}
		ASSERT_TRUE(Normalize(*Material));
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_TRUE(Material->GetExpressionCollection().Expressions.empty());
		EXPECT_TRUE(Material->GetParameterDefinitions().empty());
		EXPECT_FALSE(ReadOutput().ExpressionId.IsValid());
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(Material->GetExpressionCollection().Expressions.front()->Id, SampleId);
		EXPECT_EQ(Material->GetParameterDefinitions().front().Id, ParameterId);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_TRUE(Transactions->Reset());
	}
	ASSERT_TRUE(FMaterialGraphOperations::AddTextureToSurfaceOutput(*Material,
		{.Output = EMaterialSurfaceOutput::BaseColor, .X = 0, .Y = 0}));
	ASSERT_TRUE(FMaterialGraphOperations::AddTextureToSurfaceOutput(*Material,
		{.Output = EMaterialSurfaceOutput::Normal, .X = 320, .Y = 360}));
	auto Presentation = Material->GetMaterialGraphPresentation();
	Presentation.bHasMaterialOutputPosition = true;
	Presentation.MaterialOutputX = 760;
	Presentation.MaterialOutputY = 100;
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(std::move(Presentation)));
	ImGuiContext* Context = ImGui::CreateContext();
	auto& IO = ImGui::GetIO();
	IO.DisplaySize = {1200, 850}; IO.DeltaTime = 1.f / 60; IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault(); IO.Fonts->Build();
	{
		FMaterialGraphCanvas Canvas;
		Canvas.SetViewport(.85f, {30, 50});
		int Errors = 0;
		for (int Frame = 0; Frame < 2; ++Frame)
		{
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(IO.DisplaySize);
			ImGui::Begin("Compact Texture Authoring", nullptr, ImGuiWindowFlags_NoResize);
			Canvas.Draw(*Material, *Transactions.Get(), 780, [&](std::string) { ++Errors; });
			ImGui::End(); ImGui::Render();
		}
		EXPECT_EQ(Errors, 0);
	}
	ImGui::DestroyContext(Context);
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
	FVector3 OriginalBaseColor;
	ASSERT_TRUE(Material->GetVectorParameterValue(
		Material->FindParameterDefinition(ParameterId)->Name, OriginalBaseColor));
	Durin::Tests::FTestTransactorOwner Transactions;
	FMaterialGraphParameterEditSession Session;
	ASSERT_TRUE(Session.Begin(*Material, ParameterId, Transactions.Get()));
	ASSERT_TRUE(Session.Apply(FMaterialParameterValue::MakeVector(
		{0.4, 0.5, 0.6})));
	FVector3 BaseColor;
	ASSERT_TRUE(Material->GetVectorParameterValue(
		Material->FindParameterDefinition(ParameterId)->Name, BaseColor));
	EXPECT_EQ(BaseColor, FVector3(0.4, 0.5, 0.6));
	EXPECT_EQ(Material->GetMaterialCompileStatus().RequestGeneration,
		CompileGeneration);
	ASSERT_TRUE(Session.Apply(FMaterialParameterValue::MakeVector(
		{0.7, 0.8, 0.9})));
	ASSERT_TRUE(Material->GetVectorParameterValue(
		Material->FindParameterDefinition(ParameterId)->Name, BaseColor));
	EXPECT_EQ(BaseColor, FVector3(0.7, 0.8, 0.9));
	EXPECT_EQ(Material->GetMaterialCompileStatus().RequestGeneration,
		CompileGeneration);
	ASSERT_TRUE(Session.Commit());
	ASSERT_TRUE(FMaterialGraphOperations::MoveMaterialOutput(
		*Material, 713, -91));

	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Material->GetVectorParameterValue(
		Material->FindParameterDefinition(ParameterId)->Name, BaseColor));
	EXPECT_EQ(BaseColor, OriginalBaseColor);
	EXPECT_EQ(Material->GetMaterialGraphPresentation().MaterialOutputX, 713);
	EXPECT_EQ(Material->GetMaterialGraphPresentation().MaterialOutputY, -91);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Material->GetVectorParameterValue(
		Material->FindParameterDefinition(ParameterId)->Name, BaseColor));
	EXPECT_EQ(BaseColor, FVector3(0.7, 0.8, 0.9));

	ASSERT_TRUE(Session.Begin(*Material, ParameterId, Transactions.Get()));
	ASSERT_TRUE(Session.Apply(FMaterialParameterValue::MakeVector(
		{0.1, 0.1, 0.1})));
	ASSERT_TRUE(Session.Cancel());
	ASSERT_TRUE(Material->GetVectorParameterValue(
		Material->FindParameterDefinition(ParameterId)->Name, BaseColor));
	EXPECT_EQ(BaseColor, FVector3(0.7, 0.8, 0.9));
	EXPECT_EQ(Material->GetMaterialCompileStatus().RequestGeneration,
		CompileGeneration);

	EXPECT_TRUE(Transactions->Reset());
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphOperationsTests,
	CompactTransactionAccountingExcludesUnchangedProgramSnapshots)
{
	InitializeDObjectSystem();
	DMaterial* Material = MakeExpandedGraphMaterial("CompactGraphTransactions");
	ASSERT_NE(Material, nullptr);
	ASSERT_FALSE(Material->GetExpressionCollection().Expressions.empty());
	const FGuid NodeId = Material->GetExpressionCollection().Expressions.front()->Id;
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
		Material->GetExpressionOutputs().RoughnessDefault;
	ASSERT_TRUE(FMaterialGraphOperations::SetSurfaceDefault(*Material, {
		.Output = EMaterialSurfaceOutput::Roughness,
		.Value = {Roughness == 0.41f ? 0.42f : 0.41f, 0.0f, 0.0f, 0.0f}},
		Transactions.Get()));
	const size_t SemanticTransactionBytes = Transactions->GetOwnedBytes();
	EXPECT_GT(SemanticTransactionBytes, PresentationTransactionBytes);

	EXPECT_TRUE(Transactions->Reset());
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
	auto Node = Testing::MakeGraphExpression<DMaterialExpressionVector4Parameter>();
	Node->Metadata.Id = Definition.Id; Node->Metadata.Name = Definition.Name;
	Node->Metadata.DisplayName = Definition.DisplayName;
	Node->DefaultValue = Definition.Value.GetVector4();
	ASSERT_TRUE(Source->SetMaterialExpressions(std::array<DMaterialExpression*, 1>{Node.Get()}, {}));
	ASSERT_TRUE(FMaterialGraphOperations::Layout(*Source));
	ASSERT_TRUE(Target->SetMaterialExpressions({}, {}));
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(FMaterialGraphOperations::CopySelection(*Source, std::span(&Node->Id, 1), Payload));
	ASSERT_EQ(Payload.Nodes.size(), 1u);
	Durin::Tests::FTestTransactorOwner Transactions;
	const auto BeforeRevision = Target->GetMaterialCompileStatus().AuthoredRevision;
	const auto Pasted = FMaterialGraphOperations::Paste(*Target, Payload, 300, 200, Transactions.Get());
	ASSERT_TRUE(Pasted) << Pasted.Message;
	ASSERT_EQ(Target->GetParameterDefinitions().size(), 1u);
	const auto LocalId = Target->GetParameterDefinitions().front().Id;
	EXPECT_NE(LocalId, Definition.Id);
	EXPECT_EQ(Cast<DMaterialExpressionParameter>(Target->GetExpressionCollection().Expressions.front().Get())->Metadata.Id, LocalId);
	EXPECT_EQ(Target->GetMaterialCompileStatus().AuthoredRevision, BeforeRevision + 1);
	const auto AfterPresentation = Target->GetMaterialGraphPresentation();
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_TRUE(Target->GetParameterDefinitions().empty());
	EXPECT_TRUE(Target->GetExpressionCollection().Expressions.empty());
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Target->GetParameterDefinitions().front().Id, LocalId);
	EXPECT_EQ(Target->GetMaterialGraphPresentation(), AfterPresentation);
	ASSERT_TRUE(FMaterialGraphOperations::Paste(*Target, Payload, 600, 200));
	EXPECT_EQ(Target->GetParameterDefinitions().size(), 1u);
	EXPECT_EQ(Cast<DMaterialExpressionParameter>(Target->GetExpressionCollection().Expressions.back().Get())->Metadata.Id, LocalId);

	const auto BeforeProgram = CaptureExpressions(*Target);
	const auto BeforePresentation = Target->GetMaterialGraphPresentation();
	const auto Revision = Target->GetMaterialCompileStatus().AuthoredRevision;
	Cast<DMaterialExpressionParameter>(Payload.Nodes.front().Expression.Get())->Metadata.Name = {};
	EXPECT_FALSE(FMaterialGraphOperations::Paste(*Target, Payload, 0, 0));
	EXPECT_EQ(CaptureExpressions(*Target), BeforeProgram);
	EXPECT_EQ(Target->GetMaterialGraphPresentation(), BeforePresentation);
	EXPECT_EQ(Target->GetMaterialCompileStatus().AuthoredRevision, Revision);
	Cast<DMaterialExpressionParameter>(Payload.Nodes.front().Expression.Get())->Metadata = {};
	EXPECT_FALSE(FMaterialGraphOperations::Paste(*Target, Payload, 0, 0));
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
	ASSERT_TRUE(FMaterialGraphOperations::Layout(*Material));
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(FMaterialGraphOperations::CopySelection(*Material, std::span(&Node->Id, 1), Payload));
	ASSERT_TRUE(FMaterialGraphOperations::RenameParameter(*Material, Definition.Id, "RenamedAmount"));
	ASSERT_TRUE(FMaterialGraphOperations::Paste(*Material, Payload, 400, 0));
	EXPECT_NE(Cast<DMaterialExpressionParameter>(Material->GetExpressionCollection().Expressions.back().Get())->Metadata.Id, Definition.Id);
	EXPECT_EQ(Cast<DMaterialExpressionParameter>(Material->GetExpressionCollection().Expressions.back().Get())->Metadata.Name, Definition.Name);
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 2u);
	ASSERT_TRUE(Material->SetMaterialExpressions({}, {}));
	EXPECT_TRUE(FMaterialGraphOperations::Paste(*Material, Payload, 400, 0));
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
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
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
	ASSERT_TRUE(Promoted) << Promoted.Message;
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
	ASSERT_TRUE(FMaterialGraphOperations::Layout(*Source));
	FMaterialGraphClipboardPayload Payload;
	ASSERT_TRUE(FMaterialGraphOperations::CopySelection(*Source, std::span(&Node->Id, 1), Payload));
	Node.Reset();
	MarkAsGarbage(Source);
	CollectGarbage();
	ASSERT_NE(WeakTexture.Get(), nullptr);
	EXPECT_EQ(Payload.SourceRoot.Get(), nullptr);
	DMaterial* Target = NewObject<DMaterial>(nullptr, "ClipboardTextureTarget");
	ASSERT_TRUE(Target->SetMaterialExpressions({}, {}));
	ASSERT_TRUE(FMaterialGraphOperations::Paste(*Target, Payload, 0, 0));
	EXPECT_EQ(Target->GetParameterDefinitions().front().Value.GetTexture().Texture.Get(), WeakTexture.Get());
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
	ImGuiID DetailsId = 0;
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
				for (const char* Key : {"Graph", "Preview", "Details", "Parameters", "Diagnostics"})
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
					else if (std::string_view(Key) == "Parameters")
						EXPECT_EQ(Window->DockId, DetailsId);
					else if (std::string_view(Key) == "Diagnostics")
						EXPECT_NE(Window->DockId, GraphId);
					else if (std::string_view(Key) == "Details") DetailsId = Window->DockId;

					ImGui::End();
				}
			}
			ImGui::End();
		}
		ImGui::Render();
	}
	ImGui::DestroyContext(Context);
}

TEST(FMaterialGraphOperationsTests, PreviewFramingFitsBothAxesAcrossViewportShapes)
{
	for (const double Aspect : {0.2, 0.5, 1.0, 2.0, 5.0})
		for (const double Radius : {0.5, 1.0, 3.0})
		{
			const double Distance = CalculateMaterialPreviewDistance(Radius, Aspect);
			ASSERT_GT(Distance, Radius);
			const double ProjectedRadius = Radius / std::sqrt(Distance * Distance - Radius * Radius);
			const double VerticalTangent = std::tan(Math::DegreesToRadians(MaterialPreviewFieldOfView) * 0.5);
			EXPECT_LE(ProjectedRadius / VerticalTangent, 0.800001);
			EXPECT_LE(ProjectedRadius / (VerticalTangent * Aspect), 0.800001);
		}
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
	EXPECT_TRUE(Material->GetExpressionCollection().Expressions.empty());
	Parameter.Reset();
	CollectGarbage();
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
	// Direct live edits must not mutate the transaction's independent copy.
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
	FMaterialExpressionSurfaceOutputs Outputs; Outputs.Surface = {Surface->Id};
	ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
	Tests::FTestTransactorOwner Transactions;
	const auto DeletedId = Value->Id;
	ASSERT_TRUE(FMaterialGraphDocument(*Material).RemoveNodes(std::span(&DeletedId, 1), Transactions.Get()));
	const auto* Remaining = Cast<DMaterialExpressionSetSurfaceAttributes>(Material->GetExpressionCollection().Expressions.back().Get());
	ASSERT_NE(Remaining, nullptr);
	EXPECT_TRUE(Remaining->Attributes.empty());
	EXPECT_EQ(Remaining->Surface.ExpressionId, Base->Id);
	ASSERT_TRUE(Transactions->Undo());
	Remaining = Cast<DMaterialExpressionSetSurfaceAttributes>(Material->GetExpressionCollection().Expressions.back().Get());
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
	const auto B = FMaterialGraphOperations::DuplicateNodes(*Material, A.GeneratedNodeIds, 100, 0);
	ASSERT_TRUE(B) << B.Message;
	ASSERT_EQ(Material->GetParameterDefinitions().size(), 1u);
	ASSERT_TRUE(Document.AssignMaterialOutput(EMaterialSurfaceOutput::Metallic, {A.GeneratedNodeIds.front()}));
	ASSERT_TRUE(Document.AssignMaterialOutput(EMaterialSurfaceOutput::Roughness, {B.GeneratedNodeIds.front()}));
	std::vector<DMaterialExpression*> Expressions;
	for (const auto& E : Material->GetExpressionCollection().Expressions) Expressions.push_back(E.Get());
	const auto Built = FMaterialExpressionBuildContext(Expressions).FinishSurface(Material->GetExpressionOutputs());
	ASSERT_TRUE(Built.Diagnostics.empty());
	ASSERT_EQ(Built.Parameters.size(), 1u);
	EXPECT_EQ(Built.Parameters.front().Id, Id);
	auto* Instance = NewObject<DMaterialInstance>(nullptr, "SharedParameterInstance");
	ASSERT_TRUE(Instance->SetParent(Material));
	ASSERT_TRUE(Instance->SetParameterValue(Id, FMaterialParameterValue::MakeScalar(.8f)));
	ASSERT_TRUE(FMaterialGraphOperations::SetParameterValue(*Material, Id, FMaterialParameterValue::MakeScalar(.6f), Transactions.Get()));
	for (const auto& E : Material->GetExpressionCollection().Expressions)
		EXPECT_FLOAT_EQ(Cast<DMaterialExpressionScalarParameter>(E.Get())->DefaultValue, .6f);
	ASSERT_TRUE(Transactions->Undo());
	for (const auto& E : Material->GetExpressionCollection().Expressions)
		EXPECT_FLOAT_EQ(Cast<DMaterialExpressionScalarParameter>(E.Get())->DefaultValue, .25f);
	ASSERT_TRUE(Transactions->Redo());

	TStrongObjectPtr<DMaterialExpressionScalarParameter> Edit(Cast<DMaterialExpressionScalarParameter>(
		DuplicateObject(Material->GetExpressionCollection().Expressions.back().Get(), nullptr, NAME_None)));
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
	EXPECT_FLOAT_EQ(Cast<DMaterialExpressionScalarParameter>(Material->GetExpressionCollection().Expressions.back().Get())->DefaultValue, .6f);
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
	ASSERT_TRUE(FMaterialGraphOperations::Layout(*Source));
	const auto Before = CaptureExpressions(*Source);
	B->DefaultValue = .7f;
	EXPECT_FALSE(Source->SetMaterialExpressions(Nodes, {}));
	EXPECT_FALSE(FMaterialExpressionBuildContext(Nodes).FinishSurface({}).Diagnostics.empty());
	EXPECT_EQ(CaptureExpressions(*Source), Before);
	B->DefaultValue = .4f;
	B->Metadata.Id = FGuid::NewGuid();
	EXPECT_FALSE(Source->SetMaterialExpressions(Nodes, {}));
	FMaterialGraphClipboardPayload Payload;
	const std::array Ids{A->Id, B->Id};
	ASSERT_TRUE(FMaterialGraphOperations::CopySelection(*Source, Ids, Payload));
	ASSERT_TRUE(FMaterialGraphOperations::Paste(*Target, Payload, 0, 0));
	ASSERT_EQ(Target->GetParameterDefinitions().size(), 1u);
	const auto TargetId = Target->GetParameterDefinitions().front().Id;
	EXPECT_NE(TargetId, A->Metadata.Id);
	ASSERT_TRUE(Target->SetParameterValue(TargetId, FMaterialParameterValue::MakeScalar(.9f)));
	ASSERT_TRUE(FMaterialGraphOperations::Paste(*Target, Payload, 100, 0));
	EXPECT_EQ(Target->GetParameterDefinitions().size(), 1u);
	for (const auto& E : Target->GetExpressionCollection().Expressions)
		EXPECT_FLOAT_EQ(Cast<DMaterialExpressionScalarParameter>(E.Get())->DefaultValue, .9f);
	FMaterialParameterDefinition Conflict;
	Conflict.Name = "Amount"; Conflict.Type = EMaterialParameterType::Vector;
	Conflict.Value = FMaterialParameterValue::MakeVector({1, 1, 1});
	const auto TargetBefore = CaptureExpressions(*Target);
	EXPECT_FALSE(FMaterialGraphOperations::CreateParameter(*Target, Conflict));
	EXPECT_EQ(CaptureExpressions(*Target), TargetBefore);
	MarkAsGarbage(Source); MarkAsGarbage(Target); CollectGarbage();
}

TEST(FMaterialGraphOperationsTests, SharedTextureParametersPreserveLocalSamplingAndRoundTrip)
{
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture MountRegistry;
	const auto Root = Testing::GetTestWorkDirectory() / "SharedTextureParameters";
	Testing::RemoveTestWorkDirectory(Root);
	Testing::RegisterMountPointForTests("/SharedTextureParameters/", Root.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/SharedTextureParameters/Base", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	auto A = Testing::MakeGraphExpression<DMaterialExpressionTextureSampleParameter2D>();
	auto B = Testing::MakeGraphExpression<DMaterialExpressionTextureSampleParameter2D>();
	auto UV = Testing::MakeGraphExpression<DMaterialExpressionVector2Constant>();
	UV->Value = {.2f, .7f};
	A->Metadata = {FGuid::NewGuid(), "SharedTexture"}; B->Metadata = A->Metadata;
	B->UV = {UV->Id};
	const auto Id = A->Metadata.Id, BId = B->Id;
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 3>{A.Get(), B.Get(), UV.Get()}, {}));
	Durin::Tests::FTestTransactorOwner Transactions;
	auto Definition = A->GetParameterDefinition();
	Definition.Value.GetTexture().TextureFallback = EMaterialTextureFallback::FlatRGNormal;
	ASSERT_TRUE(FMaterialGraphOperations::SetParameterValue(*Material, Id, Definition.Value, Transactions.Get()));
	EXPECT_EQ(FindExpression<DMaterialExpressionTextureSampleParameter2D>(*Material, BId)->UV.ExpressionId, UV->Id);
	EXPECT_EQ(FindExpression<DMaterialExpressionTextureSampleParameter2D>(*Material, BId)->DefaultValue.TextureFallback, EMaterialTextureFallback::FlatRGNormal);
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	ASSERT_TRUE(Transactions->Reset());
	ASSERT_TRUE(UnloadPackage(Path));
	Material = nullptr;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Path), Material));
	ASSERT_EQ(Material->GetParameterDefinitions().size(), 1u);
	EXPECT_EQ(Material->GetParameterDefinitions().front().Id, Id);
	EXPECT_EQ(FindExpression<DMaterialExpressionTextureSampleParameter2D>(*Material, BId)->UV.ExpressionId, UV->Id);
	EXPECT_EQ(FindExpression<DMaterialExpressionTextureSampleParameter2D>(*Material, BId)->DefaultValue.TextureFallback, EMaterialTextureFallback::FlatRGNormal);
	ASSERT_TRUE(UnloadPackage(Path)); CollectGarbage();
}
