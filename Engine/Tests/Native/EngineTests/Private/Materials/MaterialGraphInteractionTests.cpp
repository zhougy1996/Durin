#include "MaterialGraphDocument.h"
#include "MaterialGraphTestSupport.h"
#include "Widgets/MaterialDetailsStyle.h"

TEST(FMaterialGraphInteractionTests, ParameterValueControlsPreviewCoalesceAndCancel)
{
	InitializeDObjectSystem();
	for (const auto Presentation : {EMaterialParameterPresentation::Drag, EMaterialParameterPresentation::Default,
		EMaterialParameterPresentation::Color})
	{
		TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		FMaterialParameterDefinition Definition;
		Definition.Id = FGuid::NewGuid(); Definition.Name = "Continuous"; Definition.DisplayName = "Continuous";
		Definition.Presentation = Presentation;
		Definition.Type = Presentation == EMaterialParameterPresentation::Drag ? EMaterialParameterType::Scalar : EMaterialParameterType::Vector4;
		Definition.Value = Definition.Type == EMaterialParameterType::Scalar ? FMaterialParameterValue::MakeScalar(.2f)
			: FMaterialParameterValue::MakeVector4({.2, .3, .4, 1});
		ASSERT_TRUE(FMaterialGraphOperations::CreateParameter(*Material, Definition));
		const auto Original = Material->FindParameterDefinition(Definition.Id)->Value;
		const auto Generation = Material->GetMaterialCompileStatus().RequestGeneration;
		Durin::Tests::FTestTransactorOwner Transactions;
		FMaterialGraphCanvas Canvas{FMaterialGraphDocument(*Material)};
		auto* Context = ImGui::CreateContext();
		auto& IO = ImGui::GetIO();
		IO.DisplaySize = {900, 500}; IO.DeltaTime = 1.f / 60; IO.IniFilename = nullptr;
		IO.Fonts->AddFontDefault(); IO.Fonts->Build();
		ImVec2 Start;
		int Errors = 0;
		const auto Frame = [&](ImVec2 Mouse, bool Down, bool Visible = true) {
			IO.AddMousePosEvent(Mouse.x, Mouse.y); IO.AddMouseButtonEvent(ImGuiMouseButton_Left, Down);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({800, 400});
			ImGui::Begin("Parameter value", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
			if (Visible && MonaImGui::PropertyEdit::BeginTable("Values", DetailsStyle::MakeTableConfig()))
			{
				Canvas.DrawParameterValue(*Material->FindParameterDefinition(Definition.Id), *Transactions.Get(),
					[&](std::string Error) { ++Errors; ADD_FAILURE() << Error; });
				Start = {ImGui::GetItemRectMin().x + 30, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) / 2};
				MonaImGui::PropertyEdit::EndTable();
			}
			Canvas.EndParameterFrame();
			ImGui::End(); ImGui::Render();
		};
		Frame({850, 450}, false); Frame({850, 450}, false);
		const auto Drag = [&]() {
			Frame(Start, false); Frame(Start, true);
			Frame({Start.x + 30, Start.y}, true); Frame({Start.x + 60, Start.y}, true);
		};
		Drag();
		const auto Preview = Material->FindParameterDefinition(Definition.Id)->Value;
		EXPECT_NE(Preview, Original);
		EXPECT_EQ(Transactions->GetUndoCount(), 0u);
		Frame({Start.x + 60, Start.y}, false);
		EXPECT_EQ(Transactions->GetUndoCount(), 1u);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(Material->FindParameterDefinition(Definition.Id)->Value, Original);
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(Material->FindParameterDefinition(Definition.Id)->Value, Preview);
		ASSERT_TRUE(Transactions->Reset());
		Drag();
		IO.AddKeyEvent(ImGuiKey_Escape, true); Frame({Start.x + 60, Start.y}, true);
		IO.AddKeyEvent(ImGuiKey_Escape, false); Frame({Start.x + 60, Start.y}, false);
		EXPECT_EQ(Material->FindParameterDefinition(Definition.Id)->Value, Preview);
		EXPECT_EQ(Transactions->GetUndoCount(), 0u);
		Drag();
		Frame({Start.x + 60, Start.y}, true, false);
		EXPECT_EQ(Material->FindParameterDefinition(Definition.Id)->Value, Preview);
		EXPECT_EQ(Transactions->GetUndoCount(), 0u);
		EXPECT_EQ(Material->GetMaterialCompileStatus().RequestGeneration, Generation);
		EXPECT_EQ(Errors, 0);
		Canvas.CancelInteraction();
		ImGui::DestroyContext(Context);
	}
}

namespace
{
	struct FScopedGraphClipboard
	{
		~FScopedGraphClipboard() { Durin::Editor::Material::FMaterialGraphCanvas::ClearSharedClipboard(); }
	};
}

namespace Durin::Editor::Material
{
	struct FMaterialGraphCanvasTestAccess
	{
		static auto Duplicate(FMaterialGraphCanvas& Canvas, DMaterial& Material,
			DTransactor& Transactions, std::span<const FGuid> Nodes) -> void
		{
			Canvas.DuplicateNodes(Material, Transactions, Nodes,
				[](const std::string& Error) { ADD_FAILURE() << Error; });
		}
		static auto Select(FMaterialGraphCanvas& Canvas,
			std::initializer_list<FMaterialGraphCanvasNodeId> Nodes) -> void
		{ Canvas.SelectedNodes = Nodes; }
		static auto Frame(FMaterialGraphCanvas& Canvas, const FMaterialGraphView& View,
			bool bAll) -> void
		{
			Canvas.FrameNodes(View, {1000.0f, 600.0f}, bAll
				? FMaterialGraphCanvas::EFrameScope::All
				: FMaterialGraphCanvas::EFrameScope::Selection);
		}
		static auto ProgramSelection(const FMaterialGraphCanvas& Canvas) -> std::vector<FGuid>
		{ return Canvas.GetSelectedProgramNodes(); }
		static auto ShowAdvanced(FMaterialGraphCanvas& Canvas) -> void
		{ Canvas.bShowAdvancedInputs = true; Canvas.bViewStale = true; }
		static auto HideAdvanced(FMaterialGraphCanvas& Canvas) -> void
		{ Canvas.bShowAdvancedInputs = false; Canvas.bViewStale = true; }
		static auto Prepare(FMaterialGraphCanvas& Canvas, DMaterial& Material)
			-> const FMaterialGraphView& { return Canvas.PrepareView(Material); }
		static auto Details(FMaterialGraphCanvas& Canvas, DObject& Owner)
			-> const FMaterialGraphView& { return Canvas.PrepareDetailsView(Owner); }
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
		static auto PrepareFunction(FMaterialGraphCanvas& Canvas, DMaterialFunction& Function) -> const FMaterialGraphView&
		{ Canvas.PrepareFunctionView(Function); return Canvas.CachedView; }
		static auto MoveActive(const FMaterialGraphCanvas& Canvas) -> bool { return Canvas.MoveSession.IsActive(); }
		static auto SearchMenu(FMaterialGraphCanvas& Canvas, const char* Query) -> void
		{
			ImGui::ClearActiveID(); // Release InputText's internal buffer before setting the test query.
			if (!std::holds_alternative<FMaterialGraphCanvas::FNodeCreationMenuInteraction>(Canvas.Interaction))
				Canvas.Interaction = FMaterialGraphCanvas::FNodeCreationMenuInteraction{};
			auto& Menu = std::get<FMaterialGraphCanvas::FNodeCreationMenuInteraction>(Canvas.Interaction);
			std::snprintf(Menu.Search.data(), Menu.Search.size(), "%s", Query);
		}
		static auto Remember(FMaterialGraphCanvas& Canvas, EMaterialProgramOpcode Opcode) -> void
		{
			const auto Entry = std::ranges::find_if(Canvas.Catalog, [&](const auto& Value) {
				return Value.Opcode == Opcode && Value.ResultType == EMaterialProgramValueType::Float;
			});
			ASSERT_NE(Entry, Canvas.Catalog.end());
			Canvas.RememberCreation(MakeCreationAction(*Entry));
		}
		static auto CheckRecentRows(const FMaterialGraphCanvas& Canvas) -> void
		{
			ASSERT_EQ(Canvas.CachedCreationMenuRecentCount, 2u);
			const auto& Rows = Canvas.CachedCreationMenuResults;
			const auto Base = FMaterialGraphOperations::SearchCatalogIndices(Canvas.Catalog, "");
			ASSERT_GE(Rows.size(), Base.size() + 2);
			EXPECT_EQ(std::get<FMaterialGraphCatalogEntry>(Canvas.CachedCreationActions[Rows[0]].Payload).Opcode, EMaterialProgramOpcode::Add);
			EXPECT_EQ(std::get<FMaterialGraphCatalogEntry>(Canvas.CachedCreationActions[Rows[1]].Payload).Opcode, EMaterialProgramOpcode::Multiply);
			for (size_t I = 0; I < Base.size(); ++I)
				EXPECT_EQ(Canvas.CachedCreationActions[Rows[I + 2]].Id, MakeCreationAction(Canvas.Catalog[Base[I]]).Id);
			EXPECT_EQ(std::ranges::count(Rows, Rows[0]), 2);
			EXPECT_EQ(std::ranges::count(Rows, Rows[1]), 2);
		}
		static auto SearchOrder(const FMaterialGraphCanvas& Canvas) -> bool
		{
			if (Canvas.CachedCreationMenuQuery != "texture" || Canvas.CachedCreationMenuRecentCount != 0) return false;
			std::vector<std::string> Actual, Expected;
			for (auto I : Canvas.CachedCreationMenuResults) Actual.push_back(Canvas.CachedCreationActions[I].Id);
			for (auto I : FMaterialGraphOperations::SearchCatalogIndices(Canvas.Catalog, "texture"))
				Expected.push_back(MakeCreationAction(Canvas.Catalog[I]).Id);
			return Actual == Expected;
		}
		static auto RecentAction(const FMaterialGraphCanvas& Canvas) -> std::string
		{ return Canvas.RecentCreationMenuEntries.empty() ? std::string{} : Canvas.RecentCreationMenuEntries.front(); }
		static auto FirstMenuAction(const FMaterialGraphCanvas& Canvas) -> std::string
		{ return Canvas.CachedCreationMenuResults.empty() ? std::string{} : Canvas.CachedCreationActions[Canvas.CachedCreationMenuResults.front()].Id; }
		static auto Menu(const FMaterialGraphCanvas& Canvas) -> bool
		{ return std::holds_alternative<FMaterialGraphCanvas::FNodeCreationMenuInteraction>(Canvas.Interaction); }
		static auto OpenCreationAt(FMaterialGraphCanvas& Canvas, ImVec2 Position) -> void
		{ Canvas.Interaction = FMaterialGraphCanvas::FNodeCreationMenuInteraction{.GraphPosition = Position}; }
	};
}

