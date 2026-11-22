#include "MaterialGraph.h"
#include "Graph/MaterialGraphEditSession.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "Graph/MaterialGraphInteraction.h"
#include "Graph/MaterialGraphCatalog.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "Materials/MaterialExpressionMaterialAttributes.h"
#include "Materials/MaterialExpressionArithmetic.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Misc/UnifiedError.h"
#include "NativePackageUnloadTestSupport.h"
#include "DObject/Package.h"
#include "Graph/MaterialEditingOperations.h"
#include "ThirdParty/ImGui/imgui_internal.h"
#include "MaterialGraphTestSupport.h"
#include "Widgets/MaterialDetailsStyle.h"
#include "Settings/MaterialEditorProfileSettings.h"
#include "Misc/Paths.h"
#include "FMaterialEditor.h"
#include "Widgets/MaterialEditingSession.h"
#include "MaterialEditorModule.h"
#include "Editor/AssetEditorTestHost.h"
#include "Editor/AssetEditorTestQueries.h"
#include "Modules/ModuleTestSupport.h"
#include "Asset/Testing.h"
#include "Asset/PackageReload.h"

TEST(FMaterialGraphInteractionTests, ParameterValueControlsPreviewCoalesceAndCancel)
{
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture MountRegistry;
	Testing::RegisterMountPointForTests("/ParameterGestures/", Testing::GetTestWorkDirectory().generic_string() + "/");
	for (const auto Presentation : {EMaterialParameterPresentation::Drag, EMaterialParameterPresentation::Default})
	{
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate(std::format("/ParameterGestures/{}", FGuid::NewGuid().ToString()), Path));
		auto* Package = CreatePackage(Path);
		ASSERT_NE(Package, nullptr);
		TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(Package, "Material"));
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		FMaterialParameterDefinition Definition;
		Definition.Id = FGuid::NewGuid(); Definition.Name = "Continuous"; Definition.DisplayName = "Continuous";
		Definition.Presentation = Presentation;
		Definition.Type = Presentation == EMaterialParameterPresentation::Drag ? EMaterialParameterType::Scalar : EMaterialParameterType::Vector4;
		Definition.Value = Definition.Type == EMaterialParameterType::Scalar ? FMaterialParameterValue::MakeScalar(.2f)
			: FMaterialParameterValue::MakeVector4({.2, .3, .4, 1});
		ASSERT_TRUE(MaterialEditing::CreateParameter(*Material, Definition, Durin::ETransactionMode::NotTransactable));
		Package->ClearDirty();
		const auto Original = Material->FindParameterDefinition(Definition.Id)->Value;
		const auto Generation = Material->GetMaterialCompileDebugInfo().RequestId;
		Durin::Tests::FTestEditorTransactorOwner Transactions;
		int Errors = 0;
		FMaterialGraphCanvas Canvas{*DMaterialGraph::Create(*Material), {.ReportError = [&](std::string Error) { ++Errors; ADD_FAILURE() << Error; }}};
		auto* Context = ImGui::CreateContext();
		auto& IO = ImGui::GetIO();
		IO.DisplaySize = {900, 500}; IO.DeltaTime = 1.f / 60; IO.IniFilename = nullptr;
		IO.MouseDoubleClickTime = 0.f; // Each click or drag below is a separate gesture.
		IO.Fonts->AddFontDefault(); IO.Fonts->Build();
		ImVec2 Start;

		const auto Frame = [&](ImVec2 Mouse, bool Down, bool Visible = true) {
			const auto BeforeDraw = Material->FindParameterDefinition(Definition.Id)->Value;
			IO.AddMousePosEvent(Mouse.x, Mouse.y); IO.AddMouseButtonEvent(ImGuiMouseButton_Left, Down);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({800, 400});
			ImGui::Begin("Parameter value", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
			if (Visible && MonaImGui::PropertyEdit::BeginTable("Values", DetailsStyle::MakeTableConfig()))
			{
				Canvas.DrawParameterValue(*Material->FindParameterDefinition(Definition.Id), *Transactions.Get());
				Start = {ImGui::GetItemRectMin().x + 30, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) / 2};
				MonaImGui::PropertyEdit::EndTable();
			}
			EXPECT_EQ(Material->FindParameterDefinition(Definition.Id)->Value, BeforeDraw);
			Canvas.EndParameterFrame();
			ImGui::End(); ImGui::Render();
		};
		Frame({850, 450}, false); Frame({850, 450}, false);
		const auto Drag = [&]() {
			Frame(Start, false); Frame(Start, true);
			Frame({Start.x + 30, Start.y}, true); Frame({Start.x + 60, Start.y}, true);
		};
		Frame(Start, false); Frame(Start, true); Frame(Start, false);
		EXPECT_EQ(Material->FindParameterDefinition(Definition.Id)->Value, Original);
		EXPECT_EQ(Transactions->GetQueueLength(), 0u);
		EXPECT_FALSE(Package->IsDirty());
		if (Presentation == EMaterialParameterPresentation::Drag)
		{
			Drag();
			ASSERT_NE(Material->FindParameterDefinition(Definition.Id)->Value, Original);
			Frame(Start, true); Frame(Start, false);
			EXPECT_EQ(Material->FindParameterDefinition(Definition.Id)->Value, Original);
			EXPECT_EQ(Transactions->GetQueueLength(), 0u);
			EXPECT_FALSE(Package->IsDirty());
		}
		Drag();
		const auto Preview = Material->FindParameterDefinition(Definition.Id)->Value;
		EXPECT_NE(Preview, Original);
		EXPECT_FALSE(Package->IsDirty());
		EXPECT_EQ((Transactions->GetQueueLength() - Transactions->GetUndoCount()), 0u);
		Frame({Start.x + 60, Start.y}, false);
		EXPECT_TRUE(Package->IsDirty());
		EXPECT_EQ((Transactions->GetQueueLength() - Transactions->GetUndoCount()), 1u);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(Material->FindParameterDefinition(Definition.Id)->Value, Original);
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(Material->FindParameterDefinition(Definition.Id)->Value, Preview);
		Transactions->Reset();
		Package->ClearDirty();
		Drag();
		IO.AddKeyEvent(ImGuiKey_Escape, true); Frame({Start.x + 60, Start.y}, true);
		IO.AddKeyEvent(ImGuiKey_Escape, false); Frame({Start.x + 60, Start.y}, false);
		EXPECT_FALSE(Package->IsDirty());
		EXPECT_EQ(Material->FindParameterDefinition(Definition.Id)->Value, Preview);
		EXPECT_EQ((Transactions->GetQueueLength() - Transactions->GetUndoCount()), 0u);
		Drag();
		Frame({Start.x + 60, Start.y}, true, false);
		EXPECT_FALSE(Package->IsDirty());
		EXPECT_EQ(Material->FindParameterDefinition(Definition.Id)->Value, Preview);
		EXPECT_EQ((Transactions->GetQueueLength() - Transactions->GetUndoCount()), 0u);
		EXPECT_EQ(Material->GetMaterialCompileDebugInfo().RequestId, Generation);
		EXPECT_EQ(Errors, 0);
		Canvas.CancelInteraction();
		ImGui::DestroyContext(Context);
		MarkObjectHierarchyAsGarbage(Package);
	}
}

TEST(FMaterialGraphInteractionTests, ColorPopupCommitsOnceOrRestoresHDRDefault)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialParameterDefinition Definition;
	Definition.Id = FGuid::NewGuid();
	Definition.Name = "HDRColor";
	Definition.DisplayName = "HDR Color";
	Definition.Presentation = EMaterialParameterPresentation::Color;
	Definition.Type = EMaterialParameterType::Vector4;
	Definition.Value = FMaterialParameterValue::MakeVector4({2.5, 0.3, 0.4, 1});
	ASSERT_TRUE(MaterialEditing::CreateParameter(*Material, Definition, Durin::ETransactionMode::NotTransactable));
	const auto Read = [&] { return Material->FindParameterDefinition(Definition.Id)->Value; };
	const auto Original = Read();
	ASSERT_GT(Original.GetVector4().x, 1.0);
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	FMaterialGraphCanvas Canvas{*DMaterialGraph::Create(*Material),
		{.ReportError = [](std::string Error) { ADD_FAILURE() << Error; }}};
	const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> Context(ImGui::CreateContext(), ImGui::DestroyContext);
	auto& IO = ImGui::GetIO();
	IO.ConfigInputTrickleEventQueue = false;
	IO.DisplaySize = {900, 600}; IO.DeltaTime = 1.f / 60; IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault(); IO.Fonts->Build();
	ImGui::SetColorEditOptions(ImGuiColorEditFlags_PickerHueBar);
	ImGuiID PickerId = 0;
	const auto Frame = [&](bool bOpen = false, bool bVisible = true) {
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({650, 350});
		ImGui::Begin("Graph color parameter", nullptr, ImGuiWindowFlags_NoTitleBar);
		if (bVisible && MonaImGui::PropertyEdit::BeginTable("Values", DetailsStyle::MakeTableConfig()))
		{
			if (bOpen)
			{
				ImGui::PushID(Definition.Id.ToString().c_str());
				PickerId = ImGui::GetID("##Picker");
				ImGui::OpenPopup("##Picker");
				ImGui::PopID();
			}
			Canvas.DrawParameterValue(*Material->FindParameterDefinition(Definition.Id), *Transactions.Get());
			MonaImGui::PropertyEdit::EndTable();
		}
		Canvas.EndParameterFrame();
		ImGui::End(); ImGui::Render();
	};
	const auto Click = [&](ImVec2 Position) {
		IO.AddMousePosEvent(Position.x, Position.y); Frame();
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true); Frame();
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false); Frame();
	};
	const auto Edit = [&](float Offset) {
		const auto* Popup = ImGui::FindWindowByName(std::format("##Popup_{:08x}", PickerId).c_str());
		ASSERT_NE(Popup, nullptr);
		const auto Position = Popup->DC.CursorStartPos;
		Click({Position.x + Offset, Position.y + Offset});
	};
	IO.AddMousePosEvent(300, 60);
	Frame(); Frame(true); Frame();
	EXPECT_EQ(Read(), Original);
	Edit(10);
	const auto First = Read();
	EXPECT_NE(First, Original);
	Edit(20);
	const auto Final = Read();
	EXPECT_NE(Final, First);
	EXPECT_FALSE(Transactions->CanUndo());
	Click({850, 550});
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Read(), Original);
	EXPECT_FALSE(Transactions->CanUndo());
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Read(), Final);
	ASSERT_TRUE(Transactions->Undo());
	Transactions->Reset();
	IO.AddMousePosEvent(300, 60);
	Frame(); Frame(true); Frame();
	Edit(10);
	EXPECT_NE(Read(), Original);
	IO.AddKeyEvent(ImGuiKey_Escape, true); Frame();
	EXPECT_EQ(Read(), Original);
	EXPECT_FALSE(Transactions->CanUndo());
	IO.AddKeyEvent(ImGuiKey_Escape, false); Frame(true); Frame();
	Edit(20);
	EXPECT_NE(Read(), Original);
	Frame(false, false);
	EXPECT_EQ(Read(), Original);
	EXPECT_FALSE(Transactions->CanUndo());
}

namespace
{
	struct FScopedGraphClipboard
	{
		~FScopedGraphClipboard() { Durin::MaterialEditor::FMaterialGraphCanvas::ClearSharedClipboard(); }
	};
}

namespace Durin::MaterialEditor
{
	struct FMaterialGraphCanvasTestAccess
	{
		static auto Install(FMaterialEditor& Editor, DMaterial& Material) -> FMaterialGraphCanvas&
		{
			auto Canvas = std::make_unique<FMaterialGraphCanvas>(*DMaterialGraph::Create(Material), FMaterialGraphEditorServices{});
			auto& Result = *Canvas;
			Editor.EditingSession->Canvas = std::move(Canvas);
			return Result;
		}
		static auto HasCanvas(const FMaterialEditor& Editor) -> bool
		{ return Editor.EditingSession && Editor.EditingSession->Canvas; }
		static auto ObserveParameter(FMaterialGraphCanvas& Canvas, const FGuid& Id, DTransactor& Transactions,
			ImGuiID Widget, MonaImGui::PropertyEdit::EPropertyInteractionEnd End, float Value) -> void
		{
			Canvas.ObserveParameterValue(Id, Transactions, {.Id = Widget, .End = End}, FMaterialParameterValue::MakeScalar(Value));
		}
		static auto BeginParameter(FMaterialGraphCanvas& Canvas, const FGuid& Id, DTransactor& Transactions) -> void
		{
			ObserveParameter(Canvas, Id, Transactions, 1, MonaImGui::PropertyEdit::EPropertyInteractionEnd::None, .75f);
			Canvas.EndParameterFrame();
			ASSERT_TRUE(Canvas.ParameterSession.IsActive());
		}
		static auto FinishParameter(FMaterialGraphCanvas& Canvas, bool bCancel) -> bool { return Canvas.FinishParameterEdit(bCancel); }
		static auto ParameterActive(const FMaterialGraphCanvas& Canvas) -> bool { return Canvas.ParameterSession.IsActive(); }
		static auto Duplicate(FMaterialGraphCanvas& Canvas, DMaterial& Material,
			DTransactor& Transactions, std::span<const FGuid> Nodes) -> void
		{
			Canvas.DuplicateNodes(Material, Transactions, Nodes);
		}
		static auto Select(FMaterialGraphCanvas& Canvas,
			std::initializer_list<FMaterialGraphNodeAddress> Nodes) -> void
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
		static auto TogglePins(FMaterialGraphCanvas& Canvas, const FGuid& NodeId) -> void
		{ Canvas.ToggleNodePins(NodeId); }
		static auto CanTogglePins(const FMaterialGraphCanvas& Canvas, const FGuid& NodeId) -> bool
		{ return Canvas.CollapsiblePinNodes.contains(NodeId); }
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
			const auto Base = SearchCatalogIndices(Canvas.Catalog, "");
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
			for (auto I : SearchCatalogIndices(Canvas.Catalog, "texture"))
				Expected.push_back(MakeCreationAction(Canvas.Catalog[I]).Id);
			return Actual == Expected;
		}
		static auto RecentAction(const FMaterialGraphCanvas& Canvas) -> std::string
		{ return Canvas.RecentCreationMenuEntries.empty() ? std::string{} : Canvas.RecentCreationMenuEntries.front(); }
		static auto FirstMenuAction(const FMaterialGraphCanvas& Canvas) -> std::string
		{ return Canvas.CachedCreationMenuResults.empty() ? std::string{} : Canvas.CachedCreationActions[Canvas.CachedCreationMenuResults.front()].Id; }
		static auto BeginLink(FMaterialGraphCanvas& Canvas, FGuid Source) -> void
		{ Canvas.Interaction = FMaterialGraphCanvas::FLinkingInteraction{Source}; }
		static auto Menu(const FMaterialGraphCanvas& Canvas) -> bool
		{ return std::holds_alternative<FMaterialGraphCanvas::FNodeCreationMenuInteraction>(Canvas.Interaction); }
		static auto OpenCreationAt(FMaterialGraphCanvas& Canvas, ImVec2 Position) -> void
		{ Canvas.Interaction = FMaterialGraphCanvas::FNodeCreationMenuInteraction{.GraphPosition = Position}; }
	};
}

