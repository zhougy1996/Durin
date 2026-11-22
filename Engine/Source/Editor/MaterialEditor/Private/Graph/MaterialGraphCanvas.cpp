#include "Graph/MaterialGraphNodeDisplay.h"
#include "Graph/MaterialGraphInteraction.h"
#include "Graph/MaterialGraphCatalog.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpressionIO.h"
#include "Graph/MaterialGraphCreationShortcuts.h"
#include "Graph/MaterialGraphValueTypes.h"
#include "Graph/MaterialGraphCanvas.h"
#include "Graph/MaterialEditingOperations.h"
#include "MaterialGraphEditSession.h"
#include "MaterialGraphEditInternals.h"

#include "Transactions/Transaction.h"
#include "MonaImGui.h"
#include "Asset/Asset.h"
#include "Asset/AssetDragDrop.h"
#include "ThirdParty/ImGui/imgui_internal.h"

namespace Durin::MaterialEditor
{
	namespace
	{
		std::optional<FMaterialGraphClipboardPayload> GraphClipboard;
		auto BroadcastHint(const FMaterialGraphPinView& Pin) -> const char*
		{
			return !Pin.AcceptedTypes.empty() && Pin.AcceptedTypes.front() > EMaterialProgramValueType::Float
				&& Pin.AcceptedTypes.front() <= EMaterialProgramValueType::Float4
				&& std::ranges::contains(Pin.AcceptedTypes, EMaterialProgramValueType::Float)
				? "\nScalar inputs are copied to every component." : "";
		}
		auto ConstraintHint(EMaterialProgramValueType Type,
			const FMaterialFunctionValueConstraint& Constraint) -> std::string
		{
			if (Constraint.Mode == EMaterialFunctionValueConstraintMode::Unconstrained
				&& Constraint.Stages == EMaterialEvaluationStage::Both) return {};
			if (Constraint.Mode == EMaterialFunctionValueConstraintMode::Exact)
				return GetMaterialValueSemanticsName({Type, Constraint.Stages,
					Constraint.Kind, Constraint.Space});
			const char* Stage = Constraint.Stages == EMaterialEvaluationStage::Both
				? "Vertex|Pixel" : Constraint.Stages == EMaterialEvaluationStage::Vertex
					? "Vertex" : "Pixel";
			return std::format("{} [{}]", Constraint.Mode
				== EMaterialFunctionValueConstraintMode::NonSpatial ? "Non-spatial" : "Any semantics", Stage);
		}
		constexpr float PinExpansionHeight = 20.0f;

		auto IsGraphTooltipHovered() -> bool
		{
			auto& Delay = ImGui::GetStyle().HoverDelayNormal;
			const float PreviousDelay = std::exchange(Delay, 0.8f);
			const bool bHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal
				| ImGuiHoveredFlags_NoSharedDelay | ImGuiHoveredFlags_NoNavOverride);
			Delay = PreviousDelay;
			return bHovered;
		}

		auto HideUnusedAdvancedPins(FMaterialGraphView& View,
			const std::unordered_set<FGuid>& ExpandedNodes,
			std::unordered_set<FGuid>& CollapsibleNodes) -> void
		{
			CollapsibleNodes.clear();
			std::unordered_map<FGuid, uint16> UsedSampleOutputs;
			for (const auto& Node : View.Nodes)
				for (const auto& Pin : Node.Inputs)
				{
					const auto& Link = Pin.Link;
					if (Link.SourceNodeId.IsValid() && !Link.SourceOutputId.IsValid()
						&& FindMaterialSampleOutput(EMaterialProgramOpcode::TextureSampleParameter2D, Link.SourceOutputIndex))
						UsedSampleOutputs[Link.SourceNodeId] |= static_cast<uint16>(1u << Link.SourceOutputIndex);
				}
			for (auto& Node : View.Nodes)
			{
				const auto Hide = [&](bool bCollapsible) {
					if (bCollapsible) CollapsibleNodes.insert(Node.Node.Id);
					return bCollapsible && !ExpandedNodes.contains(Node.Node.Id);
				};
				if (IsMaterialSamplingNode(Node.Node.Opcode))
					std::erase_if(Node.Outputs, [&](const auto& Pin) {
						return Hide(Pin.Type == EMaterialProgramValueType::Texture2D
							&& !(UsedSampleOutputs[Node.Node.Id] & (1u << Pin.OutputIndex)));
					});
				std::erase_if(Node.Inputs, [&](const auto& Pin) {
					return Hide(Pin.bAdvanced && !Pin.bRequired && !Pin.Link.SourceNodeId.IsValid()
						&& Pin.InlineDefault.Kind == EMaterialInputDefaultKind::None);
				});
			}
		}

		auto InputLabel(const FMaterialGraphNodeView& Node, const FMaterialGraphPinView& Pin, const DMaterial* Material = nullptr) -> std::string
		{
			if (Pin.Link.SourceNodeId.IsValid() || IsCompactGraphOperation(Node)) return Pin.Name;
			if (Node.Node.IsSampleUVInput(Pin.InputIndex) && !Pin.bUseConstant)
			{
				return "Mesh UV0";
			}
			const auto& Value = Pin.InlineDefault;
			if (Value.Kind == EMaterialInputDefaultKind::Literal)
			{
				if (Value.Type == EMaterialProgramValueType::Float) return std::format("{}: {:g}", Pin.Name, Value.Literal.X);
				if (Value.Type == EMaterialProgramValueType::Float2) return std::format("{}: {:g}, {:g}", Pin.Name, Value.Literal.X, Value.Literal.Y);
				return std::format("{}: ({:g}, {:g}, {:g})", Pin.Name, Value.Literal.X, Value.Literal.Y, Value.Literal.Z);
			}
			return Pin.Name;
		}

		const auto& Metrics = FMaterialGraphGeometry::GetMetrics();
		const float NodePadding = Metrics.BodyPadding;
		// Reveal occluded wires without reducing text, pin or heading contrast.
		constexpr int GraphNodeBodyAlpha = 230;
		constexpr int GraphSelectedNodeBodyAlpha = 240;
		constexpr float GraphBodyFontHeight = 14.0f;
		constexpr float GraphTitleFontHeight = 16.0f;
		constexpr float GraphSecondaryFontHeight = 13.0f;

		auto Add(const ImVec2& A, const ImVec2& B) -> ImVec2
		{
			return {A.x + B.x, A.y + B.y};
		}

		auto Subtract(const ImVec2& A, const ImVec2& B) -> ImVec2
		{
			return {A.x - B.x, A.y - B.y};
		}

		auto Multiply(const ImVec2& Value, float Scale) -> ImVec2
		{
			return {Value.x * Scale, Value.y * Scale};
		}

		auto Contains(const ImVec2& Minimum, const ImVec2& Maximum,
			const ImVec2& Point) -> bool
		{
			return Point.x >= Minimum.x && Point.y >= Minimum.y
				&& Point.x <= Maximum.x && Point.y <= Maximum.y;
		}

		auto Intersects(
			const ImVec2& AMin, const ImVec2& AMax,
			const ImVec2& BMin, const ImVec2& BMax) -> bool
		{
			return AMin.x <= BMax.x && AMax.x >= BMin.x
				&& AMin.y <= BMax.y && AMax.y >= BMin.y;
		}

		auto DrawCulledLink(ImDrawList& DrawList, const ImVec2& A, const ImVec2& B,
			const ImVec2& CanvasMinimum, const ImVec2& CanvasMaximum,
			ImU32 Color, float Thickness) -> void
		{
			const float Tangent = std::max(40.0f, std::abs(B.x - A.x) * 0.45f);
			const ImVec2 ControlA = Add(A, {Tangent, 0.0f});
			const ImVec2 ControlB = Subtract(B, {Tangent, 0.0f});
			// The curve stays inside its control hull; include stroke and AA fringe.
			const float Padding = Thickness * 0.5f + DrawList._FringeScale;
			const ImVec2 Minimum(
				std::min({A.x, ControlA.x, ControlB.x, B.x}) - Padding,
				std::min({A.y, ControlA.y, ControlB.y, B.y}) - Padding);
			const ImVec2 Maximum(
				std::max({A.x, ControlA.x, ControlB.x, B.x}) + Padding,
				std::max({A.y, ControlA.y, ControlB.y, B.y}) + Padding);
			if (Intersects(Minimum, Maximum, CanvasMinimum, CanvasMaximum))
				DrawList.AddBezierCubic(A, ControlA, ControlB, B, Color, Thickness);
		}

		auto NodeTitleColor(EMaterialProgramOpcode Opcode) -> ImU32
		{
			using Op = EMaterialProgramOpcode;
			switch (Opcode)
			{
			case Op::Parameter: case Op::TextureParameter: case Op::TextureSampleParameter2D:
				return IM_COL32(48, 100, 66, 255);
			case Op::Constant: case Op::TextureCoordinates: case Op::WorldPosition: case Op::Time:
			case Op::CameraPosition: case Op::CameraVector: case Op::ObjectPosition:
			case Op::VertexNormal: case Op::ScreenPosition: case Op::ViewSize:
				return IM_COL32(44, 83, 126, 255);
			case Op::TextureSample2D: case Op::BlendNormalsRNM:
				return IM_COL32(114, 79, 43, 255);
			case Op::Swizzle: case Op::AppendVector: case Op::MakeFloat2: case Op::MakeFloat3: case Op::MakeFloat4:
			case Op::Splat2: case Op::Splat3: case Op::Splat4:
				return IM_COL32(91, 65, 122, 255);
			case Op::FunctionInput: case Op::FunctionOutput: case Op::FunctionCall:
				return IM_COL32(39, 99, 105, 255);
			case Op::StaticBool: case Op::StaticSwitch: case Op::QualitySwitch:
			case Op::FeatureLevelSwitch:
				return IM_COL32(112, 76, 142, 255);
			case Op::MakeMaterialAttributes: case Op::GetMaterialAttributes: case Op::SetMaterialAttributes:
				return IM_COL32(119, 57, 60, 255);
			default: return IM_COL32(66, 72, 83, 255);
			}
		}