TEST(FMaterialGraphInteractionTests, DuplicateMixedOutputSelectionPreservesCopiedNodePositions)
{
	InitializeDObjectSystem();
	for (const bool bCanvas : {false, true})
	{
		SCOPED_TRACE(bCanvas ? "Canvas" : "Operations");
		TStrongObjectPtr<DMaterial> Material(MakeExpandedGraphMaterial("MixedDuplicate"));
		ASSERT_NE(Material.Get(), nullptr);
		Tests::FTestTransactorOwner Transactions;
		FMaterialGraphDocument Document(*Material);
		const auto First = Testing::CreateGraphConstant(Document, 0.25f, 500, 300);
		const auto Second = Testing::CreateGraphConstant(Document, 0.75f, 700, 200);
		ASSERT_TRUE(First);
		ASSERT_TRUE(Second);
		const FGuid OutputId = Material->GetOutputNode()->Id;
		const FMaterialGraphNodePresentation OutputPosition{OutputId, 0, 0};
		ASSERT_TRUE(FMaterialGraphDocument(*Material).MoveNodes(std::span(&OutputPosition, 1)));
		const auto Before = Material->GetMaterialGraphPresentation();
		const std::array Selection{OutputId, First.GeneratedNodeIds.front(), Second.GeneratedNodeIds.front()};
		std::vector<FGuid> Generated;
		if (bCanvas)
		{
			FMaterialGraphCanvas Canvas{FMaterialGraphDocument(*Material)};
			FMaterialGraphCanvasTestAccess::Duplicate(Canvas, *Material, *Transactions.Get(), Selection);
			Generated = FMaterialGraphCanvasTestAccess::ProgramSelection(Canvas);
		}
		else
		{
			const auto Result = FMaterialGraphDocument(*Material).DuplicateNodes(Selection, 40, 40, Transactions.Get());
			ASSERT_TRUE(Result) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Result);
			Generated = Result.GeneratedNodeIds;
		}
		ASSERT_EQ(Generated.size(), 2u);
		const auto View = Document.Inspect();
		std::vector<std::pair<int32, int32>> Positions;
		for (const auto& Id : Generated)
		{
			const auto* Node = FindViewNode(View, Id);
			ASSERT_NE(Node, nullptr);
			Positions.emplace_back(Node->Presentation.X, Node->Presentation.Y);
		}
		std::ranges::sort(Positions);
		const std::vector<std::pair<int32, int32>> Expected{{540, 340}, {740, 240}};
		EXPECT_EQ(Positions, Expected);
		EXPECT_EQ(Material->GetOutputNode()->Id, OutputId);
		const auto After = Material->GetMaterialGraphPresentation();
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(Material->GetMaterialGraphPresentation(), Before);
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(Material->GetMaterialGraphPresentation(), After);
		EXPECT_TRUE(Transactions->Reset());
	}
}

TEST(FMaterialGraphInteractionTests, FunctionNodesCanBeCreatedFromSearchBeforeWiring)
{
	InitializeDObjectSystem();
	const auto Root = Testing::CreateTestFixtureDirectory("FunctionMenu");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/FunctionMenu/", .Owner = EMountOwner::Test,
		.Root = Root, .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid());
	ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/FunctionMenu/RequiredFunction", Path));
	DMaterialFunction* Callee = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Callee));
	FMaterialFunctionPort Required;
	Required.Id = FGuid::NewGuid(); Required.Type = EMaterialProgramValueType::Float; Required.Name = "Required Amount";
	Required.bRequired = true;
	ASSERT_TRUE(FMaterialGraphDocument(*Callee).AddPort(false, Required));
	ASSERT_TRUE(SavePackage(Callee->GetPackage()));
	auto* Function = NewObject<DMaterialFunction>(nullptr, "MenuFunctionOutput");
	const auto Original = Function->GetFunctionSignature();
	Durin::Tests::FTestTransactorOwner Transactions;
	FMaterialGraphCanvas Canvas{FMaterialGraphDocument(*Function)};
	auto* Context = ImGui::CreateContext();
	auto& IO = ImGui::GetIO();
	IO.IniFilename = nullptr; IO.DisplaySize = {1200, 900}; IO.DeltaTime = 1.0f / 60.0f;
	IO.Fonts->AddFontDefault(); IO.Fonts->Build();
	std::vector<std::string> Errors;
	const auto Frame = [&] {
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1200, 900});
		ImGui::Begin("Function creation", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		Canvas.Draw(*Transactions.Get(), 820,
			[&](std::string Error) { Errors.push_back(std::move(Error)); }, [](std::string_view) {});
		ImGui::End(); ImGui::Render();
	};
	Frame(); Frame();
	FMaterialGraphCanvasTestAccess::OpenCreationAt(Canvas, {321, 234});
	Frame(); Frame();
	IO.AddInputCharactersUTF8("Function Output"); Frame();
	IO.AddKeyEvent(ImGuiKey_Enter, true); Frame();
	IO.AddKeyEvent(ImGuiKey_Enter, false); Frame();
	EXPECT_TRUE(Errors.empty());
	EXPECT_EQ(Function->GetFunctionSignature().Outputs.size(), Original.Outputs.size() + 1);
	EXPECT_FALSE(FMaterialGraphCanvasTestAccess::Menu(Canvas));
	for (const auto& E : Function->GetExpressionCollection().Expressions)
		if (const auto* Output = Cast<DMaterialExpressionFunctionOutput>(E.Get()); Output && Output->Port.Name == "Output 1")
		{
			EXPECT_FALSE(Output->Source.ExpressionId.IsValid());
			const auto& Positions = Function->GetFunctionPresentation().Nodes;
			const auto Position = std::ranges::find(Positions, E->Id, &FMaterialGraphNodePresentation::NodeId);
			ASSERT_NE(Position, Positions.end());
			EXPECT_EQ(Position->X, 321); EXPECT_EQ(Position->Y, 234);
		}
	EXPECT_TRUE(Transactions->Undo());
	EXPECT_EQ(Function->GetFunctionSignature(), Original);
	EXPECT_TRUE(Transactions->Redo());
	FMaterialGraphCanvasTestAccess::OpenCreationAt(Canvas, {432, 345});
	Frame(); Frame();
	IO.AddInputCharactersUTF8("RequiredFunction"); Frame();
	IO.AddKeyEvent(ImGuiKey_Enter, true); Frame();
	IO.AddKeyEvent(ImGuiKey_Enter, false); Frame();
	EXPECT_TRUE(Errors.empty());
	EXPECT_FALSE(FMaterialGraphCanvasTestAccess::Menu(Canvas));
	const auto Calls = std::ranges::count_if(Function->GetExpressionCollection().Expressions, [&](const auto& E) {
		const auto* Call = Cast<DMaterialExpressionFunctionCall>(E.Get());
		return Call && Call->Function == Callee && Call->Inputs.empty();
	});
	EXPECT_EQ(Calls, 1);
	EXPECT_EQ(FMaterialGraphCanvasTestAccess::RecentAction(Canvas), MakeFunctionCreationAction(Callee->GetObjectPath()).Id);
	EXPECT_TRUE(Transactions->Undo());
	EXPECT_TRUE(Transactions->Redo());
	FMaterialGraphCanvasTestAccess::OpenCreationAt(Canvas, {0, 0});
	Frame(); Frame();
	EXPECT_EQ(FMaterialGraphCanvasTestAccess::FirstMenuAction(Canvas), MakeFunctionCreationAction(Callee->GetObjectPath()).Id);
	Canvas.CancelInteraction(); ImGui::DestroyContext(Context);
	EXPECT_TRUE(Transactions->Reset()); MarkAsGarbage(Function); CollectGarbage();
}

TEST(FMaterialGraphInteractionTests, CanvasGeometryUsesStableMetricsAndZoomHysteresis)
{
	const FMaterialGraphCanvasMetrics& Metrics = FMaterialGraphGeometry::GetMetrics();
	EXPECT_FLOAT_EQ(Metrics.NodeWidth, 224.0f);
	EXPECT_FLOAT_EQ(Metrics.MinimumHitDiameter, 16.0f);
	EXPECT_LE(Metrics.BodyPadding + Metrics.SurfaceLabelWidth
		+ Metrics.SurfaceValueGap + Metrics.SurfaceValueWidth + Metrics.BodyPadding,
		Metrics.SurfaceWidth);
	EXPECT_FLOAT_EQ(FMaterialGraphGeometry::GetNodeHeight(0), 94.0f);
	EXPECT_FLOAT_EQ(FMaterialGraphGeometry::GetNodeHeight(3), 142.0f);
	FMaterialGraphNodeView Constant;
	Constant.Node.Opcode = EMaterialProgramOpcode::Constant;
	Constant.Node.ResultType = EMaterialProgramValueType::Float;
	EXPECT_FLOAT_EQ(GraphNodeWidth(Constant), 112.0f);
	EXPECT_FLOAT_EQ(GraphNodeHeight(Constant), Metrics.HeaderHeight);
	EXPECT_FLOAT_EQ(GraphNodePinOffset(Constant), GraphNodeHeight(Constant) * 0.5f);
	Constant.Node.ResultType = EMaterialProgramValueType::Float4;
	EXPECT_FLOAT_EQ(GraphNodeWidth(Constant), 208.0f);
	FMaterialGraphNodeView Add;
	Add.Node.Opcode = EMaterialProgramOpcode::Add;
	Add.Inputs.resize(2);
	EXPECT_FLOAT_EQ(GraphNodeWidth(Add), 160.0f);
	EXPECT_LT(GraphNodeHeight(Add), FMaterialGraphGeometry::GetNodeHeight(2));
	EXPECT_LT(GraphNodePinOffset(Add) + Metrics.PinRowHeight, GraphNodeHeight(Add));


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

TEST(FMaterialGraphInteractionTests, HiddenAdvancedPinsRetainStableIdentitiesAndRevealBindings)
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
	FMaterialGraphCanvas Canvas{FMaterialGraphDocument(*Material)};
	FMaterialGraphCanvasTestAccess::HideAdvanced(Canvas);
	const auto& Hidden = FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material);
	const auto* CallView = FindViewNode(Hidden, Call.GeneratedNodeIds[0]);
	ASSERT_NE(CallView, nullptr);
	ASSERT_EQ(CallView->Inputs.size(), 2u); // The default Surface input remains visible.
	EXPECT_EQ(CallView->Inputs.back().PortId, Visible);
	EXPECT_EQ(CallView->Inputs.back().InputIndex, 2u);
	const auto& Details = FMaterialGraphCanvasTestAccess::Details(Canvas, *Material);
	ASSERT_EQ(FindViewNode(Details, Call.GeneratedNodeIds[0])->Inputs.size(), 3u);
	FMaterialGraphCanvasTestAccess::PrepareVisuals(Canvas);
	for (int Frame = 0; Frame < 3; ++Frame)
	{
		FMaterialGraphCanvasTestAccess::Details(Canvas, *Material);
		EXPECT_FALSE(FMaterialGraphCanvasTestAccess::TopologyStale(Canvas));
	}
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