TEST(FMaterialGraphInteractionTests, ReplacementCanvasRestoresRootAndSurvivingExpressionsByAddress)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
	const auto Expression = Testing::CreateGraphConstant(*Graph, .25f);
	ASSERT_TRUE(Expression);
	FMaterialGraphCanvas Original(*Graph);
	FMaterialGraphCanvasTestAccess::Prepare(Original, *Material);
	Original.RestoreSelection({EMaterialGraphEndpoint::MaterialRoot, *Expression});
	const auto Copy = DuplicateObject(Material.Get(), nullptr, NAME_None);
	ASSERT_TRUE(Copy);
	TStrongObjectPtr<DMaterialGraph> Replacement(DMaterialGraph::Create(**Copy));
	FMaterialGraphCanvas Canvas(*Replacement);
	Canvas.RestoreSelection(Original.GetSelection());
	FMaterialGraphCanvasTestAccess::Prepare(Canvas, **Copy);
	EXPECT_EQ(Canvas.GetSelection(), Original.GetSelection());
	ASSERT_TRUE(Replacement->RemoveNodes(std::array{*Expression}, ETransactionMode::NotTransactable));
	FMaterialGraphCanvasTestAccess::Prepare(Canvas, **Copy);
	ASSERT_EQ(Canvas.GetSelection().size(), 1u);
	EXPECT_TRUE(Canvas.GetSelection().contains(EMaterialGraphEndpoint::MaterialRoot));
}

TEST(FMaterialGraphInteractionTests, RemovedDragEndpointClearsGestureAndKeepsSurvivingSelection)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	const auto Source = Testing::CreateGraphConstant(*AuthoringGraph, .25f);
	ASSERT_TRUE(Source);
	FMaterialGraphCanvas Canvas(*AuthoringGraph);
	FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material);
	const auto Root = AuthoringGraph->GetRootNode()->GetView().Node.Id;
	FMaterialGraphCanvasTestAccess::Select(Canvas, {Root, Source.value()});
	FMaterialGraphCanvasTestAccess::BeginLink(Canvas, Source.value());
	ASSERT_TRUE(AuthoringGraph->RemoveNodes(std::array{Source.value()}, ETransactionMode::NotTransactable));
	FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material);
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::Idle(Canvas));
	EXPECT_EQ(FMaterialGraphCanvasTestAccess::ProgramSelection(Canvas), std::vector<FGuid>{Root});
	EXPECT_FALSE(AuthoringGraph->FindNode(Source.value()));
}

TEST(FMaterialGraphInteractionTests, ParameterHandoffHasTheSameHistoryInBothDrawOrders)
{
	using EEnd = MonaImGui::PropertyEdit::EPropertyInteractionEnd;
	InitializeDObjectSystem();
	for (bool bDrawIncomingFirst : {false, true})
	{
		SCOPED_TRACE(bDrawIncomingFirst);
		TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		FMaterialParameterDefinition Outgoing;
		Outgoing.Id = FGuid::NewGuid();
		Outgoing.Name = "Outgoing";
		Outgoing.Type = EMaterialParameterType::Scalar;
		Outgoing.Value = FMaterialParameterValue::MakeScalar(.25f);
		auto Incoming = Outgoing;
		Incoming.Id = FGuid::NewGuid();
		Incoming.Name = "Incoming";
		Incoming.Value = FMaterialParameterValue::MakeScalar(.5f);
		ASSERT_TRUE(MaterialEditing::CreateParameter(*Material, Outgoing, ETransactionMode::NotTransactable));
		ASSERT_TRUE(MaterialEditing::CreateParameter(*Material, Incoming, ETransactionMode::NotTransactable));
		const auto Read = [&](const FGuid& Id) { return Material->FindParameterDefinition(Id)->Value.GetScalar(); };
		Tests::FTestEditorTransactorOwner Transactions;
		FMaterialGraphCanvas Canvas{*DMaterialGraph::Create(*Material),
			{.ReportError = [](std::string Error) { ADD_FAILURE() << Error; }}};
		const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> Context(ImGui::CreateContext(), ImGui::DestroyContext);
		auto& IO = ImGui::GetIO();
		IO.DisplaySize = {600, 400}; IO.DeltaTime = 1.f / 60; IO.IniFilename = nullptr;
		IO.Fonts->AddFontDefault(); IO.Fonts->Build();

		ImGui::NewFrame();
		FMaterialGraphCanvasTestAccess::BeginParameter(Canvas, Outgoing.Id, *Transactions.Get());
		ImGui::EndFrame();
		EXPECT_FLOAT_EQ(Read(Outgoing.Id), .75f);
		EXPECT_FALSE(Transactions->CanUndo());

		const auto DrawOutgoing = [&] {
			FMaterialGraphCanvasTestAccess::ObserveParameter(Canvas, Outgoing.Id, *Transactions.Get(), 1, EEnd::Commit, 1.f);
		};
		const auto DrawIncoming = [&] {
			FMaterialGraphCanvasTestAccess::ObserveParameter(Canvas, Incoming.Id, *Transactions.Get(), 2, EEnd::Commit, 2.f);
		};
		ImGui::NewFrame();
		if (bDrawIncomingFirst) { DrawIncoming(); DrawOutgoing(); }
		else { DrawOutgoing(); DrawIncoming(); }
		EXPECT_FLOAT_EQ(Read(Outgoing.Id), .75f);
		EXPECT_FLOAT_EQ(Read(Incoming.Id), .5f);
		Canvas.EndParameterFrame();
		ImGui::EndFrame();

		EXPECT_FLOAT_EQ(Read(Outgoing.Id), 1.f);
		EXPECT_FLOAT_EQ(Read(Incoming.Id), 2.f);
		EXPECT_FALSE(FMaterialGraphCanvasTestAccess::ParameterActive(Canvas));
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_FLOAT_EQ(Read(Outgoing.Id), 1.f);
		EXPECT_FLOAT_EQ(Read(Incoming.Id), .5f);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_FLOAT_EQ(Read(Outgoing.Id), .25f);
		EXPECT_FLOAT_EQ(Read(Incoming.Id), .5f);
		EXPECT_FALSE(Transactions->CanUndo());
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_FLOAT_EQ(Read(Outgoing.Id), 1.f);
		EXPECT_FLOAT_EQ(Read(Incoming.Id), .5f);
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_FLOAT_EQ(Read(Incoming.Id), 2.f);
		EXPECT_FALSE(Transactions->CanRedo());
	}
}

TEST(FMaterialGraphInteractionTests, FailedParameterRestoreReportsOnceAndRetriesOnNextFrame)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialParameterDefinition Definition;
	Definition.Id = FGuid::NewGuid(); Definition.Name = "RetryParameter";
	Definition.Type = EMaterialParameterType::Scalar;
	Definition.Value = FMaterialParameterValue::MakeScalar(.25f);
	ASSERT_TRUE(MaterialEditing::CreateParameter(*Material, Definition, ETransactionMode::NotTransactable));
	Tests::FTestEditorTransactorOwner Transactions;
	int Errors = 0;
	FMaterialGraphCanvas Canvas{*DMaterialGraph::Create(*Material), {.ReportError = [&](std::string) { ++Errors; }}};
	const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> Context(ImGui::CreateContext(), ImGui::DestroyContext);
	auto& IO = ImGui::GetIO();
	IO.DisplaySize = {600, 400}; IO.DeltaTime = 1.f / 60; IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault(); IO.Fonts->Build();
	ImGui::NewFrame();
	FMaterialGraphCanvasTestAccess::BeginParameter(Canvas, Definition.Id, *Transactions.Get());
	ASSERT_TRUE(MaterialEditing::DeleteParameter(*Material, Definition.Id, ETransactionMode::NotTransactable));
	EXPECT_FALSE(Canvas.CancelInteraction());
	EXPECT_FALSE(Canvas.CancelInteraction());
	Canvas.EndParameterFrame();
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::ParameterActive(Canvas));
	EXPECT_EQ(Errors, 1);
	ImGui::EndFrame(); ImGui::NewFrame();
	Canvas.EndParameterFrame();
	EXPECT_EQ(Errors, 1);
	Definition.Value = FMaterialParameterValue::MakeScalar(.75f);
	ASSERT_TRUE(MaterialEditing::CreateParameter(*Material, Definition, ETransactionMode::NotTransactable));
	EXPECT_FALSE(FMaterialGraphCanvasTestAccess::FinishParameter(Canvas, false));
	ImGui::EndFrame(); ImGui::NewFrame();
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::FinishParameter(Canvas, false));
	EXPECT_FALSE(FMaterialGraphCanvasTestAccess::ParameterActive(Canvas));
	EXPECT_FLOAT_EQ(Material->FindParameterDefinition(Definition.Id)->Value.GetScalar(), .25f);
	EXPECT_FALSE(Transactions->CanUndo());
	ImGui::EndFrame();
}

TEST(FMaterialGraphInteractionTests, FailedCancelVetoesDocumentDeactivateDiscardAndCloseUntilRestored)
{
	using namespace Durin;
	InitializeDObjectSystem();
	const auto Root = Testing::CreateTestFixtureDirectory("GraphCancelLifecycle");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/GraphCancelLifecycle/", .Owner = EMountOwner::Test,
		.Root = Root, .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid());
	ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/GraphCancelLifecycle/Material", Path));
	DMaterial* Source = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Source));
	ASSERT_TRUE(SavePackage(Source->GetPackage()));
	FPackagePath SecondPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/GraphCancelLifecycle/Second", SecondPath));
	DMaterial* Second = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(SecondPath, Second));
	ASSERT_TRUE(SavePackage(Second->GetPackage()));
	struct FModuleScope
	{
		FMaterialEditorModule Module;
		FModuleTestHarness Harness{"MaterialEditor"};
		FModuleScope() { Harness.Start(Module); }
		~FModuleScope() { Harness.Shutdown(); }
	} ModuleScope;
	Testing::FScopedAssetEditorTestHost Host;
	ASSERT_TRUE(Host.Open(Source->GetObjectPath(), DMaterial::StaticClass()->GetQualifiedName().ToString()));
	const auto Editors = Testing::GetReflectedEditors(Host.GetAssetEditorSubsystem());
	ASSERT_EQ(Editors.size(), 1u);
	const auto Toolkit = Editors.front()->GetToolkit();
	auto& Editor = static_cast<FMaterialEditor&>(*Toolkit);
	TStrongObjectPtr<DMaterial> Preview(NewObject<DMaterial>(nullptr, NAME_None));
	Preview->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialParameterDefinition Definition;
	Definition.Id = FGuid::NewGuid(); Definition.Name = "RetryClose";
	Definition.Type = EMaterialParameterType::Scalar;
	Definition.Value = FMaterialParameterValue::MakeScalar(.25f);
	ASSERT_TRUE(MaterialEditing::CreateParameter(*Preview, Definition, ETransactionMode::NotTransactable));
	auto& Canvas = FMaterialGraphCanvasTestAccess::Install(Editor, *Preview);
	const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> Context(ImGui::CreateContext(), ImGui::DestroyContext);
	auto& IO = ImGui::GetIO();
	IO.DisplaySize = {600, 400}; IO.DeltaTime = 1.f / 60; IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault(); IO.Fonts->Build();
	ImGui::NewFrame();
	auto* Transactions = Host.GetAssetEditorSubsystem()->GetEditor()->GetTransactor();
	ASSERT_NE(Transactions, nullptr);
	FMaterialGraphCanvasTestAccess::BeginParameter(Canvas, Definition.Id, *Transactions);
	ASSERT_TRUE(MaterialEditing::DeleteParameter(*Preview, Definition.Id, ETransactionMode::NotTransactable));
	EXPECT_FALSE(Toolkit->RequestDeactivate());
	FObjectReplacementMap ReplacementMap;
	const std::array Replacements{
		FObjectReplacementMap::FEntry{.Previous = Source->GetPackage(), .Replacement = Second->GetPackage()},
		FObjectReplacementMap::FEntry{.Previous = Source, .Replacement = Second}};
	ASSERT_TRUE(ReplacementMap.BuildFromObjects(Replacements));
	auto Reload = Toolkit->CreateReloadParticipant();
	const auto Prepared = Reload->Prepare(ReplacementMap);
	EXPECT_FALSE(Prepared);
	if (!Prepared) EXPECT_EQ(Prepared.error().Code, EObjectReplacementError::Busy);
	Reload->Abort();
	EXPECT_FALSE(Toolkit->DiscardAsset(*Source));
	EXPECT_EQ(Toolkit->RequestCloseAsset(*Source), EAssetEditorCloseResult::Rejected);
	EXPECT_FALSE(Toolkit->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
	EXPECT_FALSE(Toolkit->IsClosed());
	EXPECT_EQ(Source, Toolkit->GetActiveAsset());
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::HasCanvas(Editor));
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::ParameterActive(Canvas));
	Definition.Value = FMaterialParameterValue::MakeScalar(.75f);
	ASSERT_TRUE(MaterialEditing::CreateParameter(*Preview, Definition, ETransactionMode::NotTransactable));
	ImGui::EndFrame(); ImGui::NewFrame();
	EXPECT_TRUE(Toolkit->RequestDeactivate());
	EXPECT_TRUE(Reload->Prepare(ReplacementMap));
	Reload->Abort();
	EXPECT_FLOAT_EQ(Preview->FindParameterDefinition(Definition.Id)->Value.GetScalar(), .25f);
	EXPECT_FALSE(Transactions->CanUndo());
	EXPECT_EQ(Toolkit->RequestCloseAsset(*Source), EAssetEditorCloseResult::Closed);
	EXPECT_FALSE(FMaterialGraphCanvasTestAccess::HasCanvas(Editor));
	ImGui::EndFrame();
}