		auto TypeColor(EMaterialProgramValueType Type) -> ImU32
		{
			switch (Type)
			{
			case EMaterialProgramValueType::Float: return IM_COL32(120, 200, 120, 255);
			case EMaterialProgramValueType::Float2: return IM_COL32(100, 180, 220, 255);
			case EMaterialProgramValueType::Float3: return IM_COL32(220, 170, 80, 255);
			case EMaterialProgramValueType::Float4: return IM_COL32(210, 110, 180, 255);
			case EMaterialProgramValueType::Texture2D:
			case EMaterialProgramValueType::TextureCube: return IM_COL32(150, 110, 230, 255);
			case EMaterialProgramValueType::MaterialAttributes: return IM_COL32(235, 155, 70, 255);
			case EMaterialProgramValueType::StaticBool: return IM_COL32(188, 125, 225, 255);
			}
			return IM_COL32_WHITE;
		}

		auto WithAlpha(ImU32 Color, uint8 Alpha) -> ImU32
		{
			return (Color & IM_COL32(255, 255, 255, 0)) | IM_COL32(0, 0, 0, Alpha);
		}

		auto Ellipsize(std::string_view Text, float LogicalWidth) -> std::string
		{
			if (Text.empty()) return {};
			const float CharacterWidth = std::max(ImGui::GetFontSize() * 0.52f, 1.0f);
			const size_t Capacity = static_cast<size_t>(std::max(0.0f,
				std::floor(LogicalWidth / CharacterWidth)));
			if (Text.size() <= Capacity) return std::string(Text);
			if (Capacity <= 3) return std::string(Capacity, '.');
			return std::format("{}...", Text.substr(0, Capacity - 3));
		}

	}

	struct FMaterialGraphCanvas::FVisualNode
	{
		const FMaterialGraphNodeView* View = nullptr;
		ImVec2 Minimum{};
		ImVec2 Maximum{};
		ImVec2 OutputPin{};
		std::vector<ImVec2> OutputPins;
		std::vector<ImVec2> InputPins;
		auto OutputIndex(const FMaterialProgramLink& Link) const -> size_t
		{
			for (size_t Index = 0; Index < View->Outputs.size(); ++Index)
				if (View->Outputs[Index].PortId == Link.SourceOutputId
					&& View->Outputs[Index].OutputIndex == Link.SourceOutputIndex) return Index;
			return 0;
		}
		auto OutputPosition(const FMaterialProgramLink& Link) const -> ImVec2
		{
			return OutputPins.empty() ? OutputPin : OutputPins[OutputIndex(Link)];
		}
	};

	struct FMaterialGraphCanvas::FVisualGraph
	{
		std::vector<FVisualNode> Nodes;
		std::unordered_map<FGuid, size_t> Indices;
	};

	FMaterialGraphCanvas::FMaterialGraphCanvas(DMaterialGraph& Graph, FMaterialGraphEditorServices InServices)
		: EditorGraph(&Graph), Services(std::move(InServices)), ReadModel(EditorGraph.Get()) {}
	FMaterialGraphCanvas::~FMaterialGraphCanvas() = default;

	auto FMaterialGraphCanvas::ReportError(std::string Message) const -> void
	{
		if (Services.ReportError) Services.ReportError(std::move(Message));
	}

	auto FMaterialGraphCanvas::BreakPinLinks(const FMaterialGraphPinAddress& Address) -> bool
	{
		std::string Error;
		const bool Changed = EditorGraph->GetSchema().BreakPinLinks(*EditorGraph, Address, ETransactionMode::Record, &Error);
		if (!Error.empty()) ReportError(std::move(Error));
		return Changed;
	}

	auto FMaterialGraphCanvas::ConnectPins(const FMaterialGraphPinAddress& Input, FMaterialProgramLink Source, bool Replace) -> bool
	{
		std::string Error;
		const bool Changed = EditorGraph->GetSchema().TryCreateConnection(*EditorGraph, Input,
			FMaterialGraphPinAddress::Output(Source), Replace, ETransactionMode::Record, &Error);
		if (!Error.empty()) ReportError(std::move(Error));
		return Changed;
	}

	auto FMaterialGraphCanvas::CheckCommand(const FMaterialGraphCommandResult& Result) const -> bool
	{
		if (!Result.HasError()) return true;
		ReportError(FormatMaterialGraphCommandResult(Result));
		return false;
	}

	auto FMaterialGraphCanvas::ClearSharedClipboard() -> void { GraphClipboard.reset(); }

	void FMaterialGraphCanvas::RestoreSelection(const std::unordered_set<FMaterialGraphNodeAddress>& Selection)
	{
		SelectedNodes.clear();
		for (const auto& Key : Selection)
		{
			if (EditorGraph->ResolveNode(Key))
				SelectedNodes.insert(Key);
		}
	}

	auto FMaterialGraphCanvas::SelectAndFrame(const FGuid& NodeId) -> bool
	{
		if (!NodeId.IsValid()) return false;
		SelectedSurfaceOutput.reset();
		SelectedNodes = {SelectionKey(NodeId)};
		PendingFrameNode = NodeId;
		return true;
	}

	auto FMaterialGraphCanvas::SelectAndFrameDiagnostic(
		const FMaterialProgramDiagnostic& Diagnostic) -> bool
	{
		SelectedDiagnostic = Diagnostic;
		switch (Diagnostic.LocationKind)
		{
		case EMaterialProgramDiagnosticLocationKind::Node:
		case EMaterialProgramDiagnosticLocationKind::Input:
			SelectedSurfaceOutput.reset();
			return SelectAndFrame(Diagnostic.NodeId);
		case EMaterialProgramDiagnosticLocationKind::SurfaceOutput:
		{
			if (Diagnostic.LocationIndex >= FMaterialAttributeDefinitionMap::GetDefinitions().size()
				|| !FMaterialAttributeDefinitionMap::Find(static_cast<EMaterialProperty>(Diagnostic.LocationIndex))) return false;
			const auto* Root = EditorGraph->GetRootNode();
			if (!Diagnostic.NodeId.IsValid() && !Root) return false;
			if (Root) OutputNodeId = Root->GetView().Node.Id;
			SelectedNodes = {Diagnostic.NodeId.IsValid() ? SelectionKey(Diagnostic.NodeId) : FMaterialGraphNodeAddress{EMaterialGraphEndpoint::MaterialRoot}};
			PendingFrameNode = Diagnostic.NodeId.IsValid() ? Diagnostic.NodeId : OutputNodeId;
			SelectedSurfaceOutput =
				static_cast<EMaterialProperty>(Diagnostic.LocationIndex);
			return true;
		}
		case EMaterialProgramDiagnosticLocationKind::Program:
			return false;
		}
		return false;
	}

	auto FMaterialGraphCanvas::CancelInteraction() -> bool
	{
		ParameterTextDraft.reset();
		NodeLiteralDraft.reset();
		RootInputFrame.Finish(true, [this](bool bCancel) { return FinishRootInputEdit(bCancel); });
		if (!FinishParameterEdit(true)) return false;
		if (MoveSession.IsActive())
		{
			MoveSession.Cancel();
		}
		Interaction = FIdleInteraction{};
		bViewStale = true;
		return true;
	}

	auto FMaterialGraphCanvas::ResetInteraction() -> void
	{
		CancelInteraction();
	}

	auto FMaterialGraphCanvas::ToggleNodePins(const FGuid& NodeId) -> void
	{
		if (!CollapsiblePinNodes.contains(NodeId)) return;
		if (!ExpandedPinNodes.erase(NodeId)) ExpandedPinNodes.insert(NodeId);
		bViewStale = true;
	}

	auto FMaterialGraphCanvas::PrepareDocumentView(DObject& Owner) -> void
	{
		if (MoveSession.IsActive() && !MoveSession.IsCurrent(Owner)) CancelInteraction();
		const bool bFunction = Cast<DMaterialFunction>(&Owner) != nullptr;
		if (Catalog.empty() || bFunctionGraph != bFunction)
		{
			Catalog = EnumerateCatalog();
			bCreationMenuResultsDirty = true;
			bViewStale = true;
		}
		bFunctionGraph = bFunction;
		const auto Changes = ReadModel.Refresh(Owner);
		const auto* Root = EditorGraph->GetRootNode();
		OutputNodeId = Root ? Root->GetView().Node.Id : FGuid{};
		const auto& Inspection = ReadModel.GetView();
		using N = EMaterialGraphNodeChange;
		if (bViewStale || Changes.Has(EMaterialGraphChange::Reset)
			|| std::ranges::any_of(Changes.Nodes, [](const auto& Node) {
				return (Node.Flags & (N::Added | N::Removed | N::Interface | N::Inputs | N::Content)) != N::None;
			}))
		{
			CachedView = Inspection;
			HideUnusedAdvancedPins(CachedView, ExpandedPinNodes, CollapsiblePinNodes);
			CachedNodeIndices.clear();
			for (size_t Index = 0; Index < CachedView.Nodes.size(); ++Index)
				CachedNodeIndices.emplace(CachedView.Nodes[Index].Node.Id, Index);
			std::erase_if(SelectedNodes, [&](const auto& Id) { return !CachedNodeIndices.contains(std::holds_alternative<FGuid>(Id) ? std::get<FGuid>(Id) : OutputNodeId); });
			if (PendingFrameNode.IsValid() && !CachedNodeIndices.contains(PendingFrameNode)) PendingFrameNode = {};
			if (const auto* Context = std::get_if<FContextMenuInteraction>(&Interaction);
				Context && Context->ContextNode.IsValid() && !CachedNodeIndices.contains(Context->ContextNode)) ResetInteraction();
			if (const auto* Link = std::get_if<FLinkingInteraction>(&Interaction);
				Link && !EditorGraph->FindOutputPin({Link->SourceNode, Link->SourceOutputIndex, Link->SourceOutputId})) ResetInteraction();
			if (const auto* Menu = std::get_if<FNodeCreationMenuInteraction>(&Interaction);
				Menu && Menu->SourceNode.IsValid() && !EditorGraph->FindOutputPin({Menu->SourceNode, Menu->SourceOutputIndex, Menu->SourceOutputId})) ResetInteraction();
			if (const auto* Reconnect = std::get_if<FReconnectingInputInteraction>(&Interaction);
				Reconnect && !EditorGraph->FindInputPin(Reconnect->Destination)) ResetInteraction();
			bVisualGraphTopologyStale = true;
			bViewStale = false;
		}
		else if (!Changes.IsEmpty())
		{
			for (const auto& Change : Changes.Nodes)
			{
				const auto It = CachedNodeIndices.find(Change.NodeId);
				if (It == CachedNodeIndices.end()) continue;
				const auto& Node = Inspection.Nodes[It->second];
				CachedView.Nodes[It->second].Presentation = Node.Presentation;
			}
		}
		for (const auto& Position : MoveSession.GetPositions())
			if (const auto It = CachedNodeIndices.find(Position.NodeId); It != CachedNodeIndices.end())
			{
				auto& Target = CachedView.Nodes[It->second].Presentation;
				Target.X = Position.X; Target.Y = Position.Y;
			}
	}

	auto FMaterialGraphCanvas::PrepareView(DMaterial& Material) -> const FMaterialGraphView&
	{
		PrepareDocumentView(Material);

		return CachedView;
	}

	auto FMaterialGraphCanvas::PrepareVisualGraph(
		const FMaterialGraphView& View,
		const ImVec2& CanvasMinimum) -> const FVisualGraph&
	{
		if (!CachedVisualGraph) CachedVisualGraph = std::make_unique<FVisualGraph>();
		FVisualGraph& Result = *CachedVisualGraph;
		if (bVisualGraphTopologyStale)
		{
			Result.Nodes.clear();
			Result.Indices.clear();
			Result.Nodes.reserve(View.Nodes.size());
			Result.Indices.reserve(View.Nodes.size());
			for (const FMaterialGraphNodeView& Node : View.Nodes)
			{
				FVisualNode Visual;
				Visual.View = &Node;
				Visual.InputPins.resize(Node.Inputs.size());
				Visual.OutputPins.resize(Node.Outputs.size());
				Result.Indices.emplace(Node.Node.Id, Result.Nodes.size());
				Result.Nodes.push_back(std::move(Visual));
			}
			bVisualGraphTopologyStale = false;
		}
		for (FVisualNode& Visual : Result.Nodes)
		{
			const FMaterialGraphNodeView& Node = *Visual.View;
			const ImVec2 GraphPosition(
				static_cast<float>(Node.Presentation.X),
				static_cast<float>(Node.Presentation.Y));
			const float NodeHeight = GraphNodeHeight(Node)
				+ (CollapsiblePinNodes.contains(Node.Node.Id) ? PinExpansionHeight : 0.0f);
			Visual.Minimum = Add(CanvasMinimum, Add(Pan, Multiply(GraphPosition, Zoom)));
			Visual.Maximum = Add(Visual.Minimum,
				Multiply({GraphNodeWidth(Node), NodeHeight}, Zoom));
			Visual.OutputPin = {
				Visual.Maximum.x,
				Visual.Minimum.y + GraphNodePinOffset(Node) * Zoom};
			const float PinSpacing = GraphNodePinRowHeight(Node) * Zoom;
			for (size_t Index = 0; Index < Visual.OutputPins.size(); ++Index)
				Visual.OutputPins[Index] = {Visual.OutputPin.x, Visual.OutputPin.y + PinSpacing * Index};
			for (size_t Index = 0; Index < Node.Inputs.size(); ++Index)
				Visual.InputPins[Index] = {
					Visual.Minimum.x,
					Visual.OutputPin.y + PinSpacing * Index};
		}
		return Result;
	}

	auto FMaterialGraphCanvas::GetSelectedProgramNodes() const -> std::vector<FGuid>
	{
		std::vector<FGuid> Nodes;
		for (const FMaterialGraphNodeAddress& Id : SelectedNodes)
			if (const auto* Node = std::get_if<FGuid>(&Id)) Nodes.push_back(*Node);
			else if (OutputNodeId.IsValid()) Nodes.push_back(OutputNodeId);
		return Nodes;
	}

	auto FMaterialGraphCanvas::FrameNodes(
		const FMaterialGraphView& View,
		const ImVec2& CanvasSize, EFrameScope Scope) -> void
	{
		bool bFound = false;
		ImVec2 Minimum{};
		ImVec2 Maximum{};
		for (const FMaterialGraphNodeView& Node : View.Nodes)
		{
			if (Scope == EFrameScope::Selection
				&& !SelectedNodes.contains(SelectionKey(Node.Node.Id))) continue;
			const ImVec2 Position(
				static_cast<float>(Node.Presentation.X),
				static_cast<float>(Node.Presentation.Y));
			const float Height = GraphNodeHeight(Node)
				+ (CollapsiblePinNodes.contains(Node.Node.Id) ? PinExpansionHeight : 0.0f);
			if (!bFound)
			{
				Minimum = Position;
				Maximum = Add(Position, {GraphNodeWidth(Node), Height});
				bFound = true;
			}
			else
			{
				Minimum.x = std::min(Minimum.x, Position.x);
				Minimum.y = std::min(Minimum.y, Position.y);
				Maximum.x = std::max(Maximum.x, Position.x + GraphNodeWidth(Node));
				Maximum.y = std::max(Maximum.y, Position.y + Height);
			}
		}
		if (!bFound) return;
		const ImVec2 Extent = Subtract(Maximum, Minimum);
		Zoom = std::clamp(std::min(
			(CanvasSize.x - 80.0f) / std::max(Extent.x, 1.0f),
			(CanvasSize.y - 80.0f) / std::max(Extent.y, 1.0f)), 0.25f, 1.5f);
		const ImVec2 Center = Multiply(Add(Minimum, Maximum), 0.5f);
		Pan = Subtract(Multiply(CanvasSize, 0.5f), Multiply(Center, Zoom));
	}

	auto FMaterialGraphCanvas::DrawLinks(
		const FVisualGraph& VisualGraph,
		const ImVec2& CanvasMinimum,
		const ImVec2& CanvasMaximum,
		ImDrawList& DrawList) const -> void
	{
		for (const auto& Node : EditorGraph->GetNodes())
		{
			const auto Visible = VisualGraph.Indices.find(Node->GetView().Node.Id);
			if (Visible == VisualGraph.Indices.end()) continue;
			const auto& Destination = VisualGraph.Nodes[Visible->second];
			// Compact nodes keep the authored display order while hiding unused pins.
			size_t Ordinal = 0;
			for (size_t I = 0; I < Destination.View->Inputs.size(); ++I)
			{
				const auto& InputView = Destination.View->Inputs[I];
				while (Ordinal < Node->GetView().Inputs.size() && Node->GetView().Inputs[Ordinal].InputIndex != InputView.InputIndex) ++Ordinal;
				const auto* Input = Node->GetInputPin(static_cast<int32>(Ordinal++));
				if (!Input || Input->LinkedTo.empty()) continue;
				const auto* Output = Input->LinkedTo.front();
				const auto SourceId = Output->GetOwningNode()->GetView().Node.Id;
				const auto Found = VisualGraph.Indices.find(SourceId);
				if (Found == VisualGraph.Indices.end()) continue;
				const auto& Source = VisualGraph.Nodes[Found->second];
				const auto& Outputs = Source.View->Outputs;
				const auto Pin = std::ranges::find(Outputs, Output->SourceIndex, &FMaterialGraphOutputPinView::SourceIndex);
				if (Pin == Outputs.end()) continue;
				const auto A = Source.OutputPins[Pin - Outputs.begin()];
				const bool Focused = SelectedNodes.empty() || SelectedNodes.contains(SelectionKey(Node->GetView().Node.Id)) || SelectedNodes.contains(SelectionKey(SourceId));
				const auto Color = TypeColor(Pin->Type);
				DrawCulledLink(DrawList, A, Destination.InputPins[I], CanvasMinimum, CanvasMaximum,
					Focused ? Color : WithAlpha(Color, 72), Focused ? 3.0f : 1.5f);
			}
		}
	}

	auto FMaterialGraphCanvas::HandleKeyboardInput(
		DObject& Owner,
		DTransactor& Transactions,
		const FMaterialGraphView& View,
		const ImVec2& CanvasMinimum,
		const ImVec2& CanvasSize,
		const ImVec2& Mouse,
		bool bInputAvailable) -> void
	{
		if (!bInputAvailable || !std::holds_alternative<FIdleInteraction>(Interaction)) return;
		const ImGuiIO& IO = ImGui::GetIO();
		if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A))
		{
			SelectedNodes.clear();
			for (const FMaterialGraphNodeView& Node : View.Nodes)
				SelectedNodes.insert(SelectionKey(Node.Node.Id));
		}
		if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C) && !SelectedNodes.empty())
		{
			const std::vector<FGuid> Selection = GetSelectedProgramNodes();
			CopyNodes(Owner, Selection);
		}
		if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X) && !SelectedNodes.empty())
		{
			const std::vector<FGuid> Selection = GetSelectedProgramNodes();
			CutNodes(Owner, Transactions, Selection);
		}
		if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D) && !SelectedNodes.empty())
		{
			const std::vector<FGuid> Selection = GetSelectedProgramNodes();
			DuplicateNodes(Owner, Transactions, Selection);
		}
		if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V) && GraphClipboard)
		{
			const ImVec2 GraphPosition = Multiply(
				Subtract(Subtract(Mouse, CanvasMinimum), Pan), 1.0f / Zoom);
			PasteNodes(Owner, Transactions, GraphPosition);
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !SelectedNodes.empty())
		{
			const std::vector<FGuid> Selection = GetSelectedProgramNodes();
			RemoveNodes(Owner, Transactions, Selection);
		}
		if (ImGui::IsKeyPressed(ImGuiKey_F))
			FrameNodes(View, CanvasSize, SelectedNodes.empty()
				? EFrameScope::All : EFrameScope::Selection);
	}

	auto FMaterialGraphCanvas::CopyNodes(
		DObject& Owner,
		std::span<const FGuid> NodeIds) -> void
	{
		if (NodeIds.empty()) return;
		FMaterialGraphClipboardPayload Payload;
		std::string Error;
		if (EditorGraph->CopySelection(NodeIds, Payload, &Error)) GraphClipboard = std::move(Payload);
		else ReportError(std::move(Error));
	}

	auto FMaterialGraphCanvas::CutNodes(
		DObject& Owner,
		DTransactor& Transactions,
		std::span<const FGuid> NodeIds) -> void
	{
		if (NodeIds.empty()) return;
		FMaterialGraphClipboardPayload Payload;
		const FMaterialGraphCommandResult Cut = EditorGraph->CutSelection(NodeIds, Payload);
		if (!CheckCommand(Cut)) return;
		GraphClipboard = std::move(Payload);
		SelectedNodes.clear();
	}

	auto FMaterialGraphCanvas::DuplicateNodes(
		DObject& Owner,
		DTransactor& Transactions,
		std::span<const FGuid> NodeIds) -> void
	{
		if (NodeIds.empty()) return;
		const auto Duplicated = EditorGraph->DuplicateNodes(NodeIds, 40, 40);
		if (!CheckCommand(Duplicated)) return;
		SelectedNodes.clear();
		for (const auto* Node : *Duplicated) SelectedNodes.insert(SelectionKey(Node->GetView().Node.Id));
	}

	auto FMaterialGraphCanvas::PasteNodes(
		DObject& Owner,
		DTransactor& Transactions,
		const ImVec2& GraphPosition) -> void
	{
		if (!GraphClipboard) return;
		const bool bRepeated = LastPasteAnchor && LastPasteAnchor->x == GraphPosition.x && LastPasteAnchor->y == GraphPosition.y;
		const uint32 Offset = bRepeated ? RepeatedPasteCount + 1 : 0;
		const auto Pasted = EditorGraph->Paste(*GraphClipboard, static_cast<int32>(std::round(GraphPosition.x)) + 24 * Offset, static_cast<int32>(std::round(GraphPosition.y)) + 24 * Offset);
		if (!CheckCommand(Pasted)) return;
		LastPasteAnchor = GraphPosition;
		RepeatedPasteCount = Offset;
		SelectedSurfaceOutput.reset();
		SelectedNodes.clear();
		for (const auto* Node : *Pasted) SelectedNodes.insert(SelectionKey(Node->GetView().Node.Id));
	}

	auto FMaterialGraphCanvas::RemoveNodes(
		DObject& Owner,
		DTransactor& Transactions,
		std::span<const FGuid> NodeIds) -> void
	{
		if (NodeIds.empty()) return;
		const FMaterialGraphCommandResult Removed =
			EditorGraph->RemoveNodes(NodeIds);
		if (CheckCommand(Removed)) SelectedNodes.clear();
	}

	auto FMaterialGraphCanvas::HasClipboard() const -> bool
	{
		return GraphClipboard.has_value();
	}

	auto FMaterialGraphCanvas::DrawContextMenu(
		DObject& Owner,
		DTransactor& Transactions,
		const FMaterialGraphView& View) -> void
	{
		auto* SurfaceMaterial = Cast<DMaterial>(&Owner);
		if (!ImGui::BeginPopup("MaterialGraphContext"))
		{
			if (std::holds_alternative<FContextMenuInteraction>(Interaction))
				ResetInteraction();
			return;
		}

		const auto* Context = std::get_if<FContextMenuInteraction>(&Interaction);
		const FGuid ContextNode = Context ? Context->ContextNode : FGuid{};
		const std::optional<EMaterialProperty> ContextSurfaceOutput =
			Context ? Context->SurfaceOutput : std::nullopt;
		const auto ContextNodeIt = std::ranges::find(View.Nodes, ContextNode,
			[](const FMaterialGraphNodeView& Node) { return Node.Node.Id; });
		const FMaterialGraphNodeView* ContextNodeView =
			ContextNodeIt == View.Nodes.end() ? nullptr : &*ContextNodeIt;
		if (ContextNodeView && !ContextSurfaceOutput)
		{
			std::vector<FGuid> ContextSelection;
			if (SelectedNodes.contains(SelectionKey(ContextNodeView->Node.Id)))
				ContextSelection = GetSelectedProgramNodes();
			else ContextSelection = {ContextNodeView->Node.Id};
			const auto& Edited = ContextNodeView->Node;
			if (!Edited.bMaterialOutput && Edited.Opcode == EMaterialProgramOpcode::Constant && ImGui::BeginMenu("Type"))
			{
				for (EMaterialProgramValueType Type : {EMaterialProgramValueType::Float,
					EMaterialProgramValueType::Float2, EMaterialProgramValueType::Float3,
					EMaterialProgramValueType::Float4})
				{
					if (ImGui::MenuItem(GetProgramTypeName(Type), nullptr, Edited.ResultType == Type))
					{
						if (auto* Node = EditorGraph->FindNode(Edited.Id))
							CheckCommand(Node->SetConstantValue(MakeParameterValue(Type, Edited.GetConstantLiteral())));
					}
				}
				ImGui::EndMenu();
			}
			if (EditorGraph->GetSchema().CanOwnParameters() && SurfaceMaterial && !Edited.bMaterialOutput && Edited.Opcode == EMaterialProgramOpcode::Constant && ImGui::BeginMenu("Promote to Parameter"))
			{
				ImGui::InputTextWithHint("##ParameterName", "Parameter name", PromotionNameDraft.data(), PromotionNameDraft.size());
				if (ImGui::MenuItem("Create / Reuse"))
					CheckCommand(MaterialEditing::PromoteConstantToParameter(*SurfaceMaterial, Edited.Id, FName(PromotionNameDraft.data())));
				ImGui::EndMenu();
			}
			if (ImGui::MenuItem("Copy"))
				CopyNodes(Owner, ContextSelection);
			if (ImGui::MenuItem("Duplicate"))
				DuplicateNodes(Owner, Transactions, ContextSelection);
			if (ImGui::MenuItem("Cut"))
				CutNodes(Owner, Transactions, ContextSelection);
			const bool CanRemoveSelection = std::ranges::none_of(View.Nodes, [&](const auto& Node) {
				return std::ranges::contains(ContextSelection, Node.Node.Id)
					&& !EditorGraph->GetSchema().CanRemove(Node.Node.bMaterialOutput);
			});
			if (ImGui::MenuItem("Delete", nullptr, false, CanRemoveSelection))
				RemoveNodes(Owner, Transactions, ContextSelection);
		}
		else if (SurfaceMaterial && ContextSurfaceOutput && ContextNodeView && ContextNodeView->Node.bMaterialOutput)
		{
			if (*ContextSurfaceOutput == EMaterialProperty::MP_MaterialAttributes)
			{
				if (ImGui::MenuItem("Disconnect Surface"))
					BreakPinLinks(FMaterialGraphPinAddress::MaterialOutput());
			}
			else
			{
				const FMaterialProgramLink& Link = GetMaterialSurfaceOutputLink(
					View.Outputs, *ContextSurfaceOutput);
				const ImVec2 SurfacePosition = ImVec2(static_cast<float>(ContextNodeView->Presentation.X), static_cast<float>(ContextNodeView->Presentation.Y));
				const MaterialEditing::FMaterialGraphSurfaceNodeRequest NodeRequest{
					.Output = *ContextSurfaceOutput,
					.X = static_cast<int32>(std::round(SurfacePosition.x
						- Metrics.NodeWidth - Metrics.ColumnGap)),
					.Y = static_cast<int32>(std::round(SurfacePosition.y)),
				};
				if (Link.SourceNodeId.IsValid()
					&& ImGui::MenuItem("Disconnect to Default"))
					BreakPinLinks(FMaterialGraphPinAddress::MaterialOutput(*ContextSurfaceOutput));
				if (ImGui::MenuItem("Reset Default"))
					if (auto* Root = EditorGraph->FindNode(ContextNodeView->Node.Id))
						CheckCommand(Root->SetPinUseConstant(FMaterialGraphPinAddress::MaterialOutput(*ContextSurfaceOutput), false));
				if (MaterialEditing::CanPromoteSurfaceOutput(*SurfaceMaterial, *ContextSurfaceOutput)
					&& ImGui::MenuItem("Promote to Parameter"))
					CheckCommand(
						MaterialEditing::PromoteSurfaceOutputToParameter(*SurfaceMaterial, NodeRequest));
				if (MaterialEditing::SurfaceTextureOutput(*SurfaceMaterial, *ContextSurfaceOutput) && ImGui::MenuItem("Add Texture"))
					CheckCommand(MaterialEditing::AddTextureToSurfaceOutput(*SurfaceMaterial, NodeRequest));
			}
		}
		ImGui::EndPopup();
	}

	auto FMaterialGraphCanvas::DrawNodeHeading(const FVisualNode& Visual, ImDrawList& DrawList,
		const DMaterial* Material) const -> void
	{
		const auto Display = MakeGraphNodeDisplay(*Visual.View, Material);
		const float NodeWidth = GraphNodeWidth(*Visual.View);
		const float NodeHeaderHeight = GraphNodeHeaderHeight(*Visual.View);
		if (DetailLevel != EMaterialGraphDetailLevel::Overview)
		{
			const float FontSize = GraphTitleFontHeight * Zoom;
			const std::string Label = Ellipsize(Display.Title,
				(NodeWidth - NodePadding * 2.0f) * Zoom
					* ImGui::GetFontSize() / FontSize);
			const ImVec4 Clip(Visual.Minimum.x + 5.0f, Visual.Minimum.y,
				Visual.Maximum.x - 5.0f,
				Visual.Minimum.y + NodeHeaderHeight * Zoom);
			DrawList.AddText(ImGui::GetFont(), FontSize,
				Add(Visual.Minimum,
					{8.0f * Zoom, (NodeHeaderHeight * Zoom - FontSize) * 0.5f}),
				IM_COL32(235, 238, 242, 255), Label.c_str(), nullptr, 0.0f, &Clip);
			if (!Display.Subtitle.empty()
				&& !IsHeaderOnlyGraphNode(*Visual.View) && !IsCompactGraphOperation(*Visual.View)
				&& !HasCompactTextureHeader(*Visual.View))
			{
				const bool bColor = Display.Value && (Visual.View->Node.ResultType == EMaterialProgramValueType::Float3
					|| Visual.View->Node.ResultType == EMaterialProgramValueType::Float4);
				const float ColorSpace = bColor ? 18.0f : 0.0f;
				if (bColor)
				{
					const auto& Value = *Display.Value;
					const ImVec2 Minimum = Add(Visual.Minimum, {8.0f * Zoom, (NodeHeaderHeight + 1.0f) * Zoom});
					const ImVec2 Maximum = Add(Minimum, {12.0f * Zoom, 12.0f * Zoom});
					DrawList.AddRectFilled(Minimum, Maximum, ImGui::ColorConvertFloat4ToU32(
						{std::clamp(Value.X, 0.0f, 1.0f), std::clamp(Value.Y, 0.0f, 1.0f),
							std::clamp(Value.Z, 0.0f, 1.0f), 1.0f}));
					DrawList.AddRect(Minimum, Maximum, IM_COL32(150, 156, 168, 255));
				}
				const std::string Secondary = Ellipsize(Display.Subtitle,
					(NodeWidth - NodePadding * 2.0f - ColorSpace) * Zoom
						* ImGui::GetFontSize() / (GraphSecondaryFontHeight * Zoom));
				const ImVec4 SecondaryClip(Visual.Minimum.x + 5.0f,
					Visual.Minimum.y + NodeHeaderHeight * Zoom,
					Visual.Maximum.x - 5.0f,
					Visual.Minimum.y + (NodeHeaderHeight + Metrics.SecondaryHeight) * Zoom);
				DrawList.AddText(ImGui::GetFont(), GraphSecondaryFontHeight * Zoom,
					Add(Visual.Minimum, {(8.0f + ColorSpace) * Zoom, NodeHeaderHeight * Zoom}),
					IM_COL32(165, 172, 186, 255), Secondary.c_str(), nullptr, 0.0f,
					&SecondaryClip);
			}
		}
	}

	auto FMaterialGraphCanvas::HandleCreationShortcut(DObject& Owner, DTransactor& Transactions,
		const ImVec2& Position) -> bool
	{
		const auto& IO = ImGui::GetIO();
		if (!std::holds_alternative<FIdleInteraction>(Interaction) || IO.WantTextInput
			|| IO.KeyCtrl || IO.KeyShift || IO.KeyAlt || IO.KeySuper
			|| ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right)
			|| !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return false;
		for (const auto& Shortcut : MaterialGraphCreationShortcuts)
		{
			if (!ImGui::IsKeyDown(Shortcut.Key)) continue;
			const auto Entry = std::ranges::find_if(Catalog, [&](const auto& Candidate) {
				return Candidate.Opcode == Shortcut.Opcode && Candidate.ResultType == Shortcut.Type;
			});
			if (Entry == Catalog.end()) continue;
			std::vector<DMaterialGraphNode*> AuxiliaryNodes;
			const auto Created = MakeCreationAction(*Entry).PerformAction(*EditorGraph,
				static_cast<int32>(std::round(Position.x)), static_cast<int32>(std::round(Position.y)), {}, ETransactionMode::Record, &AuxiliaryNodes);
			if (CheckCommand(Created))
			{
				SelectedNodes.clear();
				SelectedSurfaceOutput.reset();
				SelectedNodes.insert((*Created)->GetView().Node.Id);
				for (const auto* Node : AuxiliaryNodes) SelectedNodes.insert(SelectionKey(Node->GetView().Node.Id));
				RememberCreation(MakeCreationAction(*Entry));
			}
			return true;
		}
		return false;
	}

	auto FMaterialGraphCanvas::PrepareFunctionView(DMaterialFunction& Function) -> void
	{
		PrepareDocumentView(Function);
	}

	auto FMaterialGraphCanvas::HitTest(const FVisualGraph& Graph, const ImVec2& Minimum,
		const ImVec2& Maximum, const ImVec2& Mouse) const -> FPointerHit
	{
		if (!Contains(Minimum, Maximum, Mouse)) return {};
		// Reverse paint order prevents pins on covered nodes from receiving a gesture.
		for (auto It = Graph.Nodes.rbegin(); It != Graph.Nodes.rend(); ++It)
		{
			const auto& Visual = *It;
			if (!Intersects(Visual.Minimum, Visual.Maximum, Minimum, Maximum)) continue;
			FPointerHit Hit;
			if (Contains(Visual.Minimum, Visual.Maximum, Mouse)) Hit.Node = &Visual;
			if (DetailLevel != EMaterialGraphDetailLevel::Overview)
			{
				const auto FindNearestPin = [&](std::span<const ImVec2> Pins) -> std::optional<size_t> {
					std::optional<size_t> Nearest;
					float Distance = Metrics.MinimumHitDiameter * 0.5f;
					for (size_t Index = 0; Index < Pins.size(); ++Index)
					{
						const float Candidate = std::hypot(Mouse.x - Pins[Index].x, Mouse.y - Pins[Index].y);
						if (Candidate <= Distance)
						{
							Nearest = Index;
							Distance = Candidate;
						}
					}
					return Nearest;
				};
				if (const auto Input = FindNearestPin(Visual.InputPins))
					{ Hit.InputNode = &Visual; Hit.InputIndex = static_cast<uint32>(*Input); }
				if (const auto Output = FindNearestPin(Visual.OutputPins))
					{ Hit.OutputNode = &Visual; Hit.OutputIndex = *Output; }
			}
			if (Hit.Node && !Hit.InputNode && !Hit.OutputNode
				&& CollapsiblePinNodes.contains(Visual.View->Node.Id)
				&& DetailLevel != EMaterialGraphDetailLevel::Overview)
			{
				const float CenterX = (Visual.Minimum.x + Visual.Maximum.x) * 0.5f;
				Hit.bPinExpansion = Contains({CenterX - 16.0f * Zoom, Visual.Maximum.y - PinExpansionHeight * Zoom},
					{CenterX + 16.0f * Zoom, Visual.Maximum.y}, Mouse);
			}
			if (Hit.Node || Hit.InputNode || Hit.OutputNode) return Hit;
		}
		return {};
	}

	auto FMaterialGraphCanvas::HandleViewportInput(const ImVec2& Minimum,
		const ImVec2& Mouse, bool bHovered) -> void
	{
		if (bHovered && !ImGui::IsAnyItemActive() && ImGui::GetIO().MouseWheel != 0.0f)
		{
			const auto Anchor = Multiply(Subtract(Subtract(Mouse, Minimum), Pan), 1.0f / Zoom);
			Zoom = std::clamp(Zoom * (ImGui::GetIO().MouseWheel > 0 ? 1.12f : 0.89f), 0.25f, 2.0f);
			Pan = Subtract(Subtract(Mouse, Minimum), Multiply(Anchor, Zoom));
		}
		if (bHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
			Pan = Add(Pan, ImGui::GetIO().MouseDelta);
		DetailLevel = FMaterialGraphGeometry::SelectDetailLevel(Zoom, DetailLevel);
	}

	auto FMaterialGraphCanvas::HandlePointerInput(DObject& Owner, DTransactor& Transactions,
		const FMaterialGraphView& View, const FVisualGraph& VisualGraph,
		const ImVec2& CanvasMinimum, const ImVec2& CanvasMaximum, const ImVec2& CanvasSize,
		const ImVec2& Mouse, bool bPointerAvailable) -> void
	{
		const auto Hit = HitTest(VisualGraph, CanvasMinimum, CanvasMaximum, Mouse);
		if (Hit.bPinExpansion && bPointerAvailable
			&& std::holds_alternative<FIdleInteraction>(Interaction)
			&& ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			ToggleNodePins(Hit.Node->View->Node.Id);
			return;
		}
		if (ImGui::BeginDragDropTargetCustom(ImRect(CanvasMinimum, CanvasMaximum), ImGui::GetID("MaterialFunctionDrop")))
		{
			if (const auto* Payload = ImGui::AcceptDragDropPayload(AssetDragDropPayloadType);
				Payload && Payload->IsDelivery() && Payload->DataSize == sizeof(FAssetDragDropPayload))
			{
				const auto& Data = *static_cast<const FAssetDragDropPayload*>(Payload->Data);
				FTopLevelAssetPath Path;
				if (std::ranges::contains(Data.AssetPath, '\0')
					&& FTopLevelAssetPath::TryCreate(Data.AssetPath.data(), Path))
				{
					const auto Position = Multiply(Subtract(Subtract(Mouse, CanvasMinimum), Pan), 1.0f / Zoom);
					const auto Created = MakeFunctionCreationAction(Path.ToString()).PerformAction(*EditorGraph,
						static_cast<int32>(std::round(Position.x)), static_cast<int32>(std::round(Position.y)));
					if (CheckCommand(Created))
					{
						SelectedSurfaceOutput.reset();
						SelectedNodes = {(*Created)->GetView().Node.Id};
						RememberCreation(MakeFunctionCreationAction(Path.ToString()));
						ResetInteraction();
					}
				}
				else ReportError("Drag a material function asset into the graph.");
			}
			ImGui::EndDragDropTarget();
		}
		const auto* HoveredNode = Hit.Node;
		const auto* HoveredInputNode = Hit.InputNode;
		const auto* HoveredOutput = Hit.OutputNode;
		const auto HoveredInputIndex = Hit.InputIndex;
		const auto HoveredOutputIndex = Hit.OutputIndex;
		auto* SurfaceMaterial = Cast<DMaterial>(&Owner);
		const auto& VisualNodes = VisualGraph.Nodes;
		const auto& VisualIndices = VisualGraph.Indices;
		auto* DrawList = ImGui::GetWindowDrawList();

		const bool bCanvasPointerInteractionAvailable = bPointerAvailable;
		const bool bCanvasKeyboardInteractionAvailable =
			bCanvasPointerInteractionAvailable && !ImGui::GetIO().WantTextInput;
		if (bCanvasPointerInteractionAvailable
			&& (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
				|| ImGui::IsMouseClicked(ImGuiMouseButton_Middle)
				|| ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
			ImGui::SetWindowFocus();
		bool bCreationShortcutHandled = false;
		const auto& IO = ImGui::GetIO();
		if (bCanvasKeyboardInteractionAvailable
			&& std::holds_alternative<FIdleInteraction>(Interaction)
			&& !IO.KeyCtrl && !IO.KeyShift && !IO.KeyAlt && !IO.KeySuper
			&& !HoveredNode && !HoveredOutput && !HoveredInputNode
			&& !ImGui::IsMouseDown(ImGuiMouseButton_Middle)
			&& !ImGui::IsMouseDown(ImGuiMouseButton_Right)
			&& ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			bCreationShortcutHandled = HandleCreationShortcut(Owner, Transactions,
				Multiply(Subtract(Subtract(Mouse, CanvasMinimum), Pan), 1.0f / Zoom));
		}
		const bool bOpenCreationMenuByDoubleClick = bCanvasPointerInteractionAvailable
			&& !bCreationShortcutHandled
			&& std::holds_alternative<FIdleInteraction>(Interaction)
			&& ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
			&& !HoveredNode && !HoveredOutput && !HoveredInputNode;
		if (bOpenCreationMenuByDoubleClick)
		{
			Interaction = FNodeCreationMenuInteraction{
				.SourceNode = {},
				.GraphPosition = Multiply(
					Subtract(Subtract(Mouse, CanvasMinimum), Pan), 1.0f / Zoom)};
		}
		if (bCanvasKeyboardInteractionAvailable && ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			ResetInteraction();
		}

		if (bCanvasPointerInteractionAvailable && !bCreationShortcutHandled && !bOpenCreationMenuByDoubleClick
			&& std::holds_alternative<FIdleInteraction>(Interaction)
			&& ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			if (HoveredInputNode)
			{
				Interaction = FReconnectingInputInteraction{HoveredInputNode->View->InputAddress(HoveredInputNode->View->Inputs[HoveredInputIndex])};
			}
			else if (HoveredOutput)
			{
				const auto& Pin = HoveredOutput->View->Outputs[HoveredOutputIndex];
				Interaction = FLinkingInteraction{HoveredOutput->View->Node.Id, Pin.OutputIndex, Pin.PortId};
			}

			else if (HoveredNode)
			{
				const FGuid Id = HoveredNode->View->Node.Id;
				SelectedSurfaceOutput.reset();
				bool bRemovedFromSelection = false;
				if (ImGui::GetIO().KeyCtrl)
				{
					bRemovedFromSelection = SelectedNodes.erase(SelectionKey(Id)) != 0;
					if (!bRemovedFromSelection) SelectedNodes.insert(SelectionKey(Id));
				}
				else if (!SelectedNodes.contains(SelectionKey(Id))) SelectedNodes = {SelectionKey(Id)};
				if (!bRemovedFromSelection)
				{
					const std::vector<FGuid> Selection = GetSelectedProgramNodes();
					const auto Begun = MoveSession.Begin(*EditorGraph, Selection, &Transactions);
					if (!Begun) ReportError(Begun.error());
					else
					{
						FMovingInteraction Moving{.StartMouse = Mouse};
						for (const FVisualNode& Visual : VisualNodes)
							if (SelectedNodes.contains(SelectionKey(Visual.View->Node.Id)))
								Moving.StartPositions.emplace(
									Visual.View->Node.Id, Visual.View->Presentation);
						Interaction = std::move(Moving);
					}
				}
			}
			else
			{
				if (!ImGui::GetIO().KeyShift)
				{
					SelectedNodes.clear();
					SelectedSurfaceOutput.reset();
				}
				Interaction = FMarqueeInteraction{Mouse};
			}
		}

		if (auto* Moving = std::get_if<FMovingInteraction>(&Interaction))
		{
			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) Moving->bReleased = true;
			if (!Moving->bReleased && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
			{
				const ImVec2 Delta = Multiply(Subtract(Mouse, Moving->StartMouse), 1.0f / Zoom);
				{
					std::vector<FMaterialGraphNodePresentation> Positions;
					for (const auto& [Id, Start] : Moving->StartPositions)
					{
						auto Position = Start;
						Position.X = static_cast<int32>(std::round(Start.X + Delta.x));
						Position.Y = static_cast<int32>(std::round(Start.Y + Delta.y));
						Positions.push_back(std::move(Position));
					}
					const auto Applied = MoveSession.Apply(Positions);
					if (!Applied) ReportError(Applied.error());
					else if (*Applied)
						for (const auto& Position : MoveSession.GetPositions())
							if (const auto It = CachedNodeIndices.find(Position.NodeId); It != CachedNodeIndices.end())
							{
								auto& Target = CachedView.Nodes[It->second].Presentation;
								Target.X = Position.X; Target.Y = Position.Y;
							}
				}
			}
			if (Moving->bReleased && Transactions.CanTransact())
			{
				if (const auto Result = MoveSession.Commit(); !Result) ReportError(Result.error());
				ResetInteraction();
			}
		}

		if (const auto* Marquee = std::get_if<FMarqueeInteraction>(&Interaction))
		{
			const ImVec2 Minimum(std::min(Marquee->Start.x, Mouse.x),
				std::min(Marquee->Start.y, Mouse.y));
			const ImVec2 Maximum(std::max(Marquee->Start.x, Mouse.x),
				std::max(Marquee->Start.y, Mouse.y));
			DrawList->AddRectFilled(Minimum, Maximum, IM_COL32(70, 140, 220, 35));
			DrawList->AddRect(Minimum, Maximum, IM_COL32(80, 160, 235, 180));
			if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
			{
				for (const FVisualNode& Visual : VisualNodes)
					if (Intersects(Minimum, Maximum, Visual.Minimum, Visual.Maximum))
						SelectedNodes.insert(SelectionKey(Visual.View->Node.Id));
				ResetInteraction();
			}
		}

		if (const auto* ActiveLink = std::get_if<FLinkingInteraction>(&Interaction))
		{
			const FGuid SourceNode = ActiveLink->SourceNode;
			const FMaterialProgramLink SourceLink{SourceNode, ActiveLink->SourceOutputIndex, ActiveLink->SourceOutputId};
			const auto SourceIt = VisualIndices.find(SourceNode);
			if (SourceIt != VisualIndices.end())
			{
				const ImVec2 A = VisualNodes[SourceIt->second].OutputPosition(SourceLink);
				DrawList->AddBezierCubic(A, Add(A, {60.0f, 0.0f}),
					Subtract(Mouse, {60.0f, 0.0f}), Mouse,
					TypeColor(VisualNodes[SourceIt->second].View->Node.ResultType), 2.5f);
			}
			if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
			{
				ResetInteraction();
				if (bPointerAvailable && HoveredInputNode)
					ConnectPins(HoveredInputNode->View->InputAddress(HoveredInputNode->View->Inputs[HoveredInputIndex]),
						SourceLink, ImGui::GetIO().KeyShift);

				else if (bCanvasPointerInteractionAvailable && !HoveredNode
					&& !HoveredOutput)
				{
					Interaction = FNodeCreationMenuInteraction{
						.SourceNode = SourceNode,
						.SourceOutputIndex = SourceLink.SourceOutputIndex,
						.SourceOutputId = SourceLink.SourceOutputId,
						.GraphPosition = Multiply(
							Subtract(Subtract(Mouse, CanvasMinimum), Pan), 1.0f / Zoom)};
				}
			}
		}
		if (const auto* Reconnecting =
			std::get_if<FReconnectingInputInteraction>(&Interaction))
		{
			const auto Address = Reconnecting->Destination;
			const auto DestinationIt = VisualIndices.find(Address.IsMaterialRoot() ? OutputNodeId : Address.NodeId);
			if (DestinationIt != VisualIndices.end())
			{
				const auto& Destination = VisualNodes[DestinationIt->second];
				const auto Pin = std::ranges::find_if(Destination.View->Inputs, [&](const auto& Input) {
					return Destination.View->InputAddress(Input) == Address;
				});
				if (Pin != Destination.View->Inputs.end())
				{
					const auto Row = static_cast<size_t>(Pin - Destination.View->Inputs.begin());
					const ImVec2 A = Destination.InputPins[Row];
					DrawList->AddBezierCubic(A, Subtract(A, {60.0f, 0.0f}),
						Add(Mouse, {60.0f, 0.0f}), Mouse, IM_COL32(240, 210, 105, 255), 2.5f);
				}
			}
			if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
			{
				if (bPointerAvailable && HoveredOutput)
				{
					const auto& Pin = HoveredOutput->View->Outputs[HoveredOutputIndex];
					const FMaterialProgramLink Source{HoveredOutput->View->Node.Id, Pin.OutputIndex, Pin.PortId};
					ConnectPins(Address, Source, true);
				}
				ResetInteraction();
			}
		}

		HandleKeyboardInput(Owner, Transactions, View, CanvasMinimum,
			CanvasSize, Mouse, bCanvasKeyboardInteractionAvailable);
		if (bCanvasPointerInteractionAvailable
			&& std::holds_alternative<FIdleInteraction>(Interaction)
			&& ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			if (!HoveredNode && !HoveredInputNode)
			{
				Interaction = FNodeCreationMenuInteraction{
					.GraphPosition = Multiply(
						Subtract(Subtract(Mouse, CanvasMinimum), Pan), 1.0f / Zoom)};
			}
			else
			{
				Interaction = FContextMenuInteraction{
					.ContextNode = HoveredNode ? HoveredNode->View->Node.Id
						: HoveredInputNode ? HoveredInputNode->View->Node.Id : FGuid{},
					.SurfaceOutput = HoveredInputNode ? HoveredInputNode->View->Inputs[HoveredInputIndex].MaterialProperty : std::nullopt};
				ImGui::OpenPopup("MaterialGraphContext");
			}
		}
		if (bCanvasKeyboardInteractionAvailable && std::holds_alternative<FIdleInteraction>(Interaction)
			&& ImGui::IsKeyPressed(ImGuiKey_Space))
		{
			Interaction = FNodeCreationMenuInteraction{
				.SourceNode = {},
				.GraphPosition = Multiply(
					Subtract(Subtract(Mouse, CanvasMinimum), Pan), 1.0f / Zoom)};
		}
	}

	auto FMaterialGraphCanvas::Draw(DTransactor& Transactions,
		float Height) -> void
	{
		const auto& OpenFunction = Services.OpenFunction;
		if (!EditorGraph->GetMaterialOrFunction()) { CancelInteraction(); return; }
		auto& Owner = *EditorGraph->GetMaterialOrFunction();
		auto* Material = Cast<DMaterial>(&Owner);
		ImGui::PushID(this);
		if (ImGui::BeginChild("MaterialGraph", ImVec2(0.0f, Height),
			ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar
				| ImGuiWindowFlags_NoScrollWithMouse))
		{
			const bool bFrameAllRequested = ImGui::Button("Frame All");
			ImGui::SameLine();
			const bool bFrameSelectionRequested = ImGui::Button("Frame Selection");
			ImGui::SameLine();
			if (ImGui::Button("Auto Layout"))
			{
				std::string Error;
				if (!EditorGraph->Layout({}, ETransactionMode::Record, &Error)) ReportError(std::move(Error));
			}
			ImGui::SameLine();
			const char* DetailName = DetailLevel == EMaterialGraphDetailLevel::Overview ? "Overview" : "Editing";
			ImGui::TextDisabled("%s", DetailName);
			ImGui::SameLine();
			ImGui::TextDisabled("(?)");
			if (IsGraphTooltipHovered())
				ImGui::SetTooltip("Wheel: zoom\nMMB: pan\nLMB: select / drag\nCtrl-click: toggle selection\nShift: add marquee selection / replace link");

			if (Material) PrepareView(*Material);
			else PrepareDocumentView(Owner);
			const FMaterialGraphView& View = CachedView;
			const ImVec2 CanvasMinimum = ImGui::GetCursorScreenPos();
			ImVec2 CanvasSize = ImGui::GetContentRegionAvail();
			CanvasSize.x = std::max(CanvasSize.x, 64.0f);
			CanvasSize.y = std::max(CanvasSize.y, 64.0f);
			const ImVec2 CanvasMaximum = Add(CanvasMinimum, CanvasSize);
			ImGui::InvisibleButton("##Canvas", CanvasSize,
				ImGuiButtonFlags_MouseButtonLeft
					| ImGuiButtonFlags_MouseButtonMiddle
					| ImGuiButtonFlags_MouseButtonRight);
			const bool bHovered = ImGui::IsItemHovered();
			if (Material)
			{
				AcceptTextureDrop(*Material, Transactions, CanvasMinimum);
				UpdateTexturePreviews(*Material);
			}
			ImDrawList* DrawList = ImGui::GetWindowDrawList();
			DrawList->PushClipRect(CanvasMinimum, CanvasMaximum, true);
			DrawList->AddRectFilled(CanvasMinimum, CanvasMaximum,
				IM_COL32(24, 27, 32, 255));
			if (Material)
			{
				const FMaterialCompileStatus& CompileStatus = Material->GetMaterialCompileStatus();
				if (CompileStatus.HasUnsubmittedEdits()
					|| CompileStatus.State == EMaterialCompileState::Running
					|| CompileStatus.State == EMaterialCompileState::Failed)
				{
					const bool bFailed = CompileStatus.State == EMaterialCompileState::Failed;
					const char* Label = CompileStatus.HasUnsubmittedEdits()
						? (Material->GetAcceptedCompiledProgram()
							? "Needs compile - preview is last known good"
							: "Needs compile - preview uses fallback")
						: bFailed
						? "Compile failed - preview uses error material"
						: (Material->GetAcceptedCompiledProgram()
							? "Compiling - preview is last known good"
							: "Compiling material graph");
					DrawList->AddText(Add(CanvasMinimum, {12.0f, 10.0f}),
						bFailed ? IM_COL32(245, 110, 105, 255)
							: IM_COL32(235, 190, 85, 255), Label);
				}
			}

			const ImVec2 Mouse = ImGui::GetIO().MousePos;
			HandleViewportInput(CanvasMinimum, Mouse, bHovered);
			const float GraphBodyFontSize = GraphBodyFontHeight * Zoom;
			const float GraphTitleFontSize = GraphTitleFontHeight * Zoom;
			const float GraphSecondaryFontSize = GraphSecondaryFontHeight * Zoom;

			const float GridStep = 32.0f * Zoom;
			if (GridStep >= 16.0f)
			{
				for (float X = std::fmod(Pan.x, GridStep); X < CanvasSize.x; X += GridStep)
					DrawList->AddLine(Add(CanvasMinimum, {X, 0.0f}),
						Add(CanvasMinimum, {X, CanvasSize.y}), IM_COL32(48, 52, 60, 45));
				for (float Y = std::fmod(Pan.y, GridStep); Y < CanvasSize.y; Y += GridStep)
					DrawList->AddLine(Add(CanvasMinimum, {0.0f, Y}),
						Add(CanvasMinimum, {CanvasSize.x, Y}), IM_COL32(48, 52, 60, 45));
			}

			const FVisualGraph& VisualGraph = PrepareVisualGraph(View, CanvasMinimum);
			const std::vector<FVisualNode>& VisualNodes = VisualGraph.Nodes;
			const std::unordered_map<FGuid, size_t>& VisualIndices =
				VisualGraph.Indices;

			DrawLinks(VisualGraph, CanvasMinimum, CanvasMaximum, *DrawList);

			const FLinkingInteraction* Linking =
				std::get_if<FLinkingInteraction>(&Interaction);
			const FGuid LinkSourceNode = Linking ? Linking->SourceNode : FGuid{};
			const FMaterialProgramLink LinkSource{LinkSourceNode, Linking ? Linking->SourceOutputIndex : uint8{0},
				Linking ? Linking->SourceOutputId : FGuid{}};
			const auto Hit = HitTest(VisualGraph, CanvasMinimum, CanvasMaximum, Mouse);
			const auto* HoveredNode = Hit.Node;
			const bool bOverNodeTitle = HoveredNode
				&& Mouse.y < HoveredNode->Minimum.y + GraphNodeHeaderHeight(*HoveredNode->View) * Zoom;
			const auto* HoverTarget = Hit.InputNode ? Hit.InputNode : Hit.OutputNode;
			if (!HoverTarget && (bOverNodeTitle || Hit.bPinExpansion)) HoverTarget = HoveredNode;
			bool bShowHoverTooltip = false;
			if (bHovered && HoverTarget && DetailLevel != EMaterialGraphDetailLevel::Overview
				&& std::holds_alternative<FIdleInteraction>(Interaction)
				&& !ImGui::IsAnyMouseDown() && ImGui::GetIO().MouseWheel == 0.0f)
			{
				// Give each canvas hit target its own ImGui hover delay without adding a widget.
				ImGui::PushID(HoverTarget->View->Node.Id.ToString().c_str());
				ImGui::PushID(static_cast<int>(Hit.InputNode ? Hit.InputIndex : Hit.OutputIndex));
				const ImGuiID HoverId = ImGui::GetID(Hit.InputNode ? "##InputTooltip"
					: Hit.OutputNode ? "##OutputTooltip" : Hit.bPinExpansion ? "##PinExpansionTooltip" : "##NodeTooltip");
				ImGui::PopID();
				ImGui::PopID();
				auto& LastItem = ImGui::GetCurrentContext()->LastItemData;
				const ImGuiLastItemData PreviousItem = LastItem;
				ImGui::ItemAdd({CanvasMinimum, CanvasMaximum}, HoverId, nullptr, ImGuiItemFlags_NoNav);
				bShowHoverTooltip = IsGraphTooltipHovered();
				LastItem = PreviousItem;
			}
			for (const FVisualNode& Visual : VisualNodes)
			{
				if (!Intersects(Visual.Minimum, Visual.Maximum,
					CanvasMinimum, CanvasMaximum)) continue;
				const bool bSelected = SelectedNodes.contains(SelectionKey(Visual.View->Node.Id));
				DrawList->AddRectFilled(Visual.Minimum, Visual.Maximum,
					bSelected ? IM_COL32(55, 72, 94, GraphSelectedNodeBodyAlpha)
						: IM_COL32(42, 46, 54, GraphNodeBodyAlpha),
					6.0f);
				DrawList->AddRectFilled(Visual.Minimum,
					{Visual.Maximum.x, Visual.Minimum.y + GraphNodeHeaderHeight(*Visual.View) * Zoom},
					NodeTitleColor(Visual.View->Node.Opcode), 6.0f,
					IsHeaderOnlyGraphNode(*Visual.View) ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersTop);
				DrawList->AddRect(Visual.Minimum, Visual.Maximum,
					bSelected ? IM_COL32(90, 170, 245, 255) : IM_COL32(78, 84, 96, 255),
					6.0f, 0, bSelected ? 2.5f : 1.0f);
				DrawNodeHeading(Visual, *DrawList, Material);
				if (CollapsiblePinNodes.contains(Visual.View->Node.Id)
					&& DetailLevel != EMaterialGraphDetailLevel::Overview)
				{
					const bool bExpanded = ExpandedPinNodes.contains(Visual.View->Node.Id);
					const bool bArrowHovered = bHovered && Hit.Node == &Visual && Hit.bPinExpansion;
					const ImVec2 Center{(Visual.Minimum.x + Visual.Maximum.x) * 0.5f,
						Visual.Maximum.y - PinExpansionHeight * 0.5f * Zoom};
					const float Direction = bExpanded ? -1.0f : 1.0f;
					const ImU32 Color = bArrowHovered ? IM_COL32(235, 240, 250, 255) : IM_COL32(155, 165, 180, 255);
					const ImVec2 Tip{Center.x, Center.y + 2.5f * Zoom * Direction};
					DrawList->AddLine({Center.x - 5.0f * Zoom, Center.y - 2.5f * Zoom * Direction}, Tip, Color, 1.5f);
					DrawList->AddLine(Tip, {Center.x + 5.0f * Zoom, Center.y - 2.5f * Zoom * Direction}, Color, 1.5f);
					if (bArrowHovered)
					{
						ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
						if (bShowHoverTooltip) ImGui::SetTooltip(bExpanded ? "Collapse extra pins" : "Expand extra pins");
					}
				}
				const float PinRadius = std::max(2.0f, 5.0f * Zoom);
				const bool bTexturePreview = HasGraphTexturePreview(*Visual.View);
				const float PreviewLeft = Visual.Minimum.x + NodePadding * Zoom;
				if (bTexturePreview && DetailLevel != EMaterialGraphDetailLevel::Overview)
					DrawTexturePreview(Visual.View->Node.Id, {PreviewLeft,
						Visual.Minimum.y + GraphTexturePreviewTop(*Visual.View) * Zoom}, GraphTexturePreviewSize * Zoom);
				const ImVec4 OutputTextClip{PreviewLeft + (GraphTexturePreviewSize + 4.0f) * Zoom,
					Visual.Minimum.y, Visual.Maximum.x, Visual.Maximum.y};
				const float FontScale = ImGui::GetFontSize() / GraphBodyFontSize;
				for (size_t Index = 0; Index < Visual.OutputPins.size(); ++Index)
					DrawList->AddCircleFilled(Visual.OutputPins[Index], PinRadius, TypeColor(Visual.View->Outputs[Index].Type));
				if (DetailLevel == EMaterialGraphDetailLevel::Editing)
				{
					for (size_t Index = 0; Index < Visual.OutputPins.size(); ++Index)
					{
						const float LabelWidth = GraphNodeWidth(*Visual.View) * Zoom * 0.45f;
						const std::string ResultLabel = Ellipsize(GraphOutputLabel(*Visual.View, Index), LabelWidth);
						const float ResultLabelWidth = ImGui::GetFont()->CalcTextSizeA(
							GraphBodyFontSize, FLT_MAX, 0.0f, ResultLabel.c_str()).x;
						DrawList->AddText(ImGui::GetFont(), GraphBodyFontSize,
							Add(Visual.OutputPins[Index],
								{-8.0f * Zoom - ResultLabelWidth,
									-GraphBodyFontSize * 0.5f}),
							IM_COL32(185, 190, 202, 255), ResultLabel.c_str(), nullptr, 0.0f,
							bTexturePreview ? &OutputTextClip : nullptr);
					}
				}
				for (size_t Index = 0; Index < Visual.InputPins.size(); ++Index)
				{
					DrawList->AddCircleFilled(Visual.InputPins[Index], PinRadius,
						Visual.View->Inputs[Index].bActive ? TypeColor(Visual.View->Inputs[Index].SourceType) : IM_COL32(90, 95, 105, 255));
					if (DetailLevel == EMaterialGraphDetailLevel::Editing)
					{
						const float LabelWidth = (GraphNodeWidth(*Visual.View)
							- (Index < Visual.OutputPins.size() && !GraphOutputLabel(*Visual.View, Index).empty() ? 85.f : 20.f)) * Zoom;
						const auto Label = Ellipsize(InputLabel(*Visual.View, Visual.View->Inputs[Index], Material), LabelWidth * FontScale);
						DrawList->AddText(ImGui::GetFont(), GraphBodyFontSize,
							Add(Visual.InputPins[Index],
								{9.0f * Zoom, -GraphBodyFontSize * 0.5f}),
							Visual.View->Inputs[Index].bActive ? IM_COL32(205, 210, 220, 255) : IM_COL32(100, 105, 115, 255),
							Label.c_str());
					}
					if (bHovered && (Linking || bShowHoverTooltip) && Hit.InputNode == &Visual && Hit.InputIndex == Index)
					{
						const auto& Pin = Visual.View->Inputs[Index];
						if (Linking)
						{
							const auto Response = EditorGraph->GetSchema().CanCreateConnection(
								EditorGraph->FindPin(FMaterialGraphPinAddress::Output(LinkSource)), EditorGraph->FindPin(Visual.View->InputAddress(Pin)));
							const bool Accepted = Response.Allows(ImGui::GetIO().KeyShift);
							DrawList->AddCircle(Visual.InputPins[Index], 8.0f, Accepted ? IM_COL32(90, 220, 125, 230) : IM_COL32(235, 90, 90, 230));
							const std::string_view Hint = Accepted ? "Connect" : Response.Reason;
							ImGui::SetTooltip("%.*s", static_cast<int>(Hint.size()), Hint.data());
						}
						else if (!Pin.bActive) ImGui::SetTooltip("Inactive for the current shading/blend settings. Its connection and default are retained.");
						else
						{
							std::string Default;
							if (Pin.InlineDefault.Kind == EMaterialInputDefaultKind::Literal)
								Default = "\n" + FormatGraphNumericValue(Pin.InlineDefault.Type, Pin.InlineDefault.Literal, 9);
							else if (Pin.Default.Kind != EMaterialFunctionDefaultKind::None)
								Default = "\n" + DescribeFunctionDefault(Pin.Default);
							const auto Constraint = ConstraintHint(
								Pin.AcceptedTypes.empty() ? Pin.SourceType : Pin.AcceptedTypes.front(), Pin.Constraint);
							ImGui::SetTooltip("%s%s%s%s%s%s", Pin.Name.c_str(), Pin.bRequired ? " (required)" : "",
								Default.c_str(), BroadcastHint(Pin), Constraint.empty() ? "" : "\n", Constraint.c_str());
						}
					}
				}

			}
			const auto* Reconnect = std::get_if<FReconnectingInputInteraction>(&Interaction);
			if (bHovered && (Reconnect || bShowHoverTooltip) && Hit.OutputNode)
			{
				const auto& Pin = Hit.OutputNode->View->Outputs[Hit.OutputIndex];
				if (Reconnect)
				{
					const auto Address = FMaterialGraphPinAddress::Output({Hit.OutputNode->View->Node.Id, Pin.OutputIndex, Pin.PortId});
					const auto Response = EditorGraph->GetSchema().CanCreateConnection(EditorGraph->FindPin(Reconnect->Destination), EditorGraph->FindPin(Address));
					const std::string_view Hint = Response.Allows(true) ? "Connect" : Response.Reason;
					ImGui::SetTooltip("%.*s", static_cast<int>(Hint.size()), Hint.data());
				}
				else
				{
					const char* Type = GetProgramTypeName(Pin.Type);
					const bool bNamedPin = !Pin.Name.empty() && Pin.Name != Type;
				const auto Constraint = ConstraintHint(Pin.Type, Pin.Constraint);
					if (Pin.Constraint.Mode == EMaterialFunctionValueConstraintMode::Exact)
					ImGui::SetTooltip("%s%s%s", bNamedPin ? Pin.Name.c_str() : "", bNamedPin ? "\n" : "", Constraint.c_str());
					else if (bNamedPin)
					ImGui::SetTooltip("%s (%s)%s%s", Pin.Name.c_str(), Type, Constraint.empty() ? "" : "\n", Constraint.c_str());
					else
					ImGui::SetTooltip("%s%s%s", Type, Constraint.empty() ? "" : "\n", Constraint.c_str());
				}
			}
			if (bShowHoverTooltip && bOverNodeTitle && !Hit.InputNode && !Hit.OutputNode && !Hit.bPinExpansion)
			{
				ImGui::BeginTooltip();
				const auto& Node = *HoveredNode->View;
				const auto Display = MakeGraphNodeDisplay(Node, Material);
				const bool bConstant = !Node.Node.bMaterialOutput && Node.Node.Opcode == EMaterialProgramOpcode::Constant;
				if (!bConstant) ImGui::TextUnformatted(Display.Title.c_str());
				if (Display.Value)
					ImGui::TextUnformatted(FormatGraphNumericValue(Node.Node.ResultType, *Display.Value, 9).c_str());
				if ((bConstant || !Display.Value) && !Display.Subtitle.empty() && Display.Subtitle != Display.Title)
					ImGui::TextDisabled("%s", Display.Subtitle.c_str());
				if (!bConstant && Node.Node.Opcode != EMaterialProgramOpcode::FunctionInput && Node.Outputs.size() == 1)
					ImGui::TextDisabled("Output: %s", GetProgramTypeName(Node.Outputs.front().Type));
				ImGui::EndTooltip();
			}
			if (OpenFunction && bHovered && HoveredNode && !Hit.bPinExpansion && !Hit.InputNode && !Hit.OutputNode
				&& std::holds_alternative<FIdleInteraction>(Interaction)
				&& ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
				&& !HoveredNode->View->FunctionPath.empty())
				OpenFunction(HoveredNode->View->FunctionPath);
			HandlePointerInput(Owner, Transactions, View, VisualGraph, CanvasMinimum,
				CanvasMaximum, CanvasSize, Mouse, bHovered);
			if (bFrameSelectionRequested)
				FrameNodes(View, CanvasSize, EFrameScope::Selection);
			if (bFrameAllRequested)
				FrameNodes(View, CanvasSize, EFrameScope::All);
			if (PendingFrameNode.IsValid())
			{
				SelectedNodes = {SelectionKey(PendingFrameNode)};
				FrameNodes(View, CanvasSize, EFrameScope::Selection);
				PendingFrameNode = {};
			}

			DetailLevel = FMaterialGraphGeometry::SelectDetailLevel(Zoom, DetailLevel);

			DrawContextMenu(Owner, Transactions, View);
			DrawCreationMenu(Owner, Transactions, View);

			DrawList->PopClipRect();
		}
		ImGui::EndChild();
		ImGui::PopID();
	}
}