TEST(FMaterialGraphInteractionTests, TextureOutputsHideUnusedAdvancedPinsWithoutChangingLinks)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "CompactTextureOutputs");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto Added = FMaterialGraphOperations::AddTextureToSurfaceOutput(*Material, {});
	ASSERT_TRUE(Added);
	const auto SampleId = Added.GeneratedNodeIds.front();
	FMaterialGraphCanvas Canvas{FMaterialGraphDocument(*Material)};
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
	ASSERT_EQ(Sample->Outputs.size(), 7u);
	EXPECT_EQ(Sample->Outputs[6].OutputIndex, 7u);
	EXPECT_FALSE(std::ranges::any_of(Sample->Outputs, [](const auto& Pin) { return Pin.Name == "Normal"; }));
	FMaterialGraphCanvasTestAccess::HideAdvanced(Canvas);
	FMaterialGraphDocument Document(*Material);
	const auto SecondSample = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::TextureSample2D, EMaterialProgramValueType::Float4, {SampleId, 7});
	ASSERT_TRUE(SecondSample);
	const auto RG = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::Swizzle, EMaterialProgramValueType::Float2, {SampleId, 0});
	ASSERT_TRUE(RG);
	EXPECT_EQ(FindViewNode(Document.Inspect(), RG.GeneratedNodeIds.front())->PrimaryLabel, "Component Mask RG");
	EXPECT_FALSE(Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::DecodeNormalRG,
		EMaterialProgramValueType::Float3, {RG.GeneratedNodeIds.front()}));
	const auto NormalizeCatalog = FMaterialGraphOperations::EnumerateCatalog();
	const auto NormalizeEntry = std::ranges::find_if(NormalizeCatalog, [](const auto& Entry) {
		return Entry.Opcode == EMaterialProgramOpcode::Normalize && Entry.ResultType == EMaterialProgramValueType::Float3;
	});
	ASSERT_NE(NormalizeEntry, NormalizeCatalog.end());
	const auto NormalConsumer = Document.CreateCatalogNode(*NormalizeEntry, 0, 0, {SampleId, 1});
	ASSERT_TRUE(NormalConsumer);
	const auto NormalView = Document.Inspect();
	ASSERT_EQ(FindViewNode(NormalView, NormalConsumer.GeneratedNodeIds.front())->Inputs.front().SourceType, EMaterialProgramValueType::Float3);
	ASSERT_TRUE(Document.RemoveNodes(NormalConsumer.GeneratedNodeIds));
	Sample = FindViewNode(FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material), SampleId);
	ASSERT_EQ(Sample->Outputs.size(), 7u);
	EXPECT_EQ(Sample->Outputs[6].OutputIndex, 7u);
	const auto BeforeInvalid = Material->GetExpressionOutputs();
	EXPECT_FALSE(Document.Connect(FMaterialGraphPinAddress::MaterialOutput(Material->GetOutputNode()->Id, EMaterialSurfaceOutput::Normal), FMaterialGraphPinAddress::Output({SampleId, 8}), true));
	EXPECT_EQ(Material->GetExpressionOutputs(), BeforeInvalid);
	ASSERT_TRUE(Document.Disconnect(FMaterialGraphPinAddress::MaterialOutput(Material->GetOutputNode()->Id, EMaterialSurfaceOutput::Normal)));
	const std::array Consumers{SecondSample.GeneratedNodeIds.front(), RG.GeneratedNodeIds.front()};
	ASSERT_TRUE(FMaterialGraphDocument(*Material).RemoveNodes(Consumers));
	Sample = FindViewNode(FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material), SampleId);
	EXPECT_EQ(Sample->Outputs.size(), 6u);
	EXPECT_EQ(Material->GetExpressionOutputs().BaseColor.OutputIndex, 1u);
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphInteractionTests, CanvasFramesExplicitScopeWithMaterialOutputIdentity)
{
	FMaterialGraphView View;
	FMaterialGraphNodeView Node;
	Node.Node.Id = FGuid::NewGuid();
	Node.Presentation.X = -1000;
	Node.Presentation.Y = -100;
	View.Nodes.push_back(Node);
	FMaterialGraphCanvas Canvas{FMaterialGraphDocument(*Material)};
	FMaterialGraphNodeView Output;
	Output.Node.Id = FGuid::NewGuid(); Output.Node.bMaterialOutput = true;
	Output.Presentation = {Output.Node.Id, 800, 100};
	Output.PrimaryLabel = "Material Output";
	Output.Inputs.resize(9);
	View.Nodes.push_back(Output);
	const auto ExpectCenter = [&](float X, float Y) {
		const auto [Zoom, Pan] = Canvas.GetViewport();
		EXPECT_NEAR((500.f - Pan.x) / Zoom, X, .001f);
		EXPECT_NEAR((300.f - Pan.y) / Zoom, Y, .001f);
	};
	FMaterialGraphCanvasTestAccess::Select(Canvas, {Output.Node.Id});
	FMaterialGraphCanvasTestAccess::Frame(Canvas, View, false);
	ExpectCenter(800.f + GraphNodeWidth(Output) * .5f, 100.f + GraphNodeHeight(Output) * .5f);
	EXPECT_EQ(FMaterialGraphCanvasTestAccess::ProgramSelection(Canvas), std::vector<FGuid>{Output.Node.Id});
	const auto Selection = Canvas.GetSelection();
	FMaterialGraphCanvasTestAccess::Frame(Canvas, View, true);
	EXPECT_EQ(Canvas.GetSelection(), Selection);
	const auto AllViewport = Canvas.GetViewport();
	FMaterialGraphCanvasTestAccess::Select(Canvas, {Node.Node.Id, Output.Node.Id});
	FMaterialGraphCanvasTestAccess::Frame(Canvas, View, false);
	EXPECT_FLOAT_EQ(Canvas.GetViewport().first, AllViewport.first);
	EXPECT_FLOAT_EQ(Canvas.GetViewport().second.x, AllViewport.second.x);
	EXPECT_FLOAT_EQ(Canvas.GetViewport().second.y, AllViewport.second.y);
	FMaterialGraphCanvasTestAccess::Select(Canvas, {Node.Node.Id});
	FMaterialGraphCanvasTestAccess::Frame(Canvas, View, false);
	ExpectCenter(-1000.0f + GraphNodeWidth(Node) * 0.5f,
		-100.0f + GraphNodeHeight(Node) * 0.5f);
	FMaterialGraphCanvasTestAccess::Select(Canvas, {});
	const auto Before = Canvas.GetViewport();
	FMaterialGraphCanvasTestAccess::Frame(Canvas, View, false);
	EXPECT_FLOAT_EQ(Canvas.GetViewport().first, Before.first);
	EXPECT_FLOAT_EQ(Canvas.GetViewport().second.x, Before.second.x);
	EXPECT_FLOAT_EQ(Canvas.GetViewport().second.y, Before.second.y);
}

TEST(FMaterialGraphInteractionTests, DiagnosticNavigationIsLocatedAndDocumentLocal)
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
		.NodeId = FirstNode,
		.LocationIndex = static_cast<uint32>(EMaterialSurfaceOutput::Roughness),
	}));
	EXPECT_TRUE(FirstCanvas.GetSelection().contains(FirstNode));
	EXPECT_EQ(FirstCanvas.GetSelectedSurfaceOutput(),
		EMaterialSurfaceOutput::Roughness);
	EXPECT_TRUE(FirstCanvas.SelectAndFrame(FirstNode));
	EXPECT_FALSE(FirstCanvas.GetSelectedSurfaceOutput().has_value());
	EXPECT_FALSE(FirstCanvas.SelectAndFrameDiagnostic({
		.LocationKind = EMaterialProgramDiagnosticLocationKind::Program,
	}));
	EXPECT_FALSE(FirstCanvas.SelectAndFrameDiagnostic({
		.LocationKind = EMaterialProgramDiagnosticLocationKind::SurfaceOutput,
		.NodeId = FirstNode,
		.LocationIndex = 8,
	}));
	EXPECT_FALSE(FirstCanvas.SelectAndFrameDiagnostic({
		.LocationKind = EMaterialProgramDiagnosticLocationKind::SurfaceOutput,
		.NodeId = FirstNode,
		.LocationIndex = 99,
	}));
}