TEST(FMaterialGraphInteractionTests, DuplicateMixedOutputSelectionPreservesCopiedNodePositions)
{
	InitializeDObjectSystem();
	for (const bool bCanvas : {false, true})
	{
		SCOPED_TRACE(bCanvas ? "Canvas" : "Operations");
		TStrongObjectPtr<DMaterial> Material(MakeExpandedGraphMaterial("MixedDuplicate"));
		ASSERT_NE(Material.Get(), nullptr);
		Tests::FTestEditorTransactorOwner Transactions;

		TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
		const auto First = Testing::CreateGraphConstant(*AuthoringGraph, 0.25f, 500, 300);
		const auto Second = Testing::CreateGraphConstant(*AuthoringGraph, 0.75f, 700, 200);
		ASSERT_TRUE(First);
		ASSERT_TRUE(Second);
		const FGuid OutputId = AuthoringGraph->GetRootNode()->GetView().Node.Id;
		const FMaterialGraphNodePresentation OutputPosition{OutputId, 0, 0};
		ASSERT_TRUE(AuthoringGraph->MoveNodes(std::span(&OutputPosition, 1), Durin::ETransactionMode::NotTransactable));
		const auto Before = Material->GetMaterialGraphPresentation();
		const std::array Selection{OutputId, First.value(), Second.value()};
		std::vector<FGuid> Generated;
		if (bCanvas)
		{
			FMaterialGraphCanvas Canvas{*AuthoringGraph,
				{.ReportError = [](std::string Error) { ADD_FAILURE() << Error; }}};
			FMaterialGraphCanvasTestAccess::Duplicate(Canvas, *Material, *Transactions.Get(), Selection);
			Generated = FMaterialGraphCanvasTestAccess::ProgramSelection(Canvas);
		}
		else
		{
			const auto Result = AuthoringGraph->DuplicateNodes(Selection, 40, 40).transform(Testing::NodeIds);
			ASSERT_TRUE(Result) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Result);
			Generated = Result.value();
		}
		ASSERT_EQ(Generated.size(), 2u);
		const auto View = Testing::CaptureGraphView(*AuthoringGraph);
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
		EXPECT_EQ(AuthoringGraph->GetRootNode()->GetView().Node.Id, OutputId);
		const auto After = Material->GetMaterialGraphPresentation();
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(Material->GetMaterialGraphPresentation(), Before);
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(Material->GetMaterialGraphPresentation(), After);
		Transactions->Reset();
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
	ASSERT_TRUE(DMaterialGraph::Create(*Callee)->AddPort(false, Required, {}, 0, 0, Durin::ETransactionMode::NotTransactable));
	ASSERT_TRUE(SavePackage(Callee->GetPackage()));
	auto* Function = NewObject<DMaterialFunction>(nullptr, "MenuFunctionOutput");
	const auto Original = Function->GetFunctionSignature();
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	std::vector<std::string> Errors;
	FMaterialGraphCanvas Canvas{*DMaterialGraph::Create(*Function), {.ReportError = [&](std::string Error) { Errors.push_back(std::move(Error)); }}};
	auto* Context = ImGui::CreateContext();
	auto& IO = ImGui::GetIO();
	IO.IniFilename = nullptr; IO.DisplaySize = {1200, 900}; IO.DeltaTime = 1.0f / 60.0f;
	IO.Fonts->AddFontDefault(); IO.Fonts->Build();

	const auto Frame = [&] {
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1200, 900});
		ImGui::Begin("Function creation", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		Canvas.Draw(*Transactions.Get(), 820);
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
			EXPECT_FALSE(Output->Source.IsConnected());
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
		return Call && Call->Function == Callee && std::ranges::none_of(Call->FunctionInputs, [](const auto& Binding) { return Binding.Input.IsConnected() || !Binding.InputDefault.empty(); });
	});
	EXPECT_EQ(Calls, 1);
	EXPECT_EQ(FMaterialGraphCanvasTestAccess::RecentAction(Canvas), MakeFunctionCreationAction(Callee->GetObjectPath()).Id);
	EXPECT_TRUE(Transactions->Undo());
	EXPECT_TRUE(Transactions->Redo());
	FMaterialGraphCanvasTestAccess::OpenCreationAt(Canvas, {0, 0});
	Frame(); Frame();
	EXPECT_EQ(FMaterialGraphCanvasTestAccess::FirstMenuAction(Canvas), MakeFunctionCreationAction(Callee->GetObjectPath()).Id);
	Canvas.CancelInteraction(); ImGui::DestroyContext(Context);
	Transactions->Reset(); MarkAsGarbage(Function); CollectGarbage(Durin::EObjectFlags::Standalone);
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
		0.40f, EMaterialGraphDetailLevel::Editing),
		EMaterialGraphDetailLevel::Overview);
	EXPECT_EQ(FMaterialGraphGeometry::SelectDetailLevel(
		0.45f, EMaterialGraphDetailLevel::Overview),
		EMaterialGraphDetailLevel::Overview);
	EXPECT_EQ(FMaterialGraphGeometry::SelectDetailLevel(
		0.50f, EMaterialGraphDetailLevel::Overview),
		EMaterialGraphDetailLevel::Editing);
	EXPECT_EQ(FMaterialGraphGeometry::SelectDetailLevel(
		0.70f, EMaterialGraphDetailLevel::Editing),
		EMaterialGraphDetailLevel::Editing);
}

TEST(FMaterialGraphInteractionTests, FunctionDefaultControlsPreserveInheritedComponentsAndUndo)
{
	InitializeDObjectSystem();
	struct FCase { const char* Name; EMaterialProgramValueType Type; FMaterialProgramLiteral Default; };
	const std::array Cases{
		FCase{"Scale", EMaterialProgramValueType::Float2, {1, 1}},
		FCase{"Strength", EMaterialProgramValueType::Float, {1}},
		FCase{"Normal", EMaterialProgramValueType::Float3, {0, 0, 1}},
		FCase{"Roughness", EMaterialProgramValueType::Float, {.5f}},
		FCase{"BaseColor", EMaterialProgramValueType::Float3, {.5f, .5f, .5f}}};
	for (const auto& Case : Cases)
	{
		SCOPED_TRACE(Case.Name);
		TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "DefaultControls"));
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, Case.Name));
		const FGuid PortId = FGuid::NewGuid();
		ASSERT_TRUE(DMaterialGraph::Create(*Function)->AddPort(false, {.Id = PortId, .Type = Case.Type, .Name = Case.Name,
			.Default = {.Kind = EMaterialFunctionDefaultKind::Numeric, .Numeric = Case.Default}}, {}, 0, 0, Durin::ETransactionMode::NotTransactable));
		TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
		const auto Added = AuthoringGraph->InsertFunctionCall(*Function, 0, 0, Durin::ETransactionMode::NotTransactable).transform(Testing::GraphNodeId);
		ASSERT_TRUE(Added) << FormatMaterialGraphCommandResult(Added);
		const auto CallId = Added.value();
		Durin::Tests::FTestEditorTransactorOwner Transactions;
		FMaterialGraphCanvas Canvas{*DMaterialGraph::Create(*Material), {.ReportError = [](std::string Error) { ADD_FAILURE() << Error; }}};
		FMaterialGraphCanvasTestAccess::Select(Canvas, {CallId});
		auto* Context = ImGui::CreateContext();
		auto& IO = ImGui::GetIO();
		IO.DisplaySize = {800, 600}; IO.DeltaTime = 1.f / 60; IO.IniFilename = nullptr;
		IO.Fonts->AddFontDefault(); IO.Fonts->Build();
		ImVec2 Field;
		const auto Frame = [&](bool Down = false) {
			IO.AddMousePosEvent(Field.x, Field.y); IO.AddMouseButtonEvent(ImGuiMouseButton_Left, Down);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({600, 500});
			ImGui::Begin("Function defaults", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
			ImGui::PushID(CallId.ToString().c_str());
			const auto TableId = ImGui::GetID("NodeProperties");
			ImGui::PopID();
			Canvas.DrawSelectionDetails(*Transactions.Get());
			const auto* Table = Context->Tables.GetByKey(TableId);
			if (Table) Field = {Table->Columns[1].WorkMinX + 15,
				Table->OuterRect.Min.y + DetailsStyle::MakeTableConfig().CellPadding.y + ImGui::GetFrameHeight() / 2};
			else ADD_FAILURE() << "Missing Details table";
			ImGui::End(); ImGui::Render();
		};
		Frame(); Frame();
		EXPECT_EQ((Transactions->GetQueueLength() - Transactions->GetUndoCount()), 0u);
		const auto* Call = FindExpression<DMaterialExpressionFunctionCall>(*Material, CallId);
		ASSERT_NE(Call, nullptr);
		EXPECT_TRUE(std::ranges::all_of(Call->FunctionInputs, [](const auto& Input) { return Input.InputDefault.empty(); }));
		Frame(true); Frame();
		IO.AddKeyEvent(ImGuiMod_Ctrl, true); IO.AddKeyEvent(ImGuiKey_A, true); Frame();
		IO.AddKeyEvent(ImGuiKey_A, false); IO.AddKeyEvent(ImGuiMod_Ctrl, false); Frame();
		IO.AddInputCharactersUTF8("2"); Frame();
		IO.AddKeyEvent(ImGuiKey_Enter, true); Frame();
		IO.AddKeyEvent(ImGuiKey_Enter, false); Frame();
		const auto Binding = std::ranges::find(Call->FunctionInputs, PortId, &FMaterialExpressionFunctionInputBinding::InputId);
		ASSERT_NE(Binding, Call->FunctionInputs.end());
		const std::array Expected{2.f, Case.Default.Y, Case.Default.Z, Case.Default.W};
		EXPECT_EQ(Binding->InputDefault, (std::vector<float>(Expected.begin(), Expected.begin() + static_cast<int>(Case.Type) + 1)));
		EXPECT_EQ((Transactions->GetQueueLength() - Transactions->GetUndoCount()), 1u);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_TRUE(std::ranges::all_of(Call->FunctionInputs, [](const auto& Input) { return Input.InputDefault.empty(); }));
		Frame();
		EXPECT_EQ((Transactions->GetQueueLength() - Transactions->GetUndoCount()), 0u);
		Canvas.CancelInteraction();
		ImGui::DestroyContext(Context);
	}
}