TEST(FMaterialGraphInteractionTests, CanvasPositionRefreshPreservesTopologyStorage)
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
	ASSERT_TRUE(FMaterialGraphDocument(*Material).Layout());
	FMaterialGraphCanvas Canvas{FMaterialGraphDocument(*Material)};
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
		Testing::OutputPosition(*Material, Presentation).Y += 3;
		ASSERT_TRUE(Material->SetMaterialGraphPresentation(Presentation) != Durin::EMaterialGraphPresentationResult::Rejected);
		FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material);
		EXPECT_EQ(View.Nodes.data(), Nodes);
		EXPECT_FALSE(FMaterialGraphCanvasTestAccess::TopologyStale(Canvas));
		EXPECT_EQ(FindViewNode(View, Presentation.Nodes.front().NodeId)->Presentation,
			Presentation.Nodes.front());
		EXPECT_EQ(FindViewNode(View, Material->GetOutputNode()->Id)->Presentation.Y, Testing::OutputPosition(*Material, Presentation).Y);
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
	EXPECT_EQ(FindViewNode(FMaterialGraphCanvasTestAccess::Details(Canvas, *Material), ParameterId)->PrimaryLabel,
		Material->FindParameterDefinition(Definition.Id)->DisplayName);
	FMaterialGraphCanvasTestAccess::PrepareVisuals(Canvas);
	const auto AuthoredRevision = Material->GetMaterialCompileStatus().AuthoredRevision;
	ASSERT_TRUE(Material->SetParameterValue(Definition.Id, FMaterialParameterValue::MakeScalar(.75f)));
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision, AuthoredRevision);
	FMaterialGraphCanvasTestAccess::Details(Canvas, *Material);
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::TopologyStale(Canvas));
	FMaterialGraphCanvasTestAccess::PrepareVisuals(Canvas);
	FMaterialGraphCanvasTestAccess::Details(Canvas, *Material);
	EXPECT_FALSE(FMaterialGraphCanvasTestAccess::TopologyStale(Canvas));
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphInteractionTests, FunctionCanvasConnectsAndMovesNodesWithUndo)
{
	FScopedGraphClipboard Clipboard;
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
	FMaterialGraphCanvas Canvas{FMaterialGraphDocument(*Function)};
	Canvas.SetViewport(1, {40, 40});
	ImVec2 Origin;
	int Errors = 0;
	const auto Frame = [&](ImVec2 Mouse, bool Down) {
		IO.AddMousePosEvent(Mouse.x, Mouse.y); IO.AddMouseButtonEvent(ImGuiMouseButton_Left, Down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1200, 1000});
		ImGui::Begin("Function Canvas", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		Canvas.Draw(*Transactions.Get(), 900, [&](std::string) { ++Errors; }, [](std::string_view) {});
		const auto* Child = ImGui::GetCurrentWindow()->DC.ChildWindows.back();
		Origin = {Child->Pos.x + Child->WindowPadding.x + 40, Child->Pos.y + Child->WindowPadding.y + 40 + ImGui::GetFrameHeightWithSpacing()};
		ImGui::End(); ImGui::Render();
	};
	Frame({1100, 950}, false); Frame({1100, 950}, false);
	const auto& Metrics = FMaterialGraphGeometry::GetMetrics();
	const float PinY = Metrics.HeaderHeight + Metrics.SecondaryHeight + Metrics.BodyPadding;
	const auto SourceView = *FindViewNode(Document.Inspect(), Other.GeneratedNodeIds[0]);
	const ImVec2 Source{Origin.x + GraphNodeWidth(SourceView), Origin.y + 550 + GraphNodePinOffset(SourceView)};
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
	IO.AddKeyEvent(ImGuiMod_Shift, false);
	const ImVec2 Empty{900, 750};
	IO.AddKeyEvent(ImGuiKey_3, true);
	Frame(Empty, false); Frame(Empty, true);
	ASSERT_EQ(Canvas.GetSelection().size(), 1u);
	const auto CreatedId = FMaterialGraphCanvasTestAccess::ProgramSelection(Canvas).front();
	const auto CreatedView = Document.Inspect();
	const auto* Created = FindViewNode(CreatedView, CreatedId);
	ASSERT_NE(Created, nullptr);
	EXPECT_EQ(Created->Node.ResultType, EMaterialProgramValueType::Float3);
	EXPECT_EQ(Created->Presentation.X, static_cast<int32>(std::round(Empty.x - Origin.x)));
	EXPECT_EQ(Created->Presentation.Y, static_cast<int32>(std::round(Empty.y - Origin.y)));
	IO.AddKeyEvent(ImGuiKey_3, false); Frame(Empty, false);
	IO.AddKeyEvent(IO.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl, true); IO.AddKeyEvent(ImGuiKey_C, true);
	Frame(Empty, false); IO.AddKeyEvent(ImGuiKey_C, false); Frame(Empty, false);
	const ImVec2 PastePoint{650, 700};
	Frame(PastePoint, false); Frame(PastePoint, false);
	for (int Index = 0; Index < 2; ++Index)
	{
		IO.AddKeyEvent(ImGuiKey_V, true); Frame(PastePoint, false);
		const auto Id = FMaterialGraphCanvasTestAccess::ProgramSelection(Canvas).front();
		EXPECT_NE(Id, CreatedId);
		const auto PastedView = Document.Inspect();
		const auto* Pasted = FindViewNode(PastedView, Id);
		ASSERT_NE(Pasted, nullptr);
		EXPECT_EQ(Pasted->Presentation.X, static_cast<int32>(std::round(PastePoint.x - Origin.x)) + 24 * Index);
		EXPECT_EQ(Pasted->Presentation.Y, static_cast<int32>(std::round(PastePoint.y - Origin.y)) + 24 * Index);
		IO.AddKeyEvent(ImGuiKey_V, false); Frame(PastePoint, false);
	}
	EXPECT_EQ(Errors, 0);
	ASSERT_TRUE(Transactions->Undo()); ASSERT_TRUE(Transactions->Redo());
	Canvas.SetViewport(0.3f, {40, 40}); Frame(Empty, false);
	EXPECT_GT(ImGui::GetDrawData()->TotalVtxCount, 0);
	ImGui::DestroyContext(Context);
	EXPECT_TRUE(Transactions->Reset()); MarkAsGarbage(Function); CollectGarbage();
}

TEST(FMaterialGraphInteractionTests, CanvasConnectsASecondFunctionOutputAndRefreshesItsInterface)
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
	EXPECT_FALSE(Document.Connect(FMaterialGraphPinAddress::Input(Surface.GeneratedNodeIds[0], MetallicInput), FMaterialGraphPinAddress::Output(AmountSource)));
	ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::Input(Surface.GeneratedNodeIds[0], MetallicInput), FMaterialGraphPinAddress::Output(AmountSource), true));
	const auto SurfaceInspection = Document.Inspect();
	const auto* SurfaceView = FindViewNode(SurfaceInspection, Surface.GeneratedNodeIds[0]);
	ASSERT_NE(SurfaceView, nullptr);
	ASSERT_EQ(SurfaceView->Inputs.size(), 2u);
	EXPECT_EQ(SurfaceView->Inputs[0].AcceptedTypes, std::vector{EMaterialProgramValueType::Surface});
	EXPECT_EQ(SurfaceView->Inputs[1].Link, AmountSource);
	const auto Destination = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::Saturate, EMaterialProgramValueType::Float, {Constant.GeneratedNodeIds[0]}, 350);
	ASSERT_TRUE(Destination);
	auto Presentation = Material->GetMaterialGraphPresentation();
	Testing::OutputPosition(*Material, Presentation).X = 700;
	Testing::OutputPosition(*Material, Presentation).Y = 0;
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(Presentation) != Durin::EMaterialGraphPresentationResult::Rejected);
	ImGuiContext* Context = ImGui::CreateContext();
	auto& IO = ImGui::GetIO();
	IO.DisplaySize = {1200, 720}; IO.DeltaTime = 1.0f / 60.0f; IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault(); IO.Fonts->Build();
	Durin::Tests::FTestTransactorOwner Transactions;
	FMaterialGraphCanvas Canvas{FMaterialGraphDocument(*Material)};
	Canvas.SetViewport(1.0f, {40, 40});
	ImVec2 Origin;
	int Errors = 0;
	const auto Frame = [&](ImVec2 Mouse, bool Down) {
		IO.AddMousePosEvent(Mouse.x, Mouse.y); IO.AddMouseButtonEvent(ImGuiMouseButton_Left, Down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1200, 720});
		ImGui::Begin("Function Links", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		Canvas.Draw(*Transactions.Get(), 660, [&](std::string) { ++Errors; });
		const auto* Window = ImGui::GetCurrentWindow()->DC.ChildWindows.back();
		Origin = {Window->Pos.x + Window->WindowPadding.x + 40,
			Window->Pos.y + Window->WindowPadding.y + ImGui::GetFrameHeightWithSpacing() + 40};
		ImGui::End(); ImGui::Render();
	};
	Frame({1100, 600}, false); Frame({1100, 600}, false);
	const auto& Metrics = FMaterialGraphGeometry::GetMetrics();
	const float PinY = Metrics.HeaderHeight + Metrics.SecondaryHeight + Metrics.BodyPadding;
	const ImVec2 SecondOutput{Origin.x + Metrics.NodeWidth, Origin.y + PinY + Metrics.PinRowHeight};
	const ImVec2 Target{Origin.x + 350, Origin.y + GraphNodePinOffset(*FindViewNode(FMaterialGraphDocument(*Material).Inspect(), Destination.GeneratedNodeIds[0]))};
	IO.AddKeyEvent(ImGuiMod_Shift, true);
	Frame(SecondOutput, false); Frame(SecondOutput, true);
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::Linking(Canvas));
	Frame(Target, true); Frame(Target, false);
	EXPECT_EQ(Errors, 0);
	const auto Link = FindViewNode(FMaterialGraphDocument(*Material).Inspect(), Destination.GeneratedNodeIds[0])->Inputs[0].Link;
	EXPECT_EQ(Link.SourceNodeId, Call.GeneratedNodeIds[0]);
	EXPECT_EQ(Link.SourceOutputId, Output.Id);
	auto Signature = Function->GetFunctionSignature();
	Signature.Outputs[1].Name = "Renamed Amount";
	ASSERT_TRUE(FunctionDocument.SetPort(true, Signature.Outputs[1]));
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

TEST(FMaterialGraphInteractionTests, ParameterCanvasDragMovesNodeWithoutEditingItsValue)
{
	InitializeDObjectSystem();
	for (const auto Type : {EMaterialProgramValueType::Float, EMaterialProgramValueType::Float4})
	{
		auto* Material = NewObject<DMaterial>(nullptr, NAME_None);
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		FMaterialGraphDocument Document(*Material);
		const auto Created = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::Parameter, Type);
		ASSERT_TRUE(Created);
		const auto Other = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::Parameter,
			EMaterialProgramValueType::Float, {}, 350);
		ASSERT_TRUE(Other);
		const auto Id = Created.GeneratedNodeIds.front();
		const auto ParameterId = FindViewNode(Document.Inspect(), Id)->Node.GetParameterId();
		FResolvedMaterialParameter Initial;
		ASSERT_TRUE(Material->ResolveParameterValue(ParameterId, Initial));
		const auto Presentation = Material->GetMaterialGraphPresentation();
		ImGuiContext* Context = ImGui::CreateContext();
		auto& IO = ImGui::GetIO();
		IO.DisplaySize = {1200, 720}; IO.DeltaTime = 1.0f / 60.0f; IO.IniFilename = nullptr;
		IO.Fonts->AddFontDefault(); IO.Fonts->Build();
		Durin::Tests::FTestTransactorOwner Transactions;
		FMaterialGraphCanvas Canvas{FMaterialGraphDocument(*Material)};
		Canvas.SetViewport(1.0f, {40, 40});
		ImVec2 Origin;
		int Errors = 0;
		const auto Frame = [&](ImVec2 Mouse, bool Down) {
			IO.AddMousePosEvent(Mouse.x, Mouse.y); IO.AddMouseButtonEvent(ImGuiMouseButton_Left, Down);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1200, 720});
			ImGui::Begin("Parameter Drag", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
			Canvas.Draw(*Transactions.Get(), 660, [&](std::string) { ++Errors; });
			const auto* Window = ImGui::GetCurrentWindow()->DC.ChildWindows.back();
			Origin = {Window->Pos.x + Window->WindowPadding.x + 40,
				Window->Pos.y + Window->WindowPadding.y + ImGui::GetFrameHeightWithSpacing() + 40};
			ImGui::End(); ImGui::Render();
		};
		Frame({1100, 600}, false); Frame({1100, 600}, false);
		EXPECT_TRUE(FMaterialGraphCanvasTestAccess::ProgramSelection(Canvas).empty());
		const auto& Metrics = FMaterialGraphGeometry::GetMetrics();
		const ImVec2 Start{Origin.x + 25, Origin.y + Metrics.HeaderHeight + 8};
		Frame(Start, false); Frame(Start, true);
		Frame({Start.x + 20, Start.y}, true);
		Frame({Start.x + 40, Start.y}, true);
		FResolvedMaterialParameter During;
		ASSERT_TRUE(Material->ResolveParameterValue(ParameterId, During));
		EXPECT_EQ(During.Value, Initial.Value);
		EXPECT_FALSE(Transactions->Undo());
		Frame({Start.x + 60, Start.y}, true);
		FResolvedMaterialParameter Final;
		ASSERT_TRUE(Material->ResolveParameterValue(ParameterId, Final));
		EXPECT_EQ(Final.Value, Initial.Value);
		Frame({Start.x + 60, Start.y}, false);
		EXPECT_EQ(Errors, 0);
		EXPECT_NE(Material->GetMaterialGraphPresentation(), Presentation);
		EXPECT_TRUE(Transactions->Undo());
		EXPECT_EQ(Material->GetMaterialGraphPresentation(), Presentation);
		FResolvedMaterialParameter Restored;
		ASSERT_TRUE(Material->ResolveParameterValue(ParameterId, Restored));
		EXPECT_EQ(Restored.Value, Initial.Value);
		EXPECT_FALSE(Transactions->Undo());
		EXPECT_TRUE(Transactions->Redo());
		ASSERT_TRUE(Material->ResolveParameterValue(ParameterId, Restored));
		EXPECT_EQ(Restored.Value, Final.Value);
		ImGui::DestroyContext(Context);
		MarkAsGarbage(Material); CollectGarbage();
	}
}

TEST(FMaterialGraphInteractionTests, CanvasLinkReleaseEndsGestureAcrossFrames)
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
	Testing::OutputPosition(*Material, Presentation).X = 700;
	Testing::OutputPosition(*Material, Presentation).Y = 0;
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(Presentation) != Durin::EMaterialGraphPresentationResult::Rejected);
	ImGuiContext* Context = ImGui::CreateContext();
	auto& IO = ImGui::GetIO();
	IO.DisplaySize = {1200, 720};
	IO.DeltaTime = 1.0f / 60.0f;
	IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault();
	IO.Fonts->Build();
	Durin::Tests::FTestTransactorOwner Transactions;
	FMaterialGraphCanvas Canvas{FMaterialGraphDocument(*Material)};
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
		Canvas.Draw(*Transactions.Get(), 660,
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
	const auto View = FMaterialGraphDocument(*Material).Inspect();
	const float PinY = GraphNodePinOffset(*FindViewNode(View, Destination));
	const auto* SourceView = FindViewNode(View, Source);
	const ImVec2 Output{Origin.x + GraphNodeWidth(*SourceView), Origin.y + GraphNodePinOffset(*SourceView)};
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
	EXPECT_EQ(FindViewNode(FMaterialGraphDocument(*Material).Inspect(), Destination)
		->Inputs.front().Link.SourceNodeId, Source);
	Drop({Origin.x + 700, Origin.y + (GraphNodePinOffset(*FindViewNode(View, Material->GetOutputNode()->Id)) + Metrics.PinRowHeight * 3)}, false);
	EXPECT_EQ(Material->GetExpressionOutputs().Roughness.ExpressionId, Source);
	Drop({Origin.x + 700, Origin.y + (GraphNodePinOffset(*FindViewNode(View, Material->GetOutputNode()->Id)))}, false);
	EXPECT_EQ(Errors, 1);
	EXPECT_EQ(Material->GetExpressionOutputs().BaseColor.ExpressionId, Source);
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

TEST(FMaterialGraphInteractionTests, NumericNodeDisplayTracksValuesAndUndo)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "NumericNodeDisplay");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	Durin::Tests::FTestTransactorOwner Transactions;
	const auto Constant = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::Constant,
		EMaterialProgramValueType::Float, {}, 0, 0, Transactions.Get());
	ASSERT_TRUE(Constant);
	const auto Id = Constant.GeneratedNodeIds.front();
	ASSERT_TRUE(Document.SetConstantValue(Id, FMaterialParameterValue::MakeScalar(0.5f), Transactions.Get()));
	FMaterialGraphCanvas Canvas{FMaterialGraphDocument(*Material)};
	auto Display = MakeGraphNodeDisplay(*FindViewNode(FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material), Id), Material);
	EXPECT_EQ(Display.Title, "0.5");
	EXPECT_EQ(Display.Subtitle, "Constant (Float)");
	ASSERT_TRUE(Document.SetConstantValue(Id, FMaterialParameterValue::MakeVector({1, 0.5f, -2}), Transactions.Get()));
	Display = MakeGraphNodeDisplay(*FindViewNode(FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material), Id), Material);
	EXPECT_EQ(Display.Title, "(1, 0.5, -2)");
	EXPECT_EQ(Display.Subtitle, "Constant (Float3)");
	ASSERT_TRUE(Display.Value);
	EXPECT_FLOAT_EQ(Display.Value->Z, -2); // Display swatches must not alter numeric values.
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(MakeGraphNodeDisplay(*FindViewNode(FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material), Id), Material).Title, "0.5");
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(MakeGraphNodeDisplay(*FindViewNode(FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material), Id), Material).Title, "(1, 0.5, -2)");
	const auto Parameter = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::Parameter,
		EMaterialProgramValueType::Float4, {}, 300, 0, Transactions.Get());
	ASSERT_TRUE(Parameter);
	const auto View = Document.Inspect();
	const auto* Node = FindViewNode(View, Parameter.GeneratedNodeIds.front());
	ASSERT_NE(Node, nullptr);
	ASSERT_TRUE(FMaterialGraphOperations::SetParameterValue(*Material, Node->Node.GetParameterId(),
		FMaterialParameterValue::MakeVector4({0.25f, 0.5f, 2, 1}), Transactions.Get()));
	Display = MakeGraphNodeDisplay(*Node, Material); // Resolve current values even with a cached view.
	EXPECT_EQ(Display.Title, Node->PrimaryLabel);
	EXPECT_EQ(Display.Subtitle, "(0.25, 0.5, 2, 1)");
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_NE(MakeGraphNodeDisplay(*Node, Material).Subtitle, Display.Subtitle);
	EXPECT_EQ(FormatGraphNumericValue(EMaterialProgramValueType::Float2, {-0.0f, 0.123456f}), "(0, 0.1235)");
	EXPECT_EQ(FormatGraphNumericValue(EMaterialProgramValueType::Float, {0.000001f}), "1e-06");
	EXPECT_TRUE(Transactions->Reset());
	MarkAsGarbage(Material); CollectGarbage();
}

TEST(FMaterialGraphInteractionTests, CanvasCreationShortcutsRespectGesturesAndUndo)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "CanvasCreationShortcuts");
	ImGuiContext* Context = ImGui::CreateContext();
	auto& IO = ImGui::GetIO();
	IO.DisplaySize = {1200, 720};
	IO.DeltaTime = 1.0f / 60.0f;
	IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault();
	IO.Fonts->Build();
	Durin::Tests::FTestTransactorOwner Transactions;
	FMaterialGraphCanvas Canvas{FMaterialGraphDocument(*Material)};
	Canvas.SetViewport(0.75f, {40, 40});
	ImVec2 Origin;
	int Errors = 0;
	const auto Frame = [&](ImVec2 Mouse, bool Down, bool TextInput = false) {
		IO.AddMousePosEvent(Mouse.x, Mouse.y);
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, Down);
		ImGui::NewFrame();
		IO.WantTextInput = TextInput;
		ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1200, 720});
		ImGui::Begin("Creation Shortcuts", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		Canvas.Draw(*Transactions.Get(), 660, [&](std::string) { ++Errors; });
		const auto* Window = ImGui::GetCurrentWindow()->DC.ChildWindows.back();
		Origin = {Window->Pos.x + Window->WindowPadding.x + 40,
			Window->Pos.y + Window->WindowPadding.y + ImGui::GetFrameHeightWithSpacing() + 40};
		ImGui::End(); ImGui::Render();
	};
	const ImVec2 Point{600, 500};
	Frame(Point, false); Frame(Point, false);
	struct FCase { ImGuiKey Key; EMaterialProgramOpcode Opcode; EMaterialProgramValueType Type; };
	const FCase Cases[] = {
		{ImGuiKey_1, EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float},
		{ImGuiKey_2, EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float2},
		{ImGuiKey_3, EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float3},
		{ImGuiKey_4, EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float4},
		{ImGuiKey_A, EMaterialProgramOpcode::Add, EMaterialProgramValueType::Float},
		{ImGuiKey_M, EMaterialProgramOpcode::Multiply, EMaterialProgramValueType::Float},
		{ImGuiKey_L, EMaterialProgramOpcode::Lerp, EMaterialProgramValueType::Float},
		{ImGuiKey_U, EMaterialProgramOpcode::TextureCoordinates, EMaterialProgramValueType::Float2},
		{ImGuiKey_S, EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float},
		{ImGuiKey_V, EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float4},
		{ImGuiKey_T, EMaterialProgramOpcode::TextureSampleParameter2D, EMaterialProgramValueType::Float4},
	};
	for (const auto& Case : Cases)
	{
		SCOPED_TRACE(static_cast<int>(Case.Key));
		IO.AddKeyEvent(Case.Key, true);
		Frame(Point, false); Frame(Point, true);
		const auto View = FMaterialGraphDocument(*Material).Inspect();
		ASSERT_EQ(View.Nodes.size(), 2u);
		const auto& CreatedNode = *std::ranges::find_if(View.Nodes, [](const auto& N) { return !N.Node.bMaterialOutput; });
		EXPECT_EQ(CreatedNode.Node.Opcode, Case.Opcode);
		EXPECT_EQ(CreatedNode.Node.ResultType, Case.Type);
		EXPECT_EQ(CreatedNode.Presentation.X, static_cast<int32>(std::round((Point.x - Origin.x) / 0.75f)));
		EXPECT_EQ(CreatedNode.Presentation.Y, static_cast<int32>(std::round((Point.y - Origin.y) / 0.75f)));
		EXPECT_TRUE(Canvas.GetSelection().contains(CreatedNode.Node.Id));
		EXPECT_TRUE(FMaterialGraphCanvasTestAccess::Idle(Canvas));
		Frame(Point, true); // Holding the mouse must not repeat creation.
		EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 2u);
		IO.AddKeyEvent(Case.Key, false);
		Frame(Point, false);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 2u);
		ASSERT_TRUE(Transactions->Undo());
		Frame(Point, false);
	}
	IO.AddKeyEvent(ImGuiKey_M, true);
	for (const auto Modifier : {ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt, ImGuiMod_Super})
	{
		IO.AddKeyEvent(Modifier, true);
		Frame(Point, false); Frame(Point, true); Frame(Point, false);
		EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
		IO.AddKeyEvent(Modifier, false);
		Frame(Point, false);
	}
	Frame(Point, true, true); Frame(Point, false, true);
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
	EXPECT_EQ(Errors, 0);
	Canvas.CancelInteraction();
	EXPECT_TRUE(Transactions->Reset());
	ImGui::DestroyContext(Context);
	MarkAsGarbage(Material); CollectGarbage();
}