TEST(FMaterialGraphInteractionTests, HiddenAdvancedPinsRetainStableIdentitiesAndRevealBindings)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "AdvancedPins");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	auto* Function = NewObject<DMaterialFunction>(nullptr, "AdvancedFunction");

	TStrongObjectPtr<DMaterialGraph> FunctionAuthoringGraph(DMaterialGraph::Create(*Function));
	TStrongObjectPtr<DMaterialGraph> DocumentCommandGraph(DMaterialGraph::Create(*Material));
	const FGuid Advanced = FGuid::NewGuid(), Visible = FGuid::NewGuid();
	ASSERT_TRUE(FunctionAuthoringGraph->AddPort(false, {.Id = Advanced, .Name = "Advanced", .bAdvanced = true,
		.Default = {.Kind = EMaterialFunctionDefaultKind::Numeric}}, {}, 0, 0, Durin::ETransactionMode::NotTransactable));
	ASSERT_TRUE(FunctionAuthoringGraph->AddPort(false, {.Id = Visible, .Name = "Visible", .DisplayOrder = 1,
		.Default = {.Kind = EMaterialFunctionDefaultKind::Numeric}}, {}, 0, 0, Durin::ETransactionMode::NotTransactable));
	const auto Call = DocumentCommandGraph->InsertFunctionCall(*Function, 0, 0, Durin::ETransactionMode::NotTransactable).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Call);
	EXPECT_TRUE(std::ranges::find(Function->GetFunctionSignature().Inputs, Advanced, &FMaterialFunctionPort::Id)->bAdvanced);
	FMaterialGraphCanvas Canvas{*DMaterialGraph::Create(*Material)};
	const auto& Hidden = FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material);
	const auto* CallView = FindViewNode(Hidden, Call.value());
	ASSERT_NE(CallView, nullptr);
	ASSERT_EQ(CallView->Inputs.size(), 2u); // The default Surface input remains visible.
	EXPECT_EQ(CallView->Inputs.back().PortId, Visible);
	EXPECT_EQ(CallView->Inputs.back().InputIndex, 2u);
	const auto& Details = FMaterialGraphCanvasTestAccess::Details(Canvas, *Material);
	ASSERT_EQ(FindViewNode(Details, Call.value())->Inputs.size(), 3u);
	FMaterialGraphCanvasTestAccess::PrepareVisuals(Canvas);
	for (int Frame = 0; Frame < 3; ++Frame)
	{
		FMaterialGraphCanvasTestAccess::Details(Canvas, *Material);
		EXPECT_FALSE(FMaterialGraphCanvasTestAccess::TopologyStale(Canvas));
	}
	ASSERT_TRUE(GraphEditInternals::WriteInputDefault(Material, FMaterialGraphPinAddress::Input(Call.value(), 0, Advanced), {.Kind = EMaterialInputDefaultKind::Literal, .Literal = {.X = .7f}}, Durin::ETransactionMode::NotTransactable));
	const auto& Revealed = FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material);
	CallView = FindViewNode(Revealed, Call.value());
	ASSERT_EQ(CallView->Inputs.size(), 3u);
	EXPECT_NE(std::ranges::find(CallView->Inputs, Advanced, &FMaterialGraphPinView::PortId), CallView->Inputs.end());
	EXPECT_EQ(CallView->Inputs.back().PortId, Visible);
	MarkAsGarbage(Material);
	MarkAsGarbage(Function);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphInteractionTests, NodePinChevronExpandsOnlyClickedNodeWithoutEditing)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "NodePinChevron"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, "OptionalInput"));
	TStrongObjectPtr<DMaterialGraph> FunctionAuthoringGraph(DMaterialGraph::Create(*Function));
	TStrongObjectPtr<DMaterialGraph> DocumentCommandGraph(DMaterialGraph::Create(*Material));
	const FGuid Advanced = FGuid::NewGuid();
	ASSERT_TRUE(FunctionAuthoringGraph->AddPort(false, {.Id = Advanced, .Name = "Extra", .bAdvanced = true,
		.Default = {.Kind = EMaterialFunctionDefaultKind::Numeric}}, {}, 0, 0, Durin::ETransactionMode::NotTransactable));
	const auto First = DocumentCommandGraph->InsertFunctionCall(*Function, 0, 250, Durin::ETransactionMode::NotTransactable).transform(Testing::GraphNodeId);
	const auto Second = DocumentCommandGraph->InsertFunctionCall(*Function, 400, 250, Durin::ETransactionMode::NotTransactable).transform(Testing::GraphNodeId);
	ASSERT_TRUE(First); ASSERT_TRUE(Second);
	const auto FirstId = First.value(), SecondId = Second.value();
	FMaterialGraphCanvas Canvas{*DMaterialGraph::Create(*Material)};
	const auto& Initial = FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material);
	const auto CollapsedCount = FindViewNode(Initial, FirstId)->Inputs.size();
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::CanTogglePins(Canvas, FirstId));
	EXPECT_FALSE(FMaterialGraphCanvasTestAccess::CanTogglePins(Canvas, DocumentCommandGraph->GetRootNode()->GetView().Node.Id));
	const auto Presentation = Material->GetMaterialGraphPresentation();
	const auto Generation = Material->GetMaterialCompileDebugInfo().RequestId;
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	auto* Context = ImGui::CreateContext();
	auto& IO = ImGui::GetIO();
	IO.DisplaySize = {1200, 1000}; IO.DeltaTime = 1.f / 60; IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault(); IO.Fonts->Build();
	Canvas.SetViewport(1, {40, 40});
	ImVec2 Arrow;
	const auto Frame = [&](ImVec2 Mouse, bool Down) {
		IO.AddMousePosEvent(Mouse.x, Mouse.y); IO.AddMouseButtonEvent(ImGuiMouseButton_Left, Down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1200, 1000});
		ImGui::Begin("Pin Chevron", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		Canvas.Draw(*Transactions.Get(), 900);
		const auto* Child = ImGui::GetCurrentWindow()->DC.ChildWindows.back();
		const ImVec2 Origin{Child->Pos.x + Child->WindowPadding.x + 40,
			Child->Pos.y + Child->WindowPadding.y + 40 + ImGui::GetFrameHeightWithSpacing()};
		const auto* Node = FindViewNode(FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material), FirstId);
		Arrow = {Origin.x + GraphNodeWidth(*Node) * .5f, Origin.y + 250 + GraphNodeHeight(*Node) + 10};
		ImGui::End(); ImGui::Render();
	};
	Frame({1100, 950}, false); Frame({1100, 950}, false);
	Frame(Arrow, false); Frame(Arrow, true); Frame(Arrow, false);
	const auto& Expanded = FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material);
	EXPECT_EQ(FindViewNode(Expanded, FirstId)->Inputs.size(), CollapsedCount + 1);
	EXPECT_EQ(FindViewNode(Expanded, SecondId)->Inputs.size(), CollapsedCount);
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::Idle(Canvas));
	Frame(Arrow, false); Frame(Arrow, true); Frame(Arrow, false);
	EXPECT_EQ(FindViewNode(FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material), FirstId)->Inputs.size(), CollapsedCount);
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), Presentation);
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().RequestId, Generation);
	EXPECT_EQ((Transactions->GetQueueLength() - Transactions->GetUndoCount()), 0u);
	EXPECT_TRUE(Canvas.GetSelection().empty());
	Canvas.CancelInteraction();
	ImGui::DestroyContext(Context);
}