TEST(FMaterialGraphInteractionTests, CanvasMenusDoNotInterruptMoveAndKeepSearchRanking)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "MoveMenuRegression");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	ASSERT_TRUE(Testing::CreateGraphConstant(Document, 0.5f, 0, 200));
	const auto Before = Material->GetMaterialGraphPresentation();
	ImGuiContext* Context = ImGui::CreateContext();
	auto& IO = ImGui::GetIO();
	IO.DisplaySize = {1200, 900}; IO.DeltaTime = 1.0f / 60.0f; IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault(); IO.Fonts->Build();
	Durin::Tests::FTestTransactorOwner Transactions;
	FMaterialGraphCanvas Canvas{FMaterialGraphDocument(*Material)};
	ImVec2 Origin;
	int Errors = 0;
	const auto Frame = [&](ImVec2 Mouse, bool Down) {
		IO.AddMousePosEvent(Mouse.x, Mouse.y); IO.AddMouseButtonEvent(ImGuiMouseButton_Left, Down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1200, 900});
		ImGui::Begin("Move Menu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		Canvas.Draw(*Transactions.Get(), 820, [&](std::string) { ++Errors; });
		const auto* Child = ImGui::GetCurrentWindow()->DC.ChildWindows.back();
		Origin = {Child->Pos.x + Child->WindowPadding.x + 40,
			Child->Pos.y + Child->WindowPadding.y + ImGui::GetFrameHeightWithSpacing() + 40};
		ImGui::End(); ImGui::Render();
	};
	Frame({1100, 850}, false); Frame({1100, 850}, false);
	const ImVec2 Header{Origin.x + 20, Origin.y + 212};
	Frame(Header, false); Frame(Header, true);
	const ImVec2 Moved{Header.x + 60, Header.y + 40};
	Frame(Moved, true);
	ASSERT_TRUE(FMaterialGraphCanvasTestAccess::MoveActive(Canvas));
	IO.AddKeyEvent(ImGuiKey_Space, true); Frame(Moved, true);
	EXPECT_FALSE(FMaterialGraphCanvasTestAccess::Menu(Canvas));
	IO.AddKeyEvent(ImGuiKey_Space, false);
	IO.AddMouseButtonEvent(ImGuiMouseButton_Right, true); Frame(Moved, true);
	EXPECT_FALSE(FMaterialGraphCanvasTestAccess::Menu(Canvas));
	IO.AddMouseButtonEvent(ImGuiMouseButton_Right, false); Frame(Moved, false);
	EXPECT_FALSE(FMaterialGraphCanvasTestAccess::MoveActive(Canvas));
	EXPECT_NE(Material->GetMaterialGraphPresentation(), Before);
	ASSERT_TRUE(Transactions->Undo()); EXPECT_EQ(Material->GetMaterialGraphPresentation(), Before);
	ASSERT_TRUE(Transactions->Redo());
	// A second gesture remains available after the menu attempts.
	Frame(Moved, false); Frame(Moved, true);
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::MoveActive(Canvas));
	Canvas.CancelInteraction(); Frame(Moved, false);
	FMaterialGraphCanvasTestAccess::SearchMenu(Canvas, ""); Frame({900, 600}, false);
	FMaterialGraphCanvasTestAccess::Remember(Canvas, EMaterialProgramOpcode::Add);
	FMaterialGraphCanvasTestAccess::Remember(Canvas, EMaterialProgramOpcode::Multiply);
	FMaterialGraphCanvasTestAccess::Remember(Canvas, EMaterialProgramOpcode::Add);
	Frame({900, 600}, false);
	FMaterialGraphCanvasTestAccess::CheckRecentRows(Canvas);
	FMaterialGraphCanvasTestAccess::SearchMenu(Canvas, "texture"); Frame({900, 600}, false);
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::SearchOrder(Canvas));
	EXPECT_EQ(Errors, 0);
	Canvas.CancelInteraction(); ImGui::DestroyContext(Context);
	EXPECT_TRUE(Transactions->Reset()); MarkAsGarbage(Material); CollectGarbage();
}

TEST(FMaterialGraphInteractionTests, FunctionCanvasCacheTracksPositionsAndDependencies)
{
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "CachedFunction");
	auto* Dependency = NewObject<DMaterialFunction>(nullptr, "CachedDependency");
	FMaterialGraphDocument Document(*Function), DependencyDocument(*Dependency);
	const auto Call = Document.InsertFunctionCall(*Dependency, 100, 200);
	ASSERT_TRUE(Call);
	FMaterialGraphCanvas Canvas{FMaterialGraphDocument(*Function)};
	// Details initializes the shared cache even before the canvas is drawn.
	FMaterialGraphCanvasTestAccess::Details(Canvas, *Function);
	FMaterialGraphCanvasTestAccess::PrepareVisuals(Canvas);
	FMaterialGraphCanvasTestAccess::PrepareFunction(Canvas, *Function);
	EXPECT_FALSE(FMaterialGraphCanvasTestAccess::TopologyStale(Canvas));
	auto Positions = Function->GetFunctionPresentation();
	Positions.Nodes.front().X += 70;
	ASSERT_TRUE(Function->SetFunctionPresentation(Positions));
	const auto& Moved = FMaterialGraphCanvasTestAccess::PrepareFunction(Canvas, *Function);
	EXPECT_EQ(FindViewNode(Moved, Positions.Nodes.front().NodeId)->Presentation.X, Positions.Nodes.front().X);
	FMaterialGraphCanvasTestAccess::PrepareVisuals(Canvas);
	const auto Value = Testing::CreateGraphConstant(DependencyDocument, 0.5f);
	ASSERT_TRUE(Value);
	ASSERT_TRUE(DependencyDocument.AddPort(true, {.Name = "Extra"}, {Value.GeneratedNodeIds.front()}));
	FMaterialGraphCanvasTestAccess::PrepareFunction(Canvas, *Function);
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::TopologyStale(Canvas));
	const auto& Details = FMaterialGraphCanvasTestAccess::Details(Canvas, *Function);
	EXPECT_EQ(FindViewNode(Details, Call.GeneratedNodeIds.front())->Outputs.size(), Dependency->GetFunctionSignature().Outputs.size());
	auto* Leaf = NewObject<DMaterialFunction>(nullptr, "CachedLeafDependency");
	ASSERT_TRUE(DependencyDocument.InsertFunctionCall(*Leaf, 0, 0));
	FMaterialGraphCanvasTestAccess::Details(Canvas, *Function);
	FMaterialGraphCanvasTestAccess::PrepareVisuals(Canvas);
	ASSERT_TRUE(Testing::CreateGraphConstant(FMaterialGraphDocument(*Leaf), .8f));
	FMaterialGraphCanvasTestAccess::Details(Canvas, *Function);
	// A transitive implementation edit does not change this document's visible pins.
	EXPECT_FALSE(FMaterialGraphCanvasTestAccess::TopologyStale(Canvas));
	MarkAsGarbage(Leaf);
	MarkAsGarbage(Function); MarkAsGarbage(Dependency); CollectGarbage();
}

TEST(FMaterialGraphInteractionTests, CanvasProducesValidDrawDataAcrossZoomAndGraphSizes)
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
	FMaterialGraphCanvas Canvas{FMaterialGraphDocument(*Material)};

	const auto DrawAtZoom = [&](float Zoom) {
		Canvas.SetViewport(Zoom, {40.0f, 40.0f});
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0.0f, 0.0f});
		ImGui::SetNextWindowSize({1200.0f, 720.0f});
		ImGui::Begin("Material Graph Render Test", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		Canvas.Draw(*Transactions.Get(), 660.0f,
			[](std::string Message) { FAIL() << Message; });
		ImGui::End();
		ImGui::Render();
		const ImDrawData* DrawData = ImGui::GetDrawData();
		ASSERT_NE(DrawData, nullptr);
		EXPECT_TRUE(DrawData->Valid);
		EXPECT_GT(DrawData->TotalVtxCount, 0);
		EXPECT_GT(DrawData->TotalIdxCount, 0);
	};
	DrawAtZoom(1.0f);
	DrawAtZoom(0.30f);

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
	ASSERT_TRUE(FMaterialGraphDocument(*Material).Layout());
	DrawAtZoom(0.55f);

	uint32 MaximumIndex = 1000;
	while (Graph.Expressions.size() < MaterialProgramMaxNodeCount - 1)
		Graph.Expressions.emplace_back(Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>(FGuid(MaximumIndex++, 0, 0, 1)).Get());
	ASSERT_TRUE(Graph.Apply(*Material));
	ASSERT_TRUE(FMaterialGraphDocument(*Material).Layout());
	DrawAtZoom(0.30f);

	EXPECT_TRUE(Transactions->Reset());
	ImGui::DestroyContext(Context);
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphInteractionTests, SurfaceTexturesUseCompactSamplesAndPreserveUndo)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	auto* Material = NewObject<DMaterial>(nullptr, "CompactSurfaceTextures");
	ASSERT_NE(Material, nullptr);
	// Keep every output and Undo/Redo assertion without compiling each intermediate graph.
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	Durin::Tests::FTestTransactorOwner Transactions;
	constexpr std::array<uint8, 8> Channels{1, 1, 4, 3, 2, 1, 5, 2};
	for (uint32 Index = 0; Index < Channels.size(); ++Index)
	{
		SCOPED_TRACE(Index);
		const auto Role = static_cast<EMaterialSurfaceOutput>(Index);
		const bool bNormal = Role == EMaterialSurfaceOutput::Normal;
		const auto Result = FMaterialGraphOperations::AddTextureToSurfaceOutput(*Material,
			{.Output = Role, .X = 400, .Y = 200}, Transactions.Get());
		ASSERT_TRUE(Result) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Result);
		ASSERT_EQ(Material->GetExpressionCollection().Expressions.size(), 2u);
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
		EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
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
	Testing::OutputPosition(*Material, Presentation).X = 760;
	Testing::OutputPosition(*Material, Presentation).Y = 100;
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(std::move(Presentation)) != Durin::EMaterialGraphPresentationResult::Rejected);
	ImGuiContext* Context = ImGui::CreateContext();
	auto& IO = ImGui::GetIO();
	IO.DisplaySize = {1200, 850}; IO.DeltaTime = 1.f / 60; IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault(); IO.Fonts->Build();
	{
		FMaterialGraphCanvas Canvas{FMaterialGraphDocument(*Material)};
		Canvas.SetViewport(.85f, {30, 50});
		int Errors = 0;
		for (int Frame = 0; Frame < 2; ++Frame)
		{
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(IO.DisplaySize);
			ImGui::Begin("Compact Texture Authoring", nullptr, ImGuiWindowFlags_NoResize);
			Canvas.Draw(*Transactions.Get(), 780, [&](std::string) { ++Errors; });
			ImGui::End(); ImGui::Render();
		}
		EXPECT_EQ(Errors, 0);
	}
	ImGui::DestroyContext(Context);
	EXPECT_TRUE(Transactions->Reset());
	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialGraphInteractionTests, DocumentDockLayoutsRemainIsolatedAndSurviveHiddenFrames)
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

TEST(FMaterialGraphInteractionTests, PreviewFramingFitsBothAxesAcrossViewportShapes)
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

TEST(FMaterialGraphInteractionTests, SharedTextureParametersPreserveLocalSamplingAndRoundTrip)
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

// Replay the same real ImGui gestures through both document renderers.


class FMaterialGraphCanvasInteractionTests : public ::testing::TestWithParam<bool> {};

TEST_P(FMaterialGraphCanvasInteractionTests, SelectionReconnectionCreationAndKeyboardAgree)
{
	FScopedGraphClipboard Clipboard;
	InitializeDObjectSystem();
	const bool bFunction = GetParam();
	DObject* Owner = bFunction ? static_cast<DObject*>(NewObject<DMaterialFunction>(nullptr, "FunctionGestures"))
		: static_cast<DObject*>(NewObject<DMaterial>(nullptr, "MaterialGestures"));
	FMaterialGraphDocument Document(*Owner);
	{
		GraphEditInternals::FGraphEditSession Initial(*Owner);
		for (auto& Position : Initial.Presentation.Nodes) { Position.X = 1800; Position.Y = 1200; }
		ASSERT_TRUE(Initial.Commit("Arrange fixture", nullptr));
	}
	auto Source = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
	auto Previous = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
	auto Consumer = Testing::MakeGraphExpression<DMaterialExpressionSaturate>();
	Consumer->Input = {Previous->Id};
	ASSERT_TRUE(Document.CreateExpression(*Source.Get(), 0, 0));
	ASSERT_TRUE(Document.CreateExpression(*Previous.Get(), 0, 280));
	ASSERT_TRUE(Document.CreateExpression(*Consumer.Get(), 350, 0));
	ImGuiContext* Context = ImGui::CreateContext();
	auto& IO = ImGui::GetIO();
	IO.DisplaySize = {1200, 760}; IO.DeltaTime = 1.0f / 60.0f; IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault(); IO.Fonts->Build();
	Durin::Tests::FTestTransactorOwner Transactions;
	FMaterialGraphCanvas Canvas{FMaterialGraphDocument(*Owner)};
	Canvas.SetViewport(1.0f, {40, 40});
	ImVec2 Origin;
	int Errors = 0;
	const auto Frame = [&](ImVec2 Mouse, bool Down) {
		IO.AddMousePosEvent(Mouse.x, Mouse.y);
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, Down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1200, 760});
		ImGui::Begin("Shared gestures", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		Canvas.Draw(*Transactions.Get(), 660, [&](std::string) { ++Errors; });
		const auto* Child = ImGui::GetCurrentWindow()->DC.ChildWindows.back();
		Origin = {Child->Pos.x + Child->WindowPadding.x + 40,
			Child->Pos.y + Child->WindowPadding.y + 40 + ImGui::GetFrameHeightWithSpacing()};
		ImGui::End(); ImGui::Render();
	};
	const auto Click = [&](ImVec2 At) { Frame(At, false); Frame(At, true); Frame(At, false); };
	const auto Key = [&](ImGuiKey Code) {
		IO.AddKeyEvent(Code, true); Frame({1050, 580}, false);
		IO.AddKeyEvent(Code, false); Frame({1050, 580}, false);
	};
	Frame({1100, 600}, false); Frame({1100, 600}, false);
	const ImVec2 Header{Origin.x + 80, Origin.y + 12};
	Click(Header);
	EXPECT_TRUE(Canvas.GetSelection().contains(Source->Id));
	IO.AddKeyEvent(IO.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl, true); Frame(Header, false); Click(Header);
	EXPECT_FALSE(Canvas.GetSelection().contains(Source->Id));
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::Idle(Canvas));
	Click(Header);
	EXPECT_TRUE(Canvas.GetSelection().contains(Source->Id));
	IO.AddKeyEvent(IO.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl, false); Frame(Header, false);

	// Empty-space marquee replaces selection; Shift adds another region.
	Frame({Origin.x + 330, Origin.y - 10}, true);
	Frame({Origin.x + 590, Origin.y + 150}, true);
	Frame({Origin.x + 590, Origin.y + 150}, false);
	EXPECT_TRUE(Canvas.GetSelection().contains(Consumer->Id));
	EXPECT_FALSE(Canvas.GetSelection().contains(Source->Id));
	IO.AddKeyEvent(ImGuiMod_Shift, true); Frame({Origin.x - 10, Origin.y - 10}, false);
	Frame({Origin.x - 10, Origin.y - 10}, true);
	Frame({Origin.x + 245, Origin.y + 150}, true);
	Frame({Origin.x + 245, Origin.y + 150}, false);
	EXPECT_TRUE(Canvas.GetSelection().contains(Source->Id));
	EXPECT_TRUE(Canvas.GetSelection().contains(Consumer->Id));
	IO.AddKeyEvent(ImGuiMod_Shift, false); Frame({1050, 580}, false);

	// A group drag is one undo step, and Escape discards the entire draft.
	Frame(Header, true); Frame({Header.x + 40, Header.y + 20}, true);
	Frame({Header.x + 40, Header.y + 20}, false);
	EXPECT_EQ(FindViewNode(Document.Inspect(), Source->Id)->Presentation.X, 40);
	EXPECT_EQ(FindViewNode(Document.Inspect(), Consumer->Id)->Presentation.X, 390);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(FindViewNode(Document.Inspect(), Source->Id)->Presentation.X, 0);
	EXPECT_EQ(FindViewNode(Document.Inspect(), Consumer->Id)->Presentation.X, 350);
	Frame(Header, false); Frame(Header, true); Frame({Header.x + 60, Header.y + 30}, true);
	IO.AddKeyEvent(ImGuiKey_Escape, true); Frame({Header.x + 60, Header.y + 30}, true);
	IO.AddKeyEvent(ImGuiKey_Escape, false); Frame(Header, false);
	EXPECT_EQ(FindViewNode(Document.Inspect(), Source->Id)->Presentation.X, 0);
	EXPECT_EQ(FindViewNode(Document.Inspect(), Consumer->Id)->Presentation.X, 350);

	const auto& Metrics = FMaterialGraphGeometry::GetMetrics();
	const float PinY = Metrics.HeaderHeight + Metrics.SecondaryHeight + Metrics.BodyPadding;
	const auto View = Document.Inspect();
	const ImVec2 Input{Origin.x + 350, Origin.y + GraphNodePinOffset(*FindViewNode(View, Consumer->Id))};
	const auto* SourceView = FindViewNode(View, Source->Id);
	const ImVec2 Output{Origin.x + GraphNodeWidth(*SourceView), Origin.y + GraphNodePinOffset(*SourceView)};
	const auto SourceLink = [&] { return FindViewNode(Document.Inspect(), Consumer->Id)->Inputs.front().Link.SourceNodeId; };
	Frame(Input, true); Frame(Output, true);
	EXPECT_EQ(SourceLink(), Previous->Id); // Preserve the authored link throughout the drag.
	Frame(Output, false);
	EXPECT_EQ(SourceLink(), Source->Id);
	ASSERT_TRUE(Transactions->Undo()); EXPECT_EQ(SourceLink(), Previous->Id);
	ASSERT_TRUE(Transactions->Redo()); EXPECT_EQ(SourceLink(), Source->Id);
	Frame(Input, false); Frame(Input, true); Frame({1000, 560}, true); Frame({1000, 560}, false);
	EXPECT_EQ(SourceLink(), Source->Id);
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::Idle(Canvas));
	Frame(Input, true); IO.AddKeyEvent(ImGuiKey_Escape, true); Frame(Output, true);
	IO.AddKeyEvent(ImGuiKey_Escape, false); Frame(Output, false);
	EXPECT_EQ(SourceLink(), Source->Id);
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::Idle(Canvas));

	// Stable function-call input IDs also reconnect through either owner.
	auto* Dependency = NewObject<DMaterialFunction>(nullptr, "GestureDependency");
	FMaterialGraphDocument DependencyDocument(*Dependency);
	ASSERT_TRUE(DependencyDocument.AddPort(false, {.Name = "Amount", .Default = {.Kind = EMaterialFunctionDefaultKind::Numeric}}));
	const auto PortId = Dependency->GetFunctionSignature().Inputs.back().Id;
	const auto Call = Document.InsertFunctionCall(*Dependency, 700, 0);
	ASSERT_TRUE(Call);
	const auto CallId = Call.GeneratedNodeIds.front();
	ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::Input(CallId, 0, PortId), FMaterialGraphPinAddress::Output({Previous->Id})));
	const auto CallView = Document.Inspect();
	const auto* CallNode = FindViewNode(CallView, CallId);
	const auto Port = std::ranges::find(CallNode->Inputs, PortId, &FMaterialGraphPinView::PortId);
	ASSERT_NE(Port, CallNode->Inputs.end());
	const ImVec2 CallInput{Origin.x + 700,
		Origin.y + PinY + static_cast<float>(Port - CallNode->Inputs.begin()) * Metrics.PinRowHeight};
	Frame(CallInput, false); Frame(CallInput, true); Frame(Output, true); Frame(Output, false);
	const auto ConnectedView = Document.Inspect();
	const auto* ConnectedCall = FindViewNode(ConnectedView, CallId);
	EXPECT_EQ(std::ranges::find(ConnectedCall->Inputs, PortId, &FMaterialGraphPinView::PortId)->Link.SourceNodeId, Source->Id);
	ASSERT_TRUE(Transactions->Undo());
	const auto UndoneView = Document.Inspect();
	const auto* UndoneCall = FindViewNode(UndoneView, CallId);
	EXPECT_EQ(std::ranges::find(UndoneCall->Inputs, PortId, &FMaterialGraphPinView::PortId)->Link.SourceNodeId, Previous->Id);

	// Shared keyboard commands retain selection and one-step undo.
	FMaterialGraphCanvasTestAccess::Select(Canvas, {Source->Id});
	const auto BeforeDuplicate = Document.Inspect().Nodes.size();
	IO.AddKeyEvent(IO.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl, true); Frame({1050, 580}, false);
	Key(ImGuiKey_D);
	EXPECT_EQ(Document.Inspect().Nodes.size(), BeforeDuplicate + 1);
	EXPECT_EQ(Canvas.GetSelection().size(), 1u);
	EXPECT_FALSE(Canvas.GetSelection().contains(Source->Id));
	Key(ImGuiKey_X);
	EXPECT_EQ(Document.Inspect().Nodes.size(), BeforeDuplicate);
	EXPECT_TRUE(Canvas.GetSelection().empty());
	Key(ImGuiKey_V);
	EXPECT_EQ(Document.Inspect().Nodes.size(), BeforeDuplicate + 1);
	Key(ImGuiKey_A);
	EXPECT_EQ(Canvas.GetSelection().size(), Document.Inspect().Nodes.size());
	IO.AddKeyEvent(IO.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl, false); Frame({1050, 580}, false);

	// Blank double click opens creation instead of starting a second marquee.
	Click({850, 510}); Click({850, 510});
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::Menu(Canvas));
	EXPECT_EQ(Errors, 0);
	Canvas.CancelInteraction();
	EXPECT_TRUE(Transactions->Reset());
	ImGui::DestroyContext(Context);
	MarkAsGarbage(Owner); MarkAsGarbage(Dependency); CollectGarbage();
}