TEST(FMaterialGraphInteractionTests, TextureOutputsHideUnusedAdvancedPinsWithoutChangingLinks)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "CompactTextureOutputs");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto Added = MaterialEditing::AddTextureToSurfaceOutput(*Material, {}, Durin::ETransactionMode::NotTransactable).transform(Testing::ExpressionNodeId);
	ASSERT_TRUE(Added);
	const auto SampleId = Added.value();
	FMaterialGraphCanvas Canvas{*DMaterialGraph::Create(*Material)};
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
	FMaterialGraphCanvasTestAccess::TogglePins(Canvas, SampleId);
	Sample = FindViewNode(FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material), SampleId);
	ASSERT_EQ(Sample->Outputs.size(), 7u);
	EXPECT_EQ(Sample->Outputs[6].OutputIndex, 7u);
	EXPECT_FALSE(std::ranges::any_of(Sample->Outputs, [](const auto& Pin) { return Pin.Name == "Normal"; }));
	FMaterialGraphCanvasTestAccess::TogglePins(Canvas, SampleId);

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	const auto SecondSample = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::TextureSample2D, EMaterialProgramValueType::Float4, {SampleId, 7});
	ASSERT_TRUE(SecondSample);
	const auto RG = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::Swizzle, EMaterialProgramValueType::Float2, {SampleId, 0});
	ASSERT_TRUE(RG);
	EXPECT_EQ(FindViewNode(Testing::CaptureGraphView(*AuthoringGraph), RG->front())->PrimaryLabel, "Component Mask RG");
	EXPECT_FALSE(Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::DecodeNormalRG,
		EMaterialProgramValueType::Float3, {RG->front()}));
	const auto NormalizeCatalog = EnumerateCatalog();
	const auto NormalizeEntry = std::ranges::find_if(NormalizeCatalog, [](const auto& Entry) {
		return Entry.Opcode == EMaterialProgramOpcode::Normalize && Entry.ResultType == EMaterialProgramValueType::Float3;
	});
	ASSERT_NE(NormalizeEntry, NormalizeCatalog.end());
	const auto NormalConsumer = Testing::CreateGraphCatalogNode(*AuthoringGraph, *NormalizeEntry, 0, 0, {SampleId, 1}, Durin::ETransactionMode::NotTransactable);
	ASSERT_TRUE(NormalConsumer);
	const auto NormalView = Testing::CaptureGraphView(*AuthoringGraph);
	ASSERT_EQ(FindViewNode(NormalView, NormalConsumer->front())->Inputs.front().SourceType, EMaterialProgramValueType::Float3);
	ASSERT_TRUE(AuthoringGraph->RemoveNodes(NormalConsumer.value(), Durin::ETransactionMode::NotTransactable));
	Sample = FindViewNode(FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material), SampleId);
	ASSERT_EQ(Sample->Outputs.size(), 7u);
	EXPECT_EQ(Sample->Outputs[6].OutputIndex, 7u);
	const auto BeforeInvalid = Material->CaptureRootInputs();
	EXPECT_FALSE(GraphEditInternals::WriteExpressionConnection(Material, FMaterialGraphPinAddress::MaterialOutput(EMaterialProperty::MP_Normal), FMaterialGraphPinAddress::Output({SampleId, 8}), true, Durin::ETransactionMode::NotTransactable));
	EXPECT_EQ(Material->CaptureRootInputs(), BeforeInvalid);
	EXPECT_FALSE(Material->GetNumericInputForProperty(EMaterialProperty::MP_Normal)->Connection.IsConnected());
	const std::array Consumers{SecondSample->front(), RG->front()};
	ASSERT_TRUE(DMaterialGraph::Create(*Material)->RemoveNodes(Consumers, Durin::ETransactionMode::NotTransactable));
	Sample = FindViewNode(FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material), SampleId);
	EXPECT_EQ(Sample->Outputs.size(), 6u);
	EXPECT_EQ(Material->GetNumericInputForProperty(EMaterialProperty::MP_BaseColor)->Connection.OutputIndex, 1u);
	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphInteractionTests, CanvasFramesExplicitScopeWithMaterialOutputIdentity)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "FrameScope"));
	FMaterialGraphView View;
	FMaterialGraphNodeView Node;
	Node.Node.Id = FGuid::NewGuid();
	Node.Presentation.X = -1000;
	Node.Presentation.Y = -100;
	View.Nodes.push_back(Node);
	FMaterialGraphCanvas Canvas{*DMaterialGraph::Create(*Material)};
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
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> FirstMaterial(NewObject<DMaterial>(nullptr, "FirstDiagnostic"));
	TStrongObjectPtr<DMaterial> SecondMaterial(NewObject<DMaterial>(nullptr, "SecondDiagnostic"));
	FMaterialGraphCanvas FirstCanvas{*DMaterialGraph::Create(*FirstMaterial)};
	FMaterialGraphCanvas SecondCanvas{*DMaterialGraph::Create(*SecondMaterial)};
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
		.LocationIndex = static_cast<uint32>(EMaterialProperty::MP_Roughness),
	}));
	EXPECT_TRUE(FirstCanvas.GetSelection().contains(FirstNode));
	EXPECT_EQ(FirstCanvas.GetSelectedSurfaceOutput(),
		EMaterialProperty::MP_Roughness);
	EXPECT_TRUE(FirstCanvas.SelectAndFrame(FirstNode));
	EXPECT_FALSE(FirstCanvas.GetSelectedSurfaceOutput().has_value());
	EXPECT_FALSE(FirstCanvas.SelectAndFrameDiagnostic({
		.LocationKind = EMaterialProgramDiagnosticLocationKind::Program,
	}));
	EXPECT_FALSE(FirstCanvas.SelectAndFrameDiagnostic({
		.LocationKind = EMaterialProgramDiagnosticLocationKind::SurfaceOutput,
		.NodeId = FirstNode,
		.LocationIndex = 255,
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
	Saturate->Input = {Parameter.Get()};
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 2>{Parameter.Get(), Saturate.Get()}, {}));
	TStrongObjectPtr<DMaterialGraph> EditorGraph(DMaterialGraph::Create(*Material));
	ASSERT_TRUE(EditorGraph->Layout({}, Durin::ETransactionMode::NotTransactable));
	FMaterialGraphCanvas Canvas{*DMaterialGraph::Create(*Material)};
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
		Material->SetRootPosition(Material->GetRootPosition().first, Material->GetRootPosition().second + 3);
		ASSERT_TRUE(Material->SetMaterialGraphPresentation(Presentation) != Durin::EMaterialGraphPresentationResult::Rejected);
		FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material);
		EXPECT_EQ(View.Nodes.data(), Nodes);
		EXPECT_FALSE(FMaterialGraphCanvasTestAccess::TopologyStale(Canvas));
		EXPECT_EQ(FindViewNode(View, Presentation.Nodes.front().NodeId)->Presentation,
			Presentation.Nodes.front());
		EXPECT_EQ(std::ranges::find_if(View.Nodes, [](const auto& N) { return N.Node.bMaterialOutput; })->Presentation.Y, Material->GetRootPosition().second);
		for (size_t Index = 0; Index < Pins.size(); ++Index)
			EXPECT_EQ(View.Nodes[Index].Inputs.data(), Pins[Index]);
	}
	const auto ParameterNode = std::ranges::find_if(View.Nodes,
		[](const auto& Node) { return Node.Node.GetParameterId().IsValid(); });
	ASSERT_NE(ParameterNode, View.Nodes.end());
	ASSERT_TRUE(MaterialEditing::RenameParameter(*Material, ParameterNode->Node.GetParameterId(), FName("RenamedCanvasParameter"), Durin::ETransactionMode::NotTransactable));
	FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material);
	EXPECT_EQ(FindViewNode(View, ParameterId)->PrimaryLabel,
		Material->FindParameterDefinition(Definition.Id)->DisplayName);
	EXPECT_EQ(FindViewNode(FMaterialGraphCanvasTestAccess::Details(Canvas, *Material), ParameterId)->PrimaryLabel,
		Material->FindParameterDefinition(Definition.Id)->DisplayName);
	FMaterialGraphCanvasTestAccess::PrepareVisuals(Canvas);
	const auto AuthoredRevision = Material->GetMaterialCompileDebugInfo().AuthoredRevision;
	ASSERT_TRUE(Material->SetParameterValue(Definition.Id, FMaterialParameterValue::MakeScalar(.75f)));
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, AuthoredRevision);
	FMaterialGraphCanvasTestAccess::Details(Canvas, *Material);
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::TopologyStale(Canvas));
	FMaterialGraphCanvasTestAccess::PrepareVisuals(Canvas);
	FMaterialGraphCanvasTestAccess::Details(Canvas, *Material);
	EXPECT_FALSE(FMaterialGraphCanvasTestAccess::TopologyStale(Canvas));
	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphInteractionTests, FunctionCanvasConnectsAndMovesNodesWithUndo)
{
	FScopedGraphClipboard Clipboard;
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "InteractiveFunction");

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Function));
	const auto Constant = Testing::CreateGraphConstant(*AuthoringGraph, 0.5f, 0, 300);
	ASSERT_TRUE(Constant);
	const auto Other = Testing::CreateGraphConstant(*AuthoringGraph, 0.9f, 0, 550);
	ASSERT_TRUE(Other);
	const auto Port = AuthoringGraph->AddPort(true, {.Name = "Amount"}, {Constant.value()}, 350, 300, Durin::ETransactionMode::NotTransactable).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Port);
	ImGuiContext* Context = ImGui::CreateContext();
	auto& IO = ImGui::GetIO();
	IO.DisplaySize = {1200, 1000}; IO.DeltaTime = 1.0f / 60.0f; IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault(); IO.Fonts->Build();
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	int Errors = 0;
	FMaterialGraphCanvas Canvas{*DMaterialGraph::Create(*Function), {.ReportError = [&](std::string) { ++Errors; }}};
	Canvas.SetViewport(1, {40, 40});
	ImVec2 Origin;

	const auto Frame = [&](ImVec2 Mouse, bool Down) {
		IO.AddMousePosEvent(Mouse.x, Mouse.y); IO.AddMouseButtonEvent(ImGuiMouseButton_Left, Down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1200, 1000});
		ImGui::Begin("Function Canvas", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		Canvas.Draw(*Transactions.Get(), 900);
		const auto* Child = ImGui::GetCurrentWindow()->DC.ChildWindows.back();
		Origin = {Child->Pos.x + Child->WindowPadding.x + 40, Child->Pos.y + Child->WindowPadding.y + 40 + ImGui::GetFrameHeightWithSpacing()};
		ImGui::End(); ImGui::Render();
	};
	Frame({1100, 950}, false); Frame({1100, 950}, false);
	const auto& Metrics = FMaterialGraphGeometry::GetMetrics();
	const float PinY = Metrics.HeaderHeight + Metrics.SecondaryHeight + Metrics.BodyPadding;
	const auto SourceView = *FindViewNode(Testing::CaptureGraphView(*AuthoringGraph), Other.value());
	const ImVec2 Source{Origin.x + GraphNodeWidth(SourceView), Origin.y + 550 + GraphNodePinOffset(SourceView)};
	const ImVec2 Destination{Origin.x + 350, Origin.y + 300 + PinY};
	IO.AddKeyEvent(ImGuiMod_Shift, true);
	Frame(Source, false); Frame(Source, true); Frame(Destination, true); Frame(Destination, false);
	EXPECT_EQ(Errors, 0);
	const auto* Output = FindExpression<DMaterialExpressionFunctionOutput>(*Function, Port.value());
	ASSERT_NE(Output, nullptr);
	EXPECT_EQ(Output->Source.GetExpressionId(), Other.value());
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
	const auto CreatedView = Testing::CaptureGraphView(*AuthoringGraph);
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
		const auto PastedView = Testing::CaptureGraphView(*AuthoringGraph);
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
	Transactions->Reset(); MarkAsGarbage(Function); CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphInteractionTests, CanvasConnectsASecondFunctionOutputAndRefreshesItsInterface)
{
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "CanvasMultiOutputFunction");
	auto* Material = NewObject<DMaterial>(nullptr, "CanvasMultiOutputCaller");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	TStrongObjectPtr<DMaterialGraph> FunctionAuthoringGraph(DMaterialGraph::Create(*Function));
	TStrongObjectPtr<DMaterialGraph> DocumentCommandGraph(DMaterialGraph::Create(*Material));
	FMaterialFunctionPort Input{.Id = FGuid::NewGuid(), .Name = "Amount",
		.Default = {.Kind = EMaterialFunctionDefaultKind::Numeric, .Numeric = {.X = 0.4f}}};
	const auto AddedInput = FunctionAuthoringGraph->AddPort(false, Input, {}, 0, 0, Durin::ETransactionMode::NotTransactable).transform(Testing::GraphNodeId);
	ASSERT_TRUE(AddedInput);
	FMaterialFunctionPort Output{.Id = FGuid::NewGuid(), .Name = "Amount Output", .DisplayOrder = 1};
	ASSERT_TRUE(FunctionAuthoringGraph->AddPort(true, Output, {AddedInput.value()}, 0, 0, Durin::ETransactionMode::NotTransactable));
	const auto Call = DocumentCommandGraph->InsertFunctionCall(*Function, 0, 0, Durin::ETransactionMode::NotTransactable).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Call);
	const auto Constant = Testing::CreateGraphConstant(*DocumentCommandGraph, 0.f, 0, 300);
	ASSERT_TRUE(Constant);
	auto SurfaceExpression = Testing::MakeGraphExpression<DMaterialExpressionSetMaterialAttributes>();
	SurfaceExpression->Surface = {FindMaterialExpression(Material->GetExpressionCollection().Expressions, Call.value()), 0, Function->GetFunctionSignature().Outputs[0].Id};
	SurfaceExpression->Attributes = {{EMaterialProperty::MP_Metallic, {FindMaterialExpression(Material->GetExpressionCollection().Expressions, Constant.value())}}};
	const auto Surface = DocumentCommandGraph->CreateExpression(*SurfaceExpression.Get(), 0, 450, Durin::ETransactionMode::NotTransactable).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Surface);
	const uint32 MetallicInput = static_cast<uint32>(EMaterialProperty::MP_Metallic) + 1;
	const FMaterialProgramLink AmountSource{Call.value(), 0, Output.Id};
	EXPECT_FALSE(GraphEditInternals::WriteExpressionConnection(Material, FMaterialGraphPinAddress::Input(Surface.value(), MetallicInput), FMaterialGraphPinAddress::Output(AmountSource), false, Durin::ETransactionMode::NotTransactable));
	ASSERT_TRUE(GraphEditInternals::WriteExpressionConnection(Material, FMaterialGraphPinAddress::Input(Surface.value(), MetallicInput), FMaterialGraphPinAddress::Output(AmountSource), true, Durin::ETransactionMode::NotTransactable));
	const auto SurfaceInspection = Testing::CaptureGraphView(*DocumentCommandGraph);
	const auto* SurfaceView = FindViewNode(SurfaceInspection, Surface.value());
	ASSERT_NE(SurfaceView, nullptr);
	ASSERT_EQ(SurfaceView->Inputs.size(), 2u);
	EXPECT_EQ(SurfaceView->Inputs[0].AcceptedTypes, std::vector{EMaterialProgramValueType::MaterialAttributes});
	EXPECT_EQ(SurfaceView->Inputs[1].Link, AmountSource);
	const auto Destination = Testing::CreateGraphCatalogNode(*DocumentCommandGraph, EMaterialProgramOpcode::Saturate, EMaterialProgramValueType::Float, {Constant.value()}, 350);
	ASSERT_TRUE(Destination);
	auto Presentation = Material->GetMaterialGraphPresentation();
	Material->SetRootPosition(700, 0);
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(Presentation) != Durin::EMaterialGraphPresentationResult::Rejected);
	ImGuiContext* Context = ImGui::CreateContext();
	auto& IO = ImGui::GetIO();
	IO.DisplaySize = {1200, 720}; IO.DeltaTime = 1.0f / 60.0f; IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault(); IO.Fonts->Build();
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	int Errors = 0;
	auto* Graph = DMaterialGraph::Create(*Material);
	FMaterialGraphCanvas Canvas{*Graph, {.ReportError = [&](std::string) { ++Errors; }}};
	Canvas.SetViewport(1.0f, {40, 40});
	ImVec2 Origin;

	const auto Frame = [&](ImVec2 Mouse, bool Down) {
		IO.AddMousePosEvent(Mouse.x, Mouse.y); IO.AddMouseButtonEvent(ImGuiMouseButton_Left, Down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1200, 720});
		ImGui::Begin("Function Links", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		Canvas.Draw(*Transactions.Get(), 660);
		const auto* Window = ImGui::GetCurrentWindow()->DC.ChildWindows.back();
		Origin = {Window->Pos.x + Window->WindowPadding.x + 40,
			Window->Pos.y + Window->WindowPadding.y + ImGui::GetFrameHeightWithSpacing() + 40};
		ImGui::End(); ImGui::Render();
	};
	Frame({1100, 600}, false); Frame({1100, 600}, false);
	const auto& Metrics = FMaterialGraphGeometry::GetMetrics();
	const float PinY = Metrics.HeaderHeight + Metrics.SecondaryHeight + Metrics.BodyPadding;
	const ImVec2 SecondOutput{Origin.x + Metrics.NodeWidth, Origin.y + PinY + Metrics.PinRowHeight};
	const ImVec2 Target{Origin.x + 350, Origin.y + GraphNodePinOffset(*FindViewNode(Testing::CaptureGraphView(*DocumentCommandGraph), Destination.value()[0]))};
	IO.AddKeyEvent(ImGuiMod_Shift, true);
	Frame(SecondOutput, false); Frame(SecondOutput, true);
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::Linking(Canvas));
	Frame(Target, true); Frame(Target, false);
	EXPECT_EQ(Errors, 0);
	const auto Link = DocumentCommandGraph->FindNode(Destination.value()[0])->GetView().Inputs[0].Link;
	EXPECT_EQ(Link.SourceNodeId, Call.value());
	EXPECT_EQ(Link.SourceOutputId, Output.Id);
	auto* OutputNode = Testing::FindFunctionTerminal<DMaterialExpressionFunctionOutput>(*FunctionAuthoringGraph, Output.Id);
	ASSERT_TRUE(OutputNode);
	ASSERT_TRUE(OutputNode->OnRenameNode("Renamed Amount", Durin::ETransactionMode::NotTransactable));
	const auto& View = FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material);
	const auto* CallView = FindViewNode(View, Call.value());
	ASSERT_NE(CallView, nullptr);
	ASSERT_EQ(CallView->Outputs.size(), 2u);
	EXPECT_EQ(CallView->Outputs[1].Name, "Renamed Amount");
	const auto Pin = std::ranges::find(CallView->Inputs, Input.Id, &FMaterialGraphPinView::PortId);
	ASSERT_NE(Pin, CallView->Inputs.end());
	EXPECT_FLOAT_EQ(Pin->Default.Numeric.X, 0.4f);
	FMaterialGraphCanvasTestAccess::Select(Canvas, {Destination->front()});
	ImGui::NewFrame();
	ImGui::SetNextWindowPos({0, 0});
	ImGui::SetNextWindowSize({600, 500});
	ImGui::Begin("Connected input details");
	ImGui::LogToBuffer();
	Canvas.DrawSelectionDetails(*Transactions.Get());
	const std::string DetailsText = Context->LogBuffer.c_str();
	ImGui::LogFinish();
	ImGui::End();
	ImGui::Render();
	EXPECT_NE(DetailsText.find("CanvasMultiOutputFunction.Renamed Amount"), std::string::npos);
	const auto HistoryLength = Transactions->GetQueueLength();
	const auto Break = Graph->GetSchema().BreakPinLinks(*Graph, FMaterialGraphPinAddress::Output(AmountSource));
	ASSERT_TRUE(Break);
	EXPECT_EQ(Transactions->GetQueueLength(), HistoryLength + 1);
	const auto AssertLinks = [&](FMaterialProgramLink Expected) {
		const auto& Pins = FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material);
		EXPECT_EQ(FindViewNode(Pins, Destination.value()[0])->Inputs[0].Link, Expected);
		EXPECT_EQ(FindViewNode(Pins, Surface.value())->Inputs[1].Link, Expected);
	};
	AssertLinks({});
	ASSERT_TRUE(Transactions->Undo());
	AssertLinks(AmountSource);
	ASSERT_TRUE(Transactions->Redo());
	AssertLinks({});
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(FunctionAuthoringGraph->RemovePort(true, Output.Id, Durin::ETransactionMode::NotTransactable));
	AssertLinks({});
	EXPECT_EQ(Graph->FindOutputPin(AmountSource), nullptr);
	ImGui::DestroyContext(Context);
	MarkAsGarbage(Material); MarkAsGarbage(Function); CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphInteractionTests, ParameterCanvasDragMovesNodeWithoutEditingItsValue)
{
	InitializeDObjectSystem();
	for (const auto Type : {EMaterialProgramValueType::Float, EMaterialProgramValueType::Float4})
	{
		auto* Material = NewObject<DMaterial>(nullptr, NAME_None);
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

		TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
		const auto Created = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::Parameter, Type);
		ASSERT_TRUE(Created);
		const auto Other = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::Parameter,
			EMaterialProgramValueType::Float, {}, 350);
		ASSERT_TRUE(Other);
		const auto Id = Created->front();
		const auto ParameterId = AuthoringGraph->FindNode(Id)->GetView().Node.GetParameterId();
		FResolvedMaterialParameter Initial;
		ASSERT_TRUE(Material->ResolveParameterValue(ParameterId, Initial));
		const auto Presentation = Material->GetMaterialGraphPresentation();
		ImGuiContext* Context = ImGui::CreateContext();
		auto& IO = ImGui::GetIO();
		IO.DisplaySize = {1200, 720}; IO.DeltaTime = 1.0f / 60.0f; IO.IniFilename = nullptr;
		IO.Fonts->AddFontDefault(); IO.Fonts->Build();
		Durin::Tests::FTestEditorTransactorOwner Transactions;
		int Errors = 0;
		FMaterialGraphCanvas Canvas{*DMaterialGraph::Create(*Material), {.ReportError = [&](std::string) { ++Errors; }}};
		Canvas.SetViewport(1.0f, {40, 40});
		ImVec2 Origin;

		const auto Frame = [&](ImVec2 Mouse, bool Down) {
			IO.AddMousePosEvent(Mouse.x, Mouse.y); IO.AddMouseButtonEvent(ImGuiMouseButton_Left, Down);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1200, 720});
			ImGui::Begin("Parameter Drag", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
			Canvas.Draw(*Transactions.Get(), 660);
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
		MarkAsGarbage(Material); CollectGarbage(Durin::EObjectFlags::Standalone);
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
	Consumer->Input = {Previous.Get()};
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 3>{Previous.Get(), Current.Get(), Consumer.Get()}, {}));
	auto Presentation = Material->GetMaterialGraphPresentation();
	Presentation.Nodes = {{Source, 0, 0}, {Destination, 350, 0}, {PreviousSource, 0, 300}};
	Material->SetRootPosition(700, 0);
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(Presentation) != Durin::EMaterialGraphPresentationResult::Rejected);
	ImGuiContext* Context = ImGui::CreateContext();
	auto& IO = ImGui::GetIO();
	IO.DisplaySize = {1200, 720};
	IO.DeltaTime = 1.0f / 60.0f;
	IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault();
	IO.Fonts->Build();
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	int Errors = 0;
	FMaterialGraphCanvas Canvas{*DMaterialGraph::Create(*Material), {.ReportError = [&](std::string) { ++Errors; }}};
	Canvas.SetViewport(1.0f, {40, 40});
	ImVec2 Origin;

	const auto Frame = [&](ImVec2 Mouse, bool Down) {
		IO.AddMousePosEvent(Mouse.x, Mouse.y);
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, Down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0});
		ImGui::SetNextWindowSize({1200, 720});
		ImGui::Begin("Link Release", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		Canvas.Draw(*Transactions.Get(), 660);
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
	TStrongObjectPtr<DMaterialGraph> InspectionGraph(DMaterialGraph::Create(*Material));
	const auto View = Testing::CaptureGraphView(*InspectionGraph);
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
	EXPECT_EQ(FindViewNode(Testing::CaptureGraphView(*InspectionGraph), Destination)
		->Inputs.front().Link.SourceNodeId, Source);
	Drop({Origin.x + 700, Origin.y + (GraphNodePinOffset(*FindViewNode(View, InspectionGraph->GetRootNode()->GetView().Node.Id)) + Metrics.PinRowHeight * 3)}, false);
	EXPECT_EQ(Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->Connection.GetExpressionId(), Source);
	Drop({Origin.x + 700, Origin.y + (GraphNodePinOffset(*FindViewNode(View, InspectionGraph->GetRootNode()->GetView().Node.Id)))}, false);
	EXPECT_EQ(Errors, 1);
	EXPECT_EQ(Material->GetNumericInputForProperty(EMaterialProperty::MP_BaseColor)->Connection.GetExpressionId(), Source);
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
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphInteractionTests, NumericNodeDisplayTracksValuesAndUndo)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "NumericNodeDisplay");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	const auto Constant = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float, {}, 0, 0, Durin::ETransactionMode::Record);
	ASSERT_TRUE(Constant);
	const auto Id = Constant->front();
	ASSERT_TRUE(AuthoringGraph->FindNode(Id)->SetConstantValue( FMaterialParameterValue::MakeScalar(0.5f), Durin::ETransactionMode::Record));
	FMaterialGraphCanvas Canvas{*AuthoringGraph};
	auto Display = MakeGraphNodeDisplay(*FindViewNode(FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material), Id), Material);
	EXPECT_EQ(Display.Title, "0.5");
	EXPECT_EQ(Display.Subtitle, "Constant (Float)");
	ASSERT_TRUE(AuthoringGraph->FindNode(Id)->SetConstantValue( FMaterialParameterValue::MakeVector({1, 0.5f, -2}), Durin::ETransactionMode::Record));
	Display = MakeGraphNodeDisplay(*FindViewNode(FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material), Id), Material);
	EXPECT_EQ(Display.Title, "(1, 0.5, -2)");
	EXPECT_EQ(Display.Subtitle, "Constant (Float3)");
	ASSERT_TRUE(Display.Value);
	EXPECT_FLOAT_EQ(Display.Value->Z, -2); // Display swatches must not alter numeric values.
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(MakeGraphNodeDisplay(*FindViewNode(FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material), Id), Material).Title, "0.5");
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(MakeGraphNodeDisplay(*FindViewNode(FMaterialGraphCanvasTestAccess::Prepare(Canvas, *Material), Id), Material).Title, "(1, 0.5, -2)");
	const auto Parameter = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float4, {}, 300, 0, Durin::ETransactionMode::Record);
	ASSERT_TRUE(Parameter);
	const auto View = Testing::CaptureGraphView(*AuthoringGraph);
	const auto* Node = FindViewNode(View, Parameter->front());
	ASSERT_NE(Node, nullptr);
	ASSERT_TRUE(MaterialEditing::SetParameterValue(*Material, Node->Node.GetParameterId(), FMaterialParameterValue::MakeVector4({0.25f, 0.5f, 2, 1})));
	Display = MakeGraphNodeDisplay(*Node, Material); // Resolve current values even with a cached view.
	EXPECT_EQ(Display.Title, Node->PrimaryLabel);
	EXPECT_EQ(Display.Subtitle, "(0.25, 0.5, 2, 1)");
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_NE(MakeGraphNodeDisplay(*Node, Material).Subtitle, Display.Subtitle);
	EXPECT_EQ(FormatGraphNumericValue(EMaterialProgramValueType::Float2, {-0.0f, 0.123456f}), "(0, 0.1235)");
	EXPECT_EQ(FormatGraphNumericValue(EMaterialProgramValueType::Float, {0.000001f}), "1e-06");
	Transactions->Reset();
	MarkAsGarbage(Material); CollectGarbage(Durin::EObjectFlags::Standalone);
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
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	int Errors = 0;
	FMaterialGraphCanvas Canvas{*DMaterialGraph::Create(*Material), {.ReportError = [&](std::string) { ++Errors; }}};
	Canvas.SetViewport(0.75f, {40, 40});
	ImVec2 Origin;

	const auto Frame = [&](ImVec2 Mouse, bool Down, bool TextInput = false) {
		IO.AddMousePosEvent(Mouse.x, Mouse.y);
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, Down);
		ImGui::NewFrame();
		IO.WantTextInput = TextInput;
		ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1200, 720});
		ImGui::Begin("Creation Shortcuts", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		Canvas.Draw(*Transactions.Get(), 660);
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
		TStrongObjectPtr<DMaterialGraph> InspectionGraph(DMaterialGraph::Create(*Material));
		const auto View = Testing::CaptureGraphView(*InspectionGraph);
		ASSERT_EQ(View.Nodes.size(), 2u);
		const auto& CreatedNode = *std::ranges::find_if(View.Nodes, [](const auto& N) { return !N.Node.bMaterialOutput; });
		EXPECT_EQ(CreatedNode.Node.Opcode, Case.Opcode);
		EXPECT_EQ(CreatedNode.Node.ResultType, Case.Type);
		EXPECT_EQ(CreatedNode.Presentation.X, static_cast<int32>(std::round((Point.x - Origin.x) / 0.75f)));
		EXPECT_EQ(CreatedNode.Presentation.Y, static_cast<int32>(std::round((Point.y - Origin.y) / 0.75f)));
		EXPECT_TRUE(Canvas.GetSelection().contains(CreatedNode.Node.Id));
		EXPECT_TRUE(FMaterialGraphCanvasTestAccess::Idle(Canvas));
		Frame(Point, true); // Holding the mouse must not repeat creation.
		EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
		IO.AddKeyEvent(Case.Key, false);
		Frame(Point, false);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 0u);
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
		ASSERT_TRUE(Transactions->Undo());
		Frame(Point, false);
	}
	IO.AddKeyEvent(ImGuiKey_M, true);
	for (const auto Modifier : {ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt, ImGuiMod_Super})
	{
		IO.AddKeyEvent(Modifier, true);
		Frame(Point, false); Frame(Point, true); Frame(Point, false);
		EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 0u);
		IO.AddKeyEvent(Modifier, false);
		Frame(Point, false);
	}
	Frame(Point, true, true); Frame(Point, false, true);
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 0u);
	EXPECT_EQ(Errors, 0);
	Canvas.CancelInteraction();
	Transactions->Reset();
	ImGui::DestroyContext(Context);
	MarkAsGarbage(Material); CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphInteractionTests, CanvasMenusDoNotInterruptMoveAndKeepSearchRanking)
{
	InitializeDObjectSystem();
	auto* Material = NewObject<DMaterial>(nullptr, "MoveMenuRegression");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	ASSERT_TRUE(Testing::CreateGraphConstant(*AuthoringGraph, 0.5f, 0, 200));
	const auto Before = Material->GetMaterialGraphPresentation();
	ImGuiContext* Context = ImGui::CreateContext();
	auto& IO = ImGui::GetIO();
	IO.DisplaySize = {1200, 900}; IO.DeltaTime = 1.0f / 60.0f; IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault(); IO.Fonts->Build();
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	int Errors = 0;
	FMaterialGraphCanvas Canvas{*DMaterialGraph::Create(*Material), {.ReportError = [&](std::string) { ++Errors; }}};
	ImVec2 Origin;

	const auto Frame = [&](ImVec2 Mouse, bool Down) {
		IO.AddMousePosEvent(Mouse.x, Mouse.y); IO.AddMouseButtonEvent(ImGuiMouseButton_Left, Down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1200, 900});
		ImGui::Begin("Move Menu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		Canvas.Draw(*Transactions.Get(), 820);
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
	Transactions->Reset(); MarkAsGarbage(Material); CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphInteractionTests, FunctionCanvasCacheTracksPositionsAndDependencies)
{
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "CachedFunction");
	auto* Dependency = NewObject<DMaterialFunction>(nullptr, "CachedDependency");
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Function));
	TStrongObjectPtr<DMaterialGraph> DependencyDocumentAuthoringGraph(DMaterialGraph::Create(*Dependency));
	const auto Call = AuthoringGraph->InsertFunctionCall(*Dependency, 100, 200, Durin::ETransactionMode::NotTransactable).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Call);
	FMaterialGraphCanvas Canvas{*DMaterialGraph::Create(*Function)};
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
	const auto Value = Testing::CreateGraphConstant(*DependencyDocumentAuthoringGraph, 0.5f);
	ASSERT_TRUE(Value);
	ASSERT_TRUE(DependencyDocumentAuthoringGraph->AddPort(true, {.Name = "Extra"}, {Value.value()}, 0, 0, Durin::ETransactionMode::NotTransactable));
	FMaterialGraphCanvasTestAccess::PrepareFunction(Canvas, *Function);
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::TopologyStale(Canvas));
	const auto& Details = FMaterialGraphCanvasTestAccess::Details(Canvas, *Function);
	EXPECT_EQ(FindViewNode(Details, Call.value())->Outputs.size(), Dependency->GetFunctionSignature().Outputs.size());
	auto* Leaf = NewObject<DMaterialFunction>(nullptr, "CachedLeafDependency");
	ASSERT_TRUE(DependencyDocumentAuthoringGraph->InsertFunctionCall(*Leaf, 0, 0, Durin::ETransactionMode::NotTransactable));
	FMaterialGraphCanvasTestAccess::Details(Canvas, *Function);
	FMaterialGraphCanvasTestAccess::PrepareVisuals(Canvas);
	ASSERT_TRUE(Testing::CreateGraphConstant(*DMaterialGraph::Create(*Leaf), .8f));
	FMaterialGraphCanvasTestAccess::Details(Canvas, *Function);
	// A transitive implementation edit does not change this document's visible pins.
	EXPECT_FALSE(FMaterialGraphCanvasTestAccess::TopologyStale(Canvas));
	MarkAsGarbage(Leaf);
	MarkAsGarbage(Function); MarkAsGarbage(Dependency); CollectGarbage(Durin::EObjectFlags::Standalone);
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
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	FMaterialGraphCanvas Canvas{*DMaterialGraph::Create(*Material), {.ReportError = [](std::string Message) { FAIL() << Message; }}};

	const auto DrawAtZoom = [&](float Zoom) {
		Canvas.SetViewport(Zoom, {40.0f, 40.0f});
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0.0f, 0.0f});
		ImGui::SetNextWindowSize({1200.0f, 720.0f});
		ImGui::Begin("Material Graph Render Test", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		Canvas.Draw(*Transactions.Get(), 660.0f);
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
	}
	for (uint32 Index = 0; Index < DenseSources.size(); ++Index)
	{
		auto Consumer = Testing::MakeGraphExpression<DMaterialExpressionSaturate>(FGuid(400 + Index, 0, 0, 1));
		Consumer->Input = {Graph.Expressions[DenseSources.size() - Index - 1].Get()};
		Graph.Expressions.emplace_back(Consumer.Get());
	}
	auto Texture = Testing::MakeGraphExpression<DMaterialExpressionTextureSampleParameter2D>();
	Texture->Metadata = {FGuid::NewGuid(), "PreviewTexture"};
	Graph.Expressions.emplace_back(Texture.Get());
	Graph.Presentation.Nodes = {{DenseSources[0], 0, 0, "Ambient Occlusion Texture With A Deliberately Long Authored Name"}};
	ASSERT_TRUE(Graph.Apply(*Material));
	TStrongObjectPtr<DMaterialGraph> EditorGraph(DMaterialGraph::Create(*Material));
	ASSERT_TRUE(EditorGraph->Layout({}, Durin::ETransactionMode::NotTransactable));
	DrawAtZoom(0.55f);
	DrawAtZoom(1.0f);

	uint32 MaximumIndex = 1000;
	while (Graph.Expressions.size() < MaterialProgramMaxNodeCount - 1)
		Graph.Expressions.emplace_back(Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>(FGuid(MaximumIndex++, 0, 0, 1)).Get());
	ASSERT_TRUE(Graph.Apply(*Material));
	ASSERT_TRUE(EditorGraph->Layout({}, Durin::ETransactionMode::NotTransactable));
	DrawAtZoom(0.30f);

	Transactions->Reset();
	ImGui::DestroyContext(Context);
	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
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
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	constexpr std::array<uint8, 11> Channels{1, 1, 4, 3, 2, 1, 5, 2, 2, 2, 2};
	for (uint32 Index = 0; Index < Channels.size(); ++Index)
	{
		SCOPED_TRACE(Index);
		constexpr std::array Properties{EMaterialProperty::MP_BaseColor, EMaterialProperty::MP_Normal,
			EMaterialProperty::MP_Metallic, EMaterialProperty::MP_Roughness, EMaterialProperty::MP_AmbientOcclusion,
			EMaterialProperty::MP_EmissiveColor, EMaterialProperty::MP_Opacity, EMaterialProperty::MP_OpacityMask,
			EMaterialProperty::MP_Specular, EMaterialProperty::MP_ClearCoat, EMaterialProperty::MP_ClearCoatRoughness};
		const auto Role = Properties[Index];
		const bool bNormal = Role == EMaterialProperty::MP_Normal;
		const auto Result = MaterialEditing::AddTextureToSurfaceOutput(*Material, {.Output = Role, .X = 400, .Y = 200});
		ASSERT_TRUE(Result) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Result);
		ASSERT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
		ASSERT_EQ(Material->GetParameterDefinitions().size(), 1u);
		const auto* Sample = Cast<DMaterialExpressionTextureSampleParameter2D>(Material->GetExpressionCollection().Expressions.front().Get());
		ASSERT_NE(Sample, nullptr);
		EXPECT_FALSE(Sample->UV.Connection.IsConnected());
		const FGuid SampleId = Sample->Id;
		const FGuid ParameterId = Sample->Metadata.Id;
		const auto ReadOutput = [&]() {
			const auto& O = Material->CaptureRootInputs();
			return O.Get(Role);
		};
		EXPECT_EQ(ReadOutput().Connection.GetExpressionId(), SampleId);
		EXPECT_EQ(ReadOutput().Connection.OutputIndex, Channels[Index]);
		if (bNormal)
		{
			EXPECT_EQ(Sample->TextureUsage, ETextureUsage::Normal);
			EXPECT_EQ(Sample->DefaultValue.TextureFallback, EMaterialTextureFallback::FlatRGNormal);
		}
		ASSERT_TRUE(Normalize(*Material));
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 0u);
		EXPECT_TRUE(Material->GetParameterDefinitions().empty());
		EXPECT_FALSE(ReadOutput().Connection.IsConnected());
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(Material->GetExpressionCollection().Expressions.front()->Id, SampleId);
		EXPECT_EQ(Material->GetParameterDefinitions().front().Id, ParameterId);
		ASSERT_TRUE(Transactions->Undo());
		Transactions->Reset();
	}
	ASSERT_TRUE(MaterialEditing::AddTextureToSurfaceOutput(*Material, {.Output = EMaterialProperty::MP_BaseColor, .X = 0, .Y = 0}, Durin::ETransactionMode::NotTransactable));
	ASSERT_TRUE(MaterialEditing::AddTextureToSurfaceOutput(*Material, {.Output = EMaterialProperty::MP_Normal, .X = 320, .Y = 360}, Durin::ETransactionMode::NotTransactable));
	auto Presentation = Material->GetMaterialGraphPresentation();
	Material->SetRootPosition(760, 100);
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(std::move(Presentation)) != Durin::EMaterialGraphPresentationResult::Rejected);
	ImGuiContext* Context = ImGui::CreateContext();
	auto& IO = ImGui::GetIO();
	IO.DisplaySize = {1200, 850}; IO.DeltaTime = 1.f / 60; IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault(); IO.Fonts->Build();
	{
		int Errors = 0;
		FMaterialGraphCanvas Canvas{*DMaterialGraph::Create(*Material), {.ReportError = [&](std::string) { ++Errors; }}};
		Canvas.SetViewport(.85f, {30, 50});

		for (int Frame = 0; Frame < 2; ++Frame)
		{
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(IO.DisplaySize);
			ImGui::Begin("Compact Texture Authoring", nullptr, ImGuiWindowFlags_NoResize);
			Canvas.Draw(*Transactions.Get(), 780);
			ImGui::End(); ImGui::Render();
		}
		EXPECT_EQ(Errors, 0);
	}
	ImGui::DestroyContext(Context);
	Transactions->Reset();
	MarkAsGarbage(Material);
	CollectGarbage(Durin::EObjectFlags::Standalone);
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
	constexpr std::string_view Wide = "/Test/Wide";
	constexpr std::string_view Narrow = "/Test/Narrow";
	const auto WideType = Workspace::MakeDockType(Wide);
	const auto NarrowType = Workspace::MakeDockType(Narrow);
	const ImGuiID WideId = TabLayout::MakeDockSpaceId(WideType, Workspace::LayoutVersion);
	const ImGuiID NarrowId = TabLayout::MakeDockSpaceId(NarrowType, Workspace::LayoutVersion);
	EXPECT_NE(WideId, NarrowId);
	EXPECT_NE(TabLayout::MakeDockClassId(WideType), TabLayout::MakeDockClassId(NarrowType));
	ImGuiID GraphId = 0;
	ImGuiID PreviewId = 0;
	ImGuiID DetailsId = 0;
	for (int Frame = 0; Frame < 5; ++Frame)
	{
		ImGui::NewFrame();
		for (const auto* Document : {&Wide, &Narrow})
		{
			const auto DockType = Workspace::MakeDockType(*Document);
			const bool bWide = Document == &Wide;
			const ImVec2 Size = bWide ? ImVec2(1400.0f, 800.0f) : ImVec2(600.0f, 500.0f);
			ImGui::SetNextWindowSize(Size);
			ImGui::Begin(bWide ? "WideHost" : "NarrowHost");
			if (Frame == 0) Workspace::BuildDefaultLayout(DockType, Size);
			TabLayout::SubmitDockSpace(DockType, Workspace::LayoutVersion, Size,
				Frame == 2 ? ImGuiDockNodeFlags_KeepAliveOnly : ImGuiDockNodeFlags_None);
			if (Frame != 2)
			{
				for (const char* Key : {"Graph", "Preview", "Details", "Parameters", "Diagnostics"})
				{
					if (Frame == 0 && std::string_view(Key) == "Diagnostics") continue;
					TabLayout::BeginDockablePanel(DockType, Key, Key);
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
	B->UV = {UV.Get()};
	const auto Id = A->Metadata.Id, BId = B->Id;
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 3>{A.Get(), B.Get(), UV.Get()}, {}));
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	auto Definition = A->GetParameterDefinition();
	Definition.Value.GetTexture().TextureFallback = EMaterialTextureFallback::FlatRGNormal;
	ASSERT_TRUE(MaterialEditing::SetParameterValue(*Material, Id, Definition.Value));
	EXPECT_EQ(FindExpression<DMaterialExpressionTextureSampleParameter2D>(*Material, BId)->UV.Connection.GetExpressionId(), UV->Id);
	EXPECT_EQ(FindExpression<DMaterialExpressionTextureSampleParameter2D>(*Material, BId)->DefaultValue.TextureFallback, EMaterialTextureFallback::FlatRGNormal);
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	Transactions->Reset();
	ASSERT_TRUE(UnloadPackage(Path));
	Material = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedValueError;
		auto LoadedValue = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &LoadedValueError);
		Material = LoadedValue;
		ASSERT_TRUE(LoadedValue);
	}
	ASSERT_EQ(Material->GetParameterDefinitions().size(), 1u);
	EXPECT_EQ(Material->GetParameterDefinitions().front().Id, Id);
	EXPECT_EQ(FindExpression<DMaterialExpressionTextureSampleParameter2D>(*Material, BId)->UV.Connection.GetExpressionId(), UV->Id);
	EXPECT_EQ(FindExpression<DMaterialExpressionTextureSampleParameter2D>(*Material, BId)->DefaultValue.TextureFallback, EMaterialTextureFallback::FlatRGNormal);
	ASSERT_TRUE(UnloadPackage(Path)); CollectGarbage(Durin::EObjectFlags::Standalone);
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

	TStrongObjectPtr<DMaterialGraph> DocumentCommandGraph(DMaterialGraph::Create(*Owner));
	{
		GraphEditInternals::FGraphEditSession Initial(*Owner);
		for (auto& Position : Initial.Presentation.Nodes) { Position.X = 1800; Position.Y = 1200; }
		ASSERT_TRUE(Initial.Commit("Arrange fixture", nullptr));
	}
	auto Source = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
	auto Previous = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
	auto Consumer = Testing::MakeGraphExpression<DMaterialExpressionSaturate>();
	ASSERT_TRUE(DocumentCommandGraph->CreateExpression(*Source.Get(), 0, 0, Durin::ETransactionMode::NotTransactable));
	ASSERT_TRUE(DocumentCommandGraph->CreateExpression(*Previous.Get(), 0, 280, Durin::ETransactionMode::NotTransactable));
	Consumer->Input = {FindMaterialExpression(FMaterialExpressionEditing::GetExpressions(*Owner), Previous->Id)};
	ASSERT_TRUE(DocumentCommandGraph->CreateExpression(*Consumer.Get(), 350, 0, Durin::ETransactionMode::NotTransactable));
	ImGuiContext* Context = ImGui::CreateContext();
	auto& IO = ImGui::GetIO();
	IO.DisplaySize = {1200, 760}; IO.DeltaTime = 1.0f / 60.0f; IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault(); IO.Fonts->Build();
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	int Errors = 0;
	FMaterialGraphCanvas Canvas{*DMaterialGraph::Create(*Owner), {.ReportError = [&](std::string) { ++Errors; }}};
	Canvas.SetViewport(1.0f, {40, 40});
	ImVec2 Origin;

	const auto Frame = [&](ImVec2 Mouse, bool Down) {
		IO.AddMousePosEvent(Mouse.x, Mouse.y);
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, Down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1200, 760});
		ImGui::Begin("Shared gestures", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		Canvas.Draw(*Transactions.Get(), 660);
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
	Durin::FTransBufferTestAccess::SetState(*Transactions, Durin::ETransactorState::Undoing);
	Frame({Header.x + 40, Header.y + 20}, false);
	EXPECT_FALSE(Transactions->CanUndo());
	EXPECT_EQ(DocumentCommandGraph->FindNode(Source->Id)->GetView().Presentation.X, 0);
	Durin::FTransBufferTestAccess::SetState(*Transactions, Durin::ETransactorState::Idle);
	Frame(Header, false);
	EXPECT_EQ(FindViewNode(Testing::CaptureGraphView(*DocumentCommandGraph), Source->Id)->Presentation.X, 40);
	EXPECT_EQ(FindViewNode(Testing::CaptureGraphView(*DocumentCommandGraph), Consumer->Id)->Presentation.X, 390);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(FindViewNode(Testing::CaptureGraphView(*DocumentCommandGraph), Source->Id)->Presentation.X, 0);
	EXPECT_EQ(FindViewNode(Testing::CaptureGraphView(*DocumentCommandGraph), Consumer->Id)->Presentation.X, 350);
	Frame(Header, false); Frame(Header, true); Frame({Header.x + 60, Header.y + 30}, true);
	IO.AddKeyEvent(ImGuiKey_Escape, true); Frame({Header.x + 60, Header.y + 30}, true);
	IO.AddKeyEvent(ImGuiKey_Escape, false); Frame(Header, false);
	EXPECT_EQ(FindViewNode(Testing::CaptureGraphView(*DocumentCommandGraph), Source->Id)->Presentation.X, 0);
	EXPECT_EQ(FindViewNode(Testing::CaptureGraphView(*DocumentCommandGraph), Consumer->Id)->Presentation.X, 350);

	const auto& Metrics = FMaterialGraphGeometry::GetMetrics();
	const float PinY = Metrics.HeaderHeight + Metrics.SecondaryHeight + Metrics.BodyPadding;
	const auto View = Testing::CaptureGraphView(*DocumentCommandGraph);
	const ImVec2 Input{Origin.x + 350, Origin.y + GraphNodePinOffset(*FindViewNode(View, Consumer->Id))};
	const auto* SourceView = FindViewNode(View, Source->Id);
	const ImVec2 Output{Origin.x + GraphNodeWidth(*SourceView), Origin.y + GraphNodePinOffset(*SourceView)};
	const auto SourceLink = [&] { return FindViewNode(Testing::CaptureGraphView(*DocumentCommandGraph), Consumer->Id)->Inputs.front().Link.SourceNodeId; };
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
	TStrongObjectPtr<DMaterialGraph> DependencyDocumentAuthoringGraph(DMaterialGraph::Create(*Dependency));
	ASSERT_TRUE(DependencyDocumentAuthoringGraph->AddPort(false, {.Name = "Amount", .Default = {.Kind = EMaterialFunctionDefaultKind::Numeric}}, {}, 0, 0, Durin::ETransactionMode::NotTransactable));
	const auto PortId = Dependency->GetFunctionSignature().Inputs.back().Id;
	const auto Call = DocumentCommandGraph->InsertFunctionCall(*Dependency, 700, 0, Durin::ETransactionMode::NotTransactable).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Call);
	const auto CallId = Call.value();
	ASSERT_TRUE(GraphEditInternals::WriteExpressionConnection(Owner, FMaterialGraphPinAddress::Input(CallId, 0, PortId), FMaterialGraphPinAddress::Output({Previous->Id}), false, Durin::ETransactionMode::NotTransactable));
	const auto CallView = Testing::CaptureGraphView(*DocumentCommandGraph);
	const auto* CallNode = FindViewNode(CallView, CallId);
	const auto Port = std::ranges::find(CallNode->Inputs, PortId, &FMaterialGraphPinView::PortId);
	ASSERT_NE(Port, CallNode->Inputs.end());
	const ImVec2 CallInput{Origin.x + 700,
		Origin.y + PinY + static_cast<float>(Port - CallNode->Inputs.begin()) * Metrics.PinRowHeight};
	Frame(CallInput, false); Frame(CallInput, true); Frame(Output, true); Frame(Output, false);
	const auto ConnectedView = Testing::CaptureGraphView(*DocumentCommandGraph);
	const auto* ConnectedCall = FindViewNode(ConnectedView, CallId);
	EXPECT_EQ(std::ranges::find(ConnectedCall->Inputs, PortId, &FMaterialGraphPinView::PortId)->Link.SourceNodeId, Source->Id);
	ASSERT_TRUE(Transactions->Undo());
	const auto UndoneView = Testing::CaptureGraphView(*DocumentCommandGraph);
	const auto* UndoneCall = FindViewNode(UndoneView, CallId);
	EXPECT_EQ(std::ranges::find(UndoneCall->Inputs, PortId, &FMaterialGraphPinView::PortId)->Link.SourceNodeId, Previous->Id);

	// Shared keyboard commands retain selection and one-step undo.
	FMaterialGraphCanvasTestAccess::Select(Canvas, {Source->Id});
	const auto BeforeDuplicate = Testing::CaptureGraphView(*DocumentCommandGraph).Nodes.size();
	IO.AddKeyEvent(IO.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl, true); Frame({1050, 580}, false);
	Key(ImGuiKey_D);
	EXPECT_EQ(Testing::CaptureGraphView(*DocumentCommandGraph).Nodes.size(), BeforeDuplicate + 1);
	ASSERT_EQ(Canvas.GetSelection().size(), 1u);
	const auto DuplicateId = FMaterialGraphCanvasTestAccess::ProgramSelection(Canvas).front();
	EXPECT_NE(DuplicateId, Source->Id);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FALSE(DocumentCommandGraph->FindNode(DuplicateId));
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(DocumentCommandGraph->FindNode(DuplicateId));
	FMaterialGraphCanvasTestAccess::Select(Canvas, {DuplicateId});
	Key(ImGuiKey_X);
	EXPECT_EQ(Testing::CaptureGraphView(*DocumentCommandGraph).Nodes.size(), BeforeDuplicate);
	EXPECT_TRUE(Canvas.GetSelection().empty());
	Key(ImGuiKey_V);
	ASSERT_EQ(Canvas.GetSelection().size(), 1u);
	const auto PastedId = FMaterialGraphCanvasTestAccess::ProgramSelection(Canvas).front();
	EXPECT_NE(PastedId, DuplicateId);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FALSE(DocumentCommandGraph->FindNode(PastedId));
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_TRUE(DocumentCommandGraph->FindNode(PastedId));
	EXPECT_EQ(Testing::CaptureGraphView(*DocumentCommandGraph).Nodes.size(), BeforeDuplicate + 1);
	Key(ImGuiKey_A);
	EXPECT_EQ(Canvas.GetSelection().size(), Testing::CaptureGraphView(*DocumentCommandGraph).Nodes.size());
	IO.AddKeyEvent(IO.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl, false); Frame({1050, 580}, false);

	// Blank double click opens creation instead of starting a second marquee.
	Click({850, 510}); Click({850, 510});
	EXPECT_TRUE(FMaterialGraphCanvasTestAccess::Menu(Canvas));
	EXPECT_EQ(Errors, 0);
	Canvas.CancelInteraction();
	Transactions->Reset();
	ImGui::DestroyContext(Context);
	MarkAsGarbage(Owner); MarkAsGarbage(Dependency); CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST_P(FMaterialGraphCanvasInteractionTests, SurfaceDetailsCommitImmediatelyAndUndo)
{
	InitializeDObjectSystem();
	DObject* Owner = GetParam() ? static_cast<DObject*>(NewObject<DMaterialFunction>(nullptr, "FunctionDetails"))
		: static_cast<DObject*>(NewObject<DMaterial>(nullptr, "MaterialDetails"));
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Owner));
	auto Surface = Testing::MakeGraphExpression<DMaterialExpressionGetMaterialAttributes>();
	const auto BaseColorID = FMaterialAttributeDefinitionMap::GetID(EMaterialProperty::MP_BaseColor);
	const auto NormalID = FMaterialAttributeDefinitionMap::GetID(EMaterialProperty::MP_Normal);
	Surface->AttributeIDs = {BaseColorID, NormalID};
	ASSERT_TRUE(AuthoringGraph->CreateExpression(*Surface.Get(), 0, 0, Durin::ETransactionMode::NotTransactable));
	const auto Add = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::Add,
		EMaterialProgramValueType::Float3, {Surface->Id, 0, BaseColorID});
	ASSERT_TRUE(Add) << FormatMaterialGraphCommandResult(Add);
	const auto AddId = Add->front();
	ASSERT_TRUE(AuthoringGraph->GetSchema().TryCreateConnection(
		AuthoringGraph->FindNode(Surface->Id)->GetOutputPin(1), AuthoringGraph->FindNode(AddId)->GetInputPin(1),
		false, Durin::ETransactionMode::NotTransactable));
	const auto CheckConnections = [&](bool BaseColorEnabled) {
		const auto* Source = AuthoringGraph->FindNode(Surface->Id);
		const auto* Target = AuthoringGraph->FindNode(AddId);
		ASSERT_NE(Source, nullptr);
		ASSERT_NE(Target, nullptr);
		auto* Sum = Cast<DMaterialExpressionAdd>(Target->GetMaterialExpression());
		ASSERT_NE(Sum, nullptr);
		ASSERT_EQ(Source->GetView().Outputs.size(), BaseColorEnabled ? 2u : 1u);
		const auto CheckPin = [&](int32 InputIndex, const FMaterialExpressionInput& Link, EMaterialProperty Property, bool Connected) {
			const auto* Input = Target->GetInputPin(InputIndex);
			ASSERT_NE(Input, nullptr);
			const auto Slot = static_cast<int32>(Property);
			const auto Pins = Source->GetPins();
			const auto Output = std::ranges::find_if(Pins, [&](const auto& Pin) {
				return Pin.Direction == EMaterialGraphPinDirection::Output && Pin.SourceIndex == Slot;
			});
			ASSERT_EQ(Output != Pins.end(), Connected);
			ASSERT_EQ(Input->LinkedTo.size(), Connected ? 1u : 0u);
			if (Connected)
			{
				EXPECT_EQ(Link, FMaterialExpressionInput(Source->GetMaterialExpression(), Slot));
				EXPECT_EQ(Input->LinkedTo.front(), &*Output);
				EXPECT_TRUE(std::ranges::contains(Output->LinkedTo, Input));
			}
			else EXPECT_FALSE(Link.IsConnected());
		};
		CheckPin(0, Sum->A.Connection, EMaterialProperty::MP_BaseColor, BaseColorEnabled);
		CheckPin(1, Sum->B.Connection, EMaterialProperty::MP_Normal, true);
	};
	CheckConnections(true);
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	FMaterialGraphCanvas Canvas{*DMaterialGraph::Create(*Owner), {.ReportError = [](std::string Error) { ADD_FAILURE() << Error; }}};
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
		Canvas.DrawSelectionDetails(*Transactions.Get());
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
	const auto* Edited = Cast<DMaterialExpressionGetMaterialAttributes>(Node->Get());
	ASSERT_NE(Edited, nullptr);
	EXPECT_EQ(Edited->AttributeIDs, (std::vector<FGuid>{NormalID}));
	CheckConnections(false);
	EXPECT_EQ((Transactions->GetQueueLength() - Transactions->GetUndoCount()), 1u);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Edited->AttributeIDs, (std::vector<FGuid>{BaseColorID, NormalID}));
	CheckConnections(true);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Edited->AttributeIDs, (std::vector<FGuid>{NormalID}));
	CheckConnections(false);
	ImGui::DestroyContext(Context);
	Transactions->Reset();
	MarkAsGarbage(Owner); CollectGarbage(Durin::EObjectFlags::Standalone);
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

	TStrongObjectPtr<DMaterialGraph> FunctionAuthoringGraph(DMaterialGraph::Create(*Callee));
	FMaterialFunctionPort Port;
	Port.Id = FGuid::NewGuid(); Port.Type = EMaterialProgramValueType::Float3; Port.Name = "Vector"; Port.bRequired = true;
	ASSERT_TRUE(FunctionAuthoringGraph->AddPort(false, Port, {}, 0, 0, Durin::ETransactionMode::NotTransactable));
	ASSERT_TRUE(SavePackage(Callee->GetPackage()));
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	const auto Scalar = Testing::CreateGraphConstant(*AuthoringGraph, .3f);
	ASSERT_TRUE(Scalar);
	const auto Action = MakeFunctionCreationAction(Callee->GetObjectPath());
	EXPECT_TRUE(AuthoringGraph->GetSchema().CanCreate(Action, EMaterialProgramValueType::Float));
	EXPECT_TRUE(AuthoringGraph->GetSchema().CanCreate(Action, EMaterialProgramValueType::Float3));
	EXPECT_FALSE(AuthoringGraph->GetSchema().CanCreate(Action, EMaterialProgramValueType::Texture2D));
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	const auto Before = CaptureExpressions(*Material);
	const auto Created = Action.PerformAction(*AuthoringGraph, 32, 64,
		FMaterialGraphPinAddress::Output({Scalar.value()}), Durin::ETransactionMode::Record).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Created) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Created);
	const auto Id = Created.value();
	const auto View = Testing::CaptureGraphView(*AuthoringGraph);
	const auto* Node = FindViewNode(View, Id);
	ASSERT_NE(Node, nullptr);
	const auto Pin = std::ranges::find(Node->Inputs, Port.Id, &FMaterialGraphPinView::PortId);
	ASSERT_NE(Pin, Node->Inputs.end());
	EXPECT_TRUE(FMaterialGraphSchema::Accepts(Pin->AcceptedTypes, EMaterialProgramValueType::Float));
	EXPECT_EQ(Pin->Link.SourceNodeId, Scalar.value());
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(CaptureExpressions(*Material), Before);
	EXPECT_FALSE(Transactions->CanUndo());
	ASSERT_TRUE(Transactions->Redo());
	Port.Name = "Renamed"; Port.DisplayOrder = -50;
	auto* InputTerminal = Testing::FindFunctionTerminal<DMaterialExpressionFunctionInput>(*FunctionAuthoringGraph, Port.Id);
	ASSERT_TRUE(InputTerminal);
	ASSERT_TRUE(InputTerminal->SetFunctionPort(Port, Durin::ETransactionMode::NotTransactable));
	const auto Address = FMaterialGraphPinAddress::Input(Id, 999, Port.Id);
	FMaterialInputDefault Default;
	Default.Kind = EMaterialInputDefaultKind::Literal; Default.Type = EMaterialProgramValueType::Float3;
	Default.Literal = {.1f, .2f, .3f, 0.f};
	ASSERT_TRUE(GraphEditInternals::WriteInputDefault(Material.Get(), FMaterialGraphPinAddress::Input(Id, 0, Port.Id), Default, Durin::ETransactionMode::NotTransactable));
	ASSERT_TRUE(GraphEditInternals::WriteExpressionConnection(Material.Get(), Address, FMaterialGraphPinAddress::Output({}), true, Durin::ETransactionMode::Record));
	const auto Disconnected = Testing::CaptureGraphView(*AuthoringGraph);
	const auto* Call = FindViewNode(Disconnected, Id);
	ASSERT_NE(Call, nullptr);
	const auto Retained = std::ranges::find(Call->Inputs, Port.Id, &FMaterialGraphPinView::PortId);
	ASSERT_NE(Retained, Call->Inputs.end());
	EXPECT_FALSE(Retained->Link.SourceNodeId.IsValid());
	EXPECT_EQ(Retained->InlineDefault, Default);
	ASSERT_TRUE(Transactions->Undo());
	const auto* RestoredInput = AuthoringGraph->FindInputPin(Address);
	ASSERT_NE(RestoredInput, nullptr);
	EXPECT_EQ(RestoredInput->Link.SourceNodeId, Scalar.value());
	EXPECT_FALSE(GraphEditInternals::WriteExpressionConnection(Material.Get(), FMaterialGraphPinAddress::Input(Id, 0, FGuid::NewGuid()), FMaterialGraphPinAddress::Output({Scalar.value()}), false, Durin::ETransactionMode::NotTransactable));
	EXPECT_FALSE(AuthoringGraph->GetSchema().CanCreate(MakePortCreationAction(false, EMaterialProgramValueType::Float)));
	const auto OutputAction = MakePortCreationAction(true, EMaterialProgramValueType::Float3);
	EXPECT_TRUE(FunctionAuthoringGraph->GetSchema().CanCreate(OutputAction, EMaterialProgramValueType::Float));
	const auto Input = MakePortCreationAction(false, EMaterialProgramValueType::Float).PerformAction(*FunctionAuthoringGraph, 0, 0, {}, Durin::ETransactionMode::NotTransactable).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Input);
	Transactions->Reset();
	const auto Signature = Callee->GetFunctionSignature();
	const auto Output = OutputAction.PerformAction(*FunctionAuthoringGraph, 10, 20,
		FMaterialGraphPinAddress::Output({Input.value()}));
	ASSERT_TRUE(Output) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(Output);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Callee->GetFunctionSignature(), Signature);
	EXPECT_FALSE(Transactions->CanUndo());
	for (const auto Type : {EMaterialProgramValueType::Texture2D, EMaterialProgramValueType::MaterialAttributes})
	{
		const auto TypedInput = MakePortCreationAction(false, Type).PerformAction(*FunctionAuthoringGraph, 0, 0, {}, Durin::ETransactionMode::NotTransactable);
		ASSERT_TRUE(TypedInput) << ::Durin::MaterialEditor::FormatMaterialGraphCommandResult(TypedInput);
	}
}