TEST_P(FMaterialGraphCanvasInteractionTests, SurfaceDetailsCommitImmediatelyAndUndo)
{
	InitializeDObjectSystem();
	DObject* Owner = GetParam() ? static_cast<DObject*>(NewObject<DMaterialFunction>(nullptr, "FunctionDetails"))
		: static_cast<DObject*>(NewObject<DMaterial>(nullptr, "MaterialDetails"));
	FMaterialGraphDocument Document(*Owner);
	auto Surface = Testing::MakeGraphExpression<DMaterialExpressionGetSurfaceAttributes>();
	Surface->AttributeMask = 3;
	ASSERT_TRUE(Document.CreateExpression(*Surface.Get(), 0, 0));
	Durin::Tests::FTestTransactorOwner Transactions;
	FMaterialGraphCanvas Canvas{FMaterialGraphDocument(*Owner)};
	FMaterialGraphCanvasTestAccess::Select(Canvas, {Surface->Id});
	auto* Context = ImGui::CreateContext();
	auto& IO = ImGui::GetIO();
	IO.DisplaySize = {800, 600}; IO.DeltaTime = 1.f / 60; IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault(); IO.Fonts->Build();
	ImVec2 Checkbox;
	const auto Frame = [&](ImVec2 Mouse, bool Down) {
		IO.AddMousePosEvent(Mouse.x, Mouse.y); IO.AddMouseButtonEvent(ImGuiMouseButton_Left, Down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({600, 500});
		ImGui::Begin("Shared Details", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		ImGui::PushID(Surface->Id.ToString().c_str());
		const auto TableId = ImGui::GetID("NodeProperties");
		ImGui::PopID();
		Canvas.DrawSelectionDetails(*Transactions.Get(), [](std::string Error) { ADD_FAILURE() << Error; });
		const auto* Table = Context->Tables.GetByKey(TableId);
		if (!Table) ADD_FAILURE() << "Missing Details table";
		if (Table) Checkbox = {Table->Columns[1].WorkMinX + ImGui::GetFrameHeight() / 2,
			Table->OuterRect.Min.y + DetailsStyle::MakeTableConfig().CellPadding.y + ImGui::GetFrameHeight() / 2};
		ImGui::End(); ImGui::Render();
	};
	Frame({750, 550}, false); Frame({750, 550}, false);
	Frame(Checkbox, false); Frame(Checkbox, true); Frame(Checkbox, false);
	const auto& Expressions = GetParam() ? Cast<DMaterialFunction>(Owner)->GetExpressionCollection().Expressions
		: Cast<DMaterial>(Owner)->GetExpressionCollection().Expressions;
	const auto Node = std::ranges::find(Expressions, Surface->Id, [](const auto& E) { return E->Id; });
	ASSERT_NE(Node, Expressions.end());
	const auto* Edited = Cast<DMaterialExpressionGetSurfaceAttributes>(Node->Get());
	ASSERT_NE(Edited, nullptr);
	EXPECT_EQ(Edited->AttributeMask, 2);
	EXPECT_EQ(Transactions->GetUndoCount(), 1u);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Edited->AttributeMask, 3);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Edited->AttributeMask, 2);
	ImGui::DestroyContext(Context);
	EXPECT_TRUE(Transactions->Reset());
	MarkAsGarbage(Owner); CollectGarbage();
}

INSTANTIATE_TEST_SUITE_P(MaterialAndFunction, FMaterialGraphCanvasInteractionTests,
	::testing::Bool(), [](const auto& Info) { return Info.param ? "Function" : "Material"; });

TEST(FMaterialGraphInteractionTests, FunctionCreationActionsShareCompatibilityAndPortIdentity)
{
	InitializeDObjectSystem();
	const auto Root = Testing::CreateTestFixtureDirectory("CreationActions");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/CreationActions/", .Owner = EMountOwner::Test,
		.Root = Root, .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid());
	ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CreationActions/Function", Path));
	DMaterialFunction* Callee = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Callee));
	FMaterialGraphDocument Function(*Callee);
	FMaterialFunctionPort Port;
	Port.Id = FGuid::NewGuid(); Port.Type = EMaterialProgramValueType::Float3; Port.Name = "Vector"; Port.bRequired = true;
	ASSERT_TRUE(Function.AddPort(false, Port));
	ASSERT_TRUE(SavePackage(Callee->GetPackage()));
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	const auto Scalar = Testing::CreateGraphConstant(Document, .3f);
	ASSERT_TRUE(Scalar);
	const auto Action = MakeFunctionCreationAction(Callee->GetObjectPath());
	EXPECT_TRUE(Document.CanCreate(Action, EMaterialProgramValueType::Float));
	EXPECT_TRUE(Document.CanCreate(Action, EMaterialProgramValueType::Float3));
	EXPECT_FALSE(Document.CanCreate(Action, EMaterialProgramValueType::Texture2D));
	Durin::Tests::FTestTransactorOwner Transactions;
	const auto Before = CaptureExpressions(*Material);
	const auto Created = Document.Create({Action, 32, 64,
		FMaterialGraphPinAddress::Output({Scalar.GeneratedNodeIds[0]})}, Transactions.Get());
	ASSERT_TRUE(Created) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Created);
	const auto Id = Created.GeneratedNodeIds[0];
	const auto View = Document.Inspect();
	const auto* Node = FindViewNode(View, Id);
	ASSERT_NE(Node, nullptr);
	const auto Pin = std::ranges::find(Node->Inputs, Port.Id, &FMaterialGraphPinView::PortId);
	ASSERT_NE(Pin, Node->Inputs.end());
	EXPECT_TRUE(IsGraphInputCompatible(Pin->AcceptedTypes, EMaterialProgramValueType::Float));
	EXPECT_EQ(Pin->Link.SourceNodeId, Scalar.GeneratedNodeIds[0]);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_FALSE(Transactions->CanUndo());
	ASSERT_TRUE(Transactions->Redo());
	Port.Name = "Renamed"; Port.DisplayOrder = -50;
	ASSERT_TRUE(Function.SetPort(false, Port));
	const auto Address = FMaterialGraphPinAddress::Input(Id, 999, Port.Id);
	FMaterialInputDefault Default;
	Default.Kind = EMaterialInputDefaultKind::Literal; Default.Type = EMaterialProgramValueType::Float3;
	Default.Literal = {.1f, .2f, .3f, 0.f};
	ASSERT_TRUE(Document.SetInputDefault(Id, 0, Default, Port.Id));
	ASSERT_TRUE(Document.Disconnect(Address, Transactions.Get()));
	const auto Disconnected = Document.Inspect();
	const auto* Call = FindViewNode(Disconnected, Id);
	ASSERT_NE(Call, nullptr);
	const auto Retained = std::ranges::find(Call->Inputs, Port.Id, &FMaterialGraphPinView::PortId);
	ASSERT_NE(Retained, Call->Inputs.end());
	EXPECT_FALSE(Retained->Link.SourceNodeId.IsValid());
	EXPECT_EQ(Retained->InlineDefault, Default);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_TRUE(Document.Connect(Address, FMaterialGraphPinAddress::Output({Scalar.GeneratedNodeIds[0]})));
	EXPECT_FALSE(Document.Connect(FMaterialGraphPinAddress::Input(Id, 0, FGuid::NewGuid()),
		FMaterialGraphPinAddress::Output({Scalar.GeneratedNodeIds[0]})));
	EXPECT_FALSE(Document.CanCreate(MakePortCreationAction(false, EMaterialProgramValueType::Float)));
	const auto OutputAction = MakePortCreationAction(true, EMaterialProgramValueType::Float3);
	EXPECT_TRUE(Function.CanCreate(OutputAction, EMaterialProgramValueType::Float));
	const auto Input = Function.Create({MakePortCreationAction(false, EMaterialProgramValueType::Float)});
	ASSERT_TRUE(Input);
	ASSERT_TRUE(Transactions->Reset());
	const auto Signature = Callee->GetFunctionSignature();
	const auto Output = Function.Create({OutputAction, 10, 20,
		FMaterialGraphPinAddress::Output({Input.GeneratedNodeIds[0]})}, Transactions.Get());
	ASSERT_TRUE(Output) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(Output);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Callee->GetFunctionSignature(), Signature);
	EXPECT_FALSE(Transactions->CanUndo());
	for (const auto Type : {EMaterialProgramValueType::Texture2D, EMaterialProgramValueType::Surface})
	{
		const auto TypedInput = Function.Create({MakePortCreationAction(false, Type)});
		ASSERT_TRUE(TypedInput) << ::Durin::Editor::Material::FormatMaterialGraphCommandResult(TypedInput);
	}
}