TEST(FMaterialLayoutSettingsTests, SharedProfileMergesResourceWritesAndKeepsNewerPreferences)
{
	using Durin::MaterialEditor::FMaterialEditorProfileSettings;
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/LayoutSettings/", Testing::GetTestWorkDirectory().generic_string() + "/");
	const auto File = "MaterialEditor/Profiles/Test." + FGuid::NewGuid().ToString() + ".yaml";
	const auto Physical = FPaths::LaunchConfigsDir() + File;
	struct FCleanup { std::string Path; ~FCleanup() { std::filesystem::remove(Path); } } Cleanup{Physical};
	FMaterialEditorProfileSettings First, Second;
	ASSERT_TRUE(First.SelectStorage(File)); ASSERT_TRUE(Second.SelectStorage(File));
	First.bAutoCompile = false;
	First.SetViewport("/LayoutSettings/A.A", {.Zoom = 1.5f}); ASSERT_TRUE(First.Save());
	Second.bDetailsVisible = false;
	Second.SetViewport("/LayoutSettings/B.B", {.Zoom = .5f}); ASSERT_TRUE(Second.Save());
	FMaterialEditorProfileSettings Loaded(File); ASSERT_TRUE(Loaded.Load());
	EXPECT_FALSE(Loaded.bAutoCompile); EXPECT_FALSE(Loaded.bDetailsVisible);
	ASSERT_NE(nullptr, Loaded.FindViewport("/LayoutSettings/A.A")); ASSERT_NE(nullptr, Loaded.FindViewport("/LayoutSettings/B.B"));
	EXPECT_FLOAT_EQ(1.5f, Loaded.FindViewport("/LayoutSettings/A.A")->Zoom); EXPECT_FLOAT_EQ(.5f, Loaded.FindViewport("/LayoutSettings/B.B")->Zoom);
	FPackagePath Source, Destination;
	ASSERT_TRUE(FPackagePath::TryCreate("/LayoutSettings/A", Source));
	ASSERT_TRUE(FPackagePath::TryCreate("/LayoutSettings/Moved", Destination));
	FAssetData Renamed;
	ASSERT_TRUE(FTopLevelAssetPath::TryCreate(Destination, "A", Renamed.AssetPath));
	FSoftObjectPath Old; ASSERT_TRUE(FSoftObjectPath::TryCreate("/LayoutSettings/A.A", Old));
	FMaterialEditorProfileSettings::RemapStoredViewports(Renamed, Old);
	ASSERT_TRUE(First.Save()); // A stale window must not resurrect a moved resource or old preference.
	ASSERT_TRUE(Loaded.Load()); EXPECT_EQ(nullptr, Loaded.FindViewport("/LayoutSettings/A.A"));
	EXPECT_NE(nullptr, Loaded.FindViewport("/LayoutSettings/Moved.A")); EXPECT_NE(nullptr, Loaded.FindViewport("/LayoutSettings/B.B"));
}
