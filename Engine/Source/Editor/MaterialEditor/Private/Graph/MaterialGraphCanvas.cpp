#include "Graph/MaterialGraphNodeDisplay.h"
#include "Graph/MaterialGraphCreationShortcuts.h"
#include "Graph/MaterialGraphValueTypes.h"
#include "Graph/MaterialGraphCanvas.h"
#include "MaterialGraphDocument.h"
#include "MaterialGraphEditSession.h"
#include "MaterialGraphEditInternals.h"

#include "Transactions/Transaction.h"
#include "MonaImGui.h"
#include "Asset/Asset.h"
#include "Asset/AssetDragDrop.h"
#include "ThirdParty/ImGui/imgui_internal.h"

namespace Durin::Editor::Material
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
		constexpr float PinExpansionHeight = 20.0f;

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
		const float NodeHeaderHeight = Metrics.HeaderHeight;
		const float PinSpacing = Metrics.PinRowHeight;
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
			case Op::Constant: case Op::UVChannel: case Op::TextureCoordinates: case Op::WorldPosition: case Op::Time:
				return IM_COL32(44, 83, 126, 255);
			case Op::TextureSample2D: case Op::BlendNormalsRNM:
				return IM_COL32(114, 79, 43, 255);
			case Op::Swizzle: case Op::AppendVector: case Op::MakeFloat2: case Op::MakeFloat3: case Op::MakeFloat4:
			case Op::Splat2: case Op::Splat3: case Op::Splat4:
				return IM_COL32(91, 65, 122, 255);
			case Op::FunctionInput: case Op::FunctionOutput: case Op::FunctionCall:
				return IM_COL32(39, 99, 105, 255);
			case Op::MakeSurface: case Op::GetSurfaceAttributes: case Op::SetSurfaceAttributes:
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
			case EMaterialProgramValueType::Texture2D: return IM_COL32(150, 110, 230, 255);
			case EMaterialProgramValueType::Surface: return IM_COL32(235, 155, 70, 255);
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

	FMaterialGraphCanvas::FMaterialGraphCanvas(FMaterialGraphDocument InDocument, FMaterialGraphEditorServices InServices)
		: GraphDocument(std::move(InDocument)), Services(std::move(InServices)) {}
	FMaterialGraphCanvas::~FMaterialGraphCanvas() = default;

	auto FMaterialGraphCanvas::ReportError(std::string Message) const -> void
	{
		if (Services.ReportError) Services.ReportError(std::move(Message));
	}

	auto FMaterialGraphCanvas::CheckCommand(const FMaterialGraphCommandResult& Result) const -> bool
	{
		if (!Result.HasError()) return true;
		ReportError(FormatMaterialGraphCommandResult(Result));
		return false;
	}

	auto FMaterialGraphCanvas::ClearSharedClipboard() -> void { GraphClipboard.reset(); }

	auto FMaterialGraphCanvas::SelectAndFrame(const FGuid& NodeId) -> bool
	{
		if (!NodeId.IsValid()) return false;
		SelectedSurfaceOutput.reset();
		SelectedNodes = {NodeId};
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
			if (Diagnostic.LocationIndex
				> static_cast<uint32>(EMaterialSurfaceOutput::OpacityMask)) return false;
			SelectedNodes = {Diagnostic.NodeId.IsValid() ? Diagnostic.NodeId : OutputNodeId};
			PendingFrameNode = Diagnostic.NodeId.IsValid() ? Diagnostic.NodeId : OutputNodeId;
			SelectedSurfaceOutput =
				static_cast<EMaterialSurfaceOutput>(Diagnostic.LocationIndex);
			return true;
		case EMaterialProgramDiagnosticLocationKind::Program:
			return false;
		}
		return false;
	}

	auto FMaterialGraphCanvas::CancelInteraction() -> void
	{
		if (ParameterSession.IsActive()) ParameterSession.Cancel();
		if (MoveSession.IsActive())
		{
			MoveSession.Cancel();
		}
		Interaction = FIdleInteraction{};
		bViewStale = true;
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
			Catalog = FMaterialGraphOperations::EnumerateCatalog();
			bCreationMenuResultsDirty = true;
			bViewStale = true;
		}
		bFunctionGraph = bFunction;
		const auto Changes = ReadModel.Refresh(Owner, Catalog);
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
		OutputNodeId = Material.GetOutputNode() ? Material.GetOutputNode()->Id : FGuid{};
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
			for (size_t Index = 0; Index < Visual.OutputPins.size(); ++Index)
				Visual.OutputPins[Index] = {Visual.OutputPin.x, Visual.OutputPin.y + PinSpacing * Index * Zoom};
			for (size_t Index = 0; Index < Node.Inputs.size(); ++Index)
				Visual.InputPins[Index] = {
					Visual.Minimum.x,
					Visual.Minimum.y + (GraphNodePinOffset(Node) + PinSpacing * Index) * Zoom};
		}
		return Result;
	}

	auto FMaterialGraphCanvas::GetSelectedProgramNodes() const -> std::vector<FGuid>
	{
		std::vector<FGuid> Nodes;
		for (const FMaterialGraphCanvasNodeId& Id : SelectedNodes)
			if (const auto* Node = std::get_if<FGuid>(&Id)) Nodes.push_back(*Node);
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
				&& !SelectedNodes.contains(Node.Node.Id)) continue;
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
		for (const FVisualNode& Destination : VisualGraph.Nodes)
			for (size_t InputIndex = 0;
				InputIndex < Destination.View->Inputs.size(); ++InputIndex)
			{
				const auto SourceIt = VisualGraph.Indices.find(
					Destination.View->Inputs[InputIndex].Link.SourceNodeId);
				if (SourceIt == VisualGraph.Indices.end()) continue;
				const ImVec2 A = VisualGraph.Nodes[SourceIt->second].OutputPosition(Destination.View->Inputs[InputIndex].Link);
				const ImVec2 B = Destination.InputPins[InputIndex];
				const bool bFocused = SelectedNodes.empty()
					|| SelectedNodes.contains(Destination.View->Node.Id)
					|| SelectedNodes.contains(
						VisualGraph.Nodes[SourceIt->second].View->Node.Id);
				const ImU32 Color = TypeColor(
					Destination.View->Inputs[InputIndex].SourceType);
				DrawCulledLink(DrawList, A, B, CanvasMinimum, CanvasMaximum,
					bFocused ? Color : WithAlpha(Color, 72),
					bFocused ? 3.0f : 1.5f);
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
				SelectedNodes.insert(Node.Node.Id);
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
		const FMaterialGraphCommandResult Copied =
			GraphDocument.CopySelection(NodeIds, Payload);
		if (CheckCommand(Copied)) GraphClipboard = std::move(Payload);
	}

	auto FMaterialGraphCanvas::CutNodes(
		DObject& Owner,
		DTransactor& Transactions,
		std::span<const FGuid> NodeIds) -> void
	{
		if (NodeIds.empty()) return;
		FMaterialGraphClipboardPayload Payload;
		const FMaterialGraphCommandResult Cut = GraphDocument.CutSelection(NodeIds, Payload, &Transactions);
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
		const auto& Document = GraphDocument;
		const auto Duplicated = Document.DuplicateNodes(NodeIds, 40, 40, &Transactions);
		if (!CheckCommand(Duplicated)) return;
		SelectedNodes.clear();
		SelectedNodes.insert(Duplicated.GeneratedNodeIds.begin(),
			Duplicated.GeneratedNodeIds.end());
	}

	auto FMaterialGraphCanvas::PasteNodes(
		DObject& Owner,
		DTransactor& Transactions,
		const ImVec2& GraphPosition) -> void
	{
		if (!GraphClipboard) return;
		const bool bRepeated = LastPasteAnchor && LastPasteAnchor->x == GraphPosition.x && LastPasteAnchor->y == GraphPosition.y;
		const uint32 Offset = bRepeated ? RepeatedPasteCount + 1 : 0;
		const FMaterialGraphCommandResult Pasted = GraphDocument.Paste(*GraphClipboard,
			static_cast<int32>(std::round(GraphPosition.x)) + 24 * Offset,
			static_cast<int32>(std::round(GraphPosition.y)) + 24 * Offset, &Transactions);
		if (!CheckCommand(Pasted)) return;
		LastPasteAnchor = GraphPosition;
		RepeatedPasteCount = Offset;
		SelectedSurfaceOutput.reset();
		SelectedNodes.clear();
		SelectedNodes.insert(Pasted.GeneratedNodeIds.begin(),
			Pasted.GeneratedNodeIds.end());
	}

	auto FMaterialGraphCanvas::RemoveNodes(
		DObject& Owner,
		DTransactor& Transactions,
		std::span<const FGuid> NodeIds) -> void
	{
		if (NodeIds.empty()) return;
		const FMaterialGraphCommandResult Removed =
			GraphDocument.RemoveNodes(NodeIds, &Transactions);
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
		const std::optional<EMaterialSurfaceOutput> ContextSurfaceOutput =
			Context ? Context->SurfaceOutput : std::nullopt;
		const auto ContextNodeIt = std::ranges::find(View.Nodes, ContextNode,
			[](const FMaterialGraphNodeView& Node) { return Node.Node.Id; });
		const FMaterialGraphNodeView* ContextNodeView =
			ContextNodeIt == View.Nodes.end() ? nullptr : &*ContextNodeIt;
		if (ContextNodeView && !ContextSurfaceOutput)
		{
			std::vector<FGuid> ContextSelection;
			if (SelectedNodes.contains(ContextNodeView->Node.Id))
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
						CheckCommand(GraphDocument.SetConstantValue(
							Edited.Id, MakeParameterValue(Type, Edited.GetConstantLiteral()), &Transactions));
					}
				}
				ImGui::EndMenu();
			}
			if (GraphDocument.GetSchema().CanOwnParameters() && SurfaceMaterial && !Edited.bMaterialOutput && Edited.Opcode == EMaterialProgramOpcode::Constant && ImGui::BeginMenu("Promote to Parameter"))
			{
				ImGui::InputTextWithHint("##ParameterName", "Parameter name", PromotionNameDraft.data(), PromotionNameDraft.size());
				if (ImGui::MenuItem("Create / Reuse"))
					CheckCommand(FMaterialGraphOperations::PromoteConstantToParameter(
						*SurfaceMaterial, Edited.Id, FName(PromotionNameDraft.data()), &Transactions));
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
					&& !GraphDocument.GetSchema().CanRemove(Node.Node.bMaterialOutput);
			});
			if (ImGui::MenuItem("Delete", nullptr, false, CanRemoveSelection))
				RemoveNodes(Owner, Transactions, ContextSelection);
		}
		else if (SurfaceMaterial && ContextSurfaceOutput)
		{
			if (static_cast<size_t>(*ContextSurfaceOutput) == 8)
			{
				if (ImGui::MenuItem("Disconnect Surface"))
					CheckCommand(FMaterialGraphDocument(*SurfaceMaterial).Disconnect(FMaterialGraphPinAddress::MaterialOutput((*SurfaceMaterial).GetOutputNode()->Id), &Transactions));
			}
			else
			{
				const FMaterialProgramLink& Link = GetMaterialSurfaceOutputLink(
					View.Outputs, *ContextSurfaceOutput);
				const ImVec2 SurfacePosition = ImVec2(static_cast<float>(ContextNodeView->Presentation.X), static_cast<float>(ContextNodeView->Presentation.Y));
				const FMaterialGraphSurfaceNodeRequest NodeRequest{
					.Output = *ContextSurfaceOutput,
					.X = static_cast<int32>(std::round(SurfacePosition.x
						- Metrics.NodeWidth - Metrics.ColumnGap)),
					.Y = static_cast<int32>(std::round(SurfacePosition.y)),
				};
				if (Link.SourceNodeId.IsValid()
					&& ImGui::MenuItem("Disconnect to Default"))
					CheckCommand(FMaterialGraphDocument(*SurfaceMaterial).Disconnect(FMaterialGraphPinAddress::MaterialOutput((*SurfaceMaterial).GetOutputNode()->Id, *ContextSurfaceOutput), &Transactions));
				if (ImGui::MenuItem("Reset Default"))
					CheckCommand(FMaterialGraphOperations::ResetSurfaceDefault(
						*SurfaceMaterial, *ContextSurfaceOutput, &Transactions));
				if (!Link.SourceNodeId.IsValid()
					&& ImGui::MenuItem("Promote to Parameter"))
					CheckCommand(
						FMaterialGraphOperations::PromoteSurfaceOutputToParameter(
							*SurfaceMaterial, NodeRequest, &Transactions));
				if (ImGui::MenuItem("Add Texture"))
					CheckCommand(FMaterialGraphOperations::AddTextureToSurfaceOutput(
						*SurfaceMaterial, NodeRequest, &Transactions));
			}
		}
		ImGui::EndPopup();
	}

	auto FMaterialGraphCanvas::DrawNodeHeading(const FVisualNode& Visual, ImDrawList& DrawList,
		const DMaterial* Material) const -> void
	{
		const auto Display = MakeGraphNodeDisplay(*Visual.View, Material);
		const float NodeWidth = GraphNodeWidth(*Visual.View);
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
			if ((DetailLevel == EMaterialGraphDetailLevel::Editing || Display.Value)
				&& !Display.Subtitle.empty()
				&& !IsHeaderOnlyGraphNode(*Visual.View) && !IsCompactGraphOperation(*Visual.View))
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
			const auto Created = GraphDocument.Create({MakeCreationAction(*Entry),
				static_cast<int32>(std::round(Position.x)), static_cast<int32>(std::round(Position.y))}, &Transactions);
			if (CheckCommand(Created))
			{
				SelectedNodes.clear();
				SelectedSurfaceOutput.reset();
				SelectedNodes.insert(Created.GeneratedNodeIds.begin(), Created.GeneratedNodeIds.end());
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
				for (size_t Index = 0; Index < Visual.InputPins.size(); ++Index)
					if (std::hypot(Mouse.x - Visual.InputPins[Index].x, Mouse.y - Visual.InputPins[Index].y) <= 8.0f)
					{ Hit.InputNode = &Visual; Hit.InputIndex = static_cast<uint32>(Index); break; }
				for (size_t Index = 0; Index < Visual.OutputPins.size(); ++Index)
					if (std::hypot(Mouse.x - Visual.OutputPins[Index].x, Mouse.y - Visual.OutputPins[Index].y) <= 8.0f)
					{ Hit.OutputNode = &Visual; Hit.OutputIndex = Index; break; }
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
					const auto Created = GraphDocument.Create({MakeFunctionCreationAction(Path.ToString()),
						static_cast<int32>(std::round(Position.x)), static_cast<int32>(std::round(Position.y))}, &Transactions);
					if (CheckCommand(Created))
					{
						SelectedSurfaceOutput.reset();
						SelectedNodes = {Created.GeneratedNodeIds.front()};
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
		const auto ConnectPin = [&](const FGuid& NodeId, uint32 PinIndex, FMaterialProgramLink Source, bool bReplace) {
			const auto Node = std::ranges::find(View.Nodes, NodeId, [](const auto& Item) { return Item.Node.Id; });
			if (Node == View.Nodes.end() || PinIndex >= Node->Inputs.size())
				return GraphEditInternals::RejectCommand("The graph input is unavailable.");
			const auto& Pin = Node->Inputs[PinIndex];
			const auto& Document = GraphDocument;
			return Document.Connect(Node->InputAddress(Pin),
				FMaterialGraphPinAddress::Output(Source), bReplace, &Transactions);
		};
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
			if (MoveSession.IsActive())
			{
				CheckCommand(MoveSession.Cancel());
			}
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
					bRemovedFromSelection = SelectedNodes.erase(Id) != 0;
					if (!bRemovedFromSelection) SelectedNodes.insert(Id);
				}
				else if (!SelectedNodes.contains(Id)) SelectedNodes = {Id};
				if (!bRemovedFromSelection)
				{
					const std::vector<FGuid> Selection = GetSelectedProgramNodes();
					const FMaterialGraphCommandResult Begun = MoveSession.Begin(Owner, Selection, &Transactions);
					if (CheckCommand(Begun))
					{
						FMovingInteraction Moving{.StartMouse = Mouse};
						for (const FVisualNode& Visual : VisualNodes)
							if (SelectedNodes.contains(Visual.View->Node.Id))
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

		if (const auto* Moving = std::get_if<FMovingInteraction>(&Interaction))
		{
			if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
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
					if (CheckCommand(Applied))
						for (const auto& Position : MoveSession.GetPositions())
							if (const auto It = CachedNodeIndices.find(Position.NodeId); It != CachedNodeIndices.end())
							{
								auto& Target = CachedView.Nodes[It->second].Presentation;
								Target.X = Position.X; Target.Y = Position.Y;
							}
				}
			}
			if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
			{
				CheckCommand(MoveSession.Commit());
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
						SelectedNodes.insert(Visual.View->Node.Id);
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
					CheckCommand(ConnectPin(HoveredInputNode->View->Node.Id, HoveredInputIndex,
						SourceLink, ImGui::GetIO().KeyShift));

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
			const auto DestinationIt = VisualIndices.find(Address.NodeId);
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
					const auto& Document = GraphDocument;
					CheckCommand(Document.Connect(Address,
						FMaterialGraphPinAddress::Output(Source), true, &Transactions));
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
					.SurfaceOutput = HoveredInputNode && HoveredInputNode->View->Node.bMaterialOutput ? std::optional{static_cast<EMaterialSurfaceOutput>(HoveredInputNode->View->Inputs[HoveredInputIndex].InputIndex)} : std::nullopt};
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
		if (!GraphDocument.GetOwner()) { CancelInteraction(); return; }
		auto& Owner = *GraphDocument.GetOwner();
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
				const FMaterialGraphCommandResult Layout = GraphDocument.Layout({}, &Transactions);
				CheckCommand(Layout);
			}
			ImGui::SameLine();
			const char* DetailName = DetailLevel == EMaterialGraphDetailLevel::Overview
				? "Overview" : DetailLevel == EMaterialGraphDetailLevel::Editing
					? "Editing" : "Readable";
			ImGui::TextDisabled("%s", DetailName);
			ImGui::SameLine();
			ImGui::TextDisabled("(?)");
			if (ImGui::IsItemHovered())
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
					|| CompileStatus.State == EMaterialCompileState::Deferred
					|| CompileStatus.State == EMaterialCompileState::Pending
					|| CompileStatus.State == EMaterialCompileState::Running
					|| CompileStatus.State == EMaterialCompileState::Failed
					|| CompileStatus.State == EMaterialCompileState::Rejected)
				{
					const bool bFailed = CompileStatus.State == EMaterialCompileState::Failed
						|| CompileStatus.State == EMaterialCompileState::Rejected;
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
			std::optional<EMaterialProgramValueType> LinkSourceType;

			if (const auto It = VisualIndices.find(LinkSourceNode);
				It != VisualIndices.end())
				if (!VisualNodes[It->second].View->Outputs.empty())
					LinkSourceType = VisualNodes[It->second].View->Outputs[VisualNodes[It->second].OutputIndex(LinkSource)].Type;
			for (const FVisualNode& Visual : VisualNodes)
			{
				if (!Intersects(Visual.Minimum, Visual.Maximum,
					CanvasMinimum, CanvasMaximum)) continue;
				const bool bSelected = SelectedNodes.contains(Visual.View->Node.Id);
				DrawList->AddRectFilled(Visual.Minimum, Visual.Maximum,
					bSelected ? IM_COL32(55, 72, 94, GraphSelectedNodeBodyAlpha)
						: IM_COL32(42, 46, 54, GraphNodeBodyAlpha),
					6.0f);
				DrawList->AddRectFilled(Visual.Minimum,
					{Visual.Maximum.x, Visual.Minimum.y + NodeHeaderHeight * Zoom},
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
						ImGui::SetTooltip(bExpanded ? "Collapse extra pins" : "Expand extra pins");
					}
				}
				const float PinRadius = std::max(2.0f, 5.0f * Zoom);
				if (DetailLevel != EMaterialGraphDetailLevel::Overview
					&& (Visual.View->Node.Opcode == EMaterialProgramOpcode::TextureParameter
						|| Visual.View->Node.Opcode == EMaterialProgramOpcode::TextureSampleParameter2D))
					DrawTexturePreview(Visual.View->Node.Id, Add(Visual.Minimum,
						{12 * Zoom, (NodeHeaderHeight + Metrics.SecondaryHeight + PinSpacing * 1.5f) * Zoom}), 80 * Zoom);
				for (size_t Index = 0; Index < Visual.OutputPins.size(); ++Index)
					DrawList->AddCircleFilled(Visual.OutputPins[Index], PinRadius, TypeColor(Visual.View->Outputs[Index].Type));
				if (DetailLevel == EMaterialGraphDetailLevel::Editing)
				{
					for (size_t Index = 0; Index < Visual.OutputPins.size(); ++Index)
					{
						const std::string ResultLabel = Ellipsize(GraphOutputLabel(*Visual.View, Index), GraphNodeWidth(*Visual.View) * Zoom * 0.45f);
						const float ResultLabelWidth = ImGui::GetFont()->CalcTextSizeA(
							GraphBodyFontSize, FLT_MAX, 0.0f, ResultLabel.c_str()).x;
						DrawList->AddText(ImGui::GetFont(), GraphBodyFontSize,
							Add(Visual.OutputPins[Index],
								{-8.0f * Zoom - ResultLabelWidth,
									-GraphBodyFontSize * 0.5f}),
							IM_COL32(185, 190, 202, 255), ResultLabel.c_str());
					}
				}
				for (size_t Index = 0; Index < Visual.InputPins.size(); ++Index)
				{
					DrawList->AddCircleFilled(Visual.InputPins[Index], PinRadius,
						Visual.View->Inputs[Index].bActive ? TypeColor(Visual.View->Inputs[Index].SourceType) : IM_COL32(90, 95, 105, 255));
					if (DetailLevel == EMaterialGraphDetailLevel::Editing)
						DrawList->AddText(ImGui::GetFont(), GraphBodyFontSize,
							Add(Visual.InputPins[Index],
								{9.0f * Zoom, -GraphBodyFontSize * 0.5f}),
							Visual.View->Inputs[Index].bActive ? IM_COL32(205, 210, 220, 255) : IM_COL32(100, 105, 115, 255),
							Ellipsize(InputLabel(*Visual.View, Visual.View->Inputs[Index], Material),
								(GraphNodeWidth(*Visual.View) - (Index < Visual.OutputPins.size() && !GraphOutputLabel(*Visual.View, Index).empty() ? 85.f : 20.f)) * Zoom
									* ImGui::GetFontSize() / GraphBodyFontSize).c_str());
					if (LinkSourceType)
					{
						const bool bAccepted = GraphDocument.GetSchema().IsConnectionTypeCompatible(Visual.View->Inputs[Index],
							*LinkSourceType);
						DrawList->AddCircle(Visual.InputPins[Index], 8.0f,
							bAccepted ? IM_COL32(90, 220, 125, 230)
								: IM_COL32(235, 90, 90, 230), 0, 1.5f);
					}
					if (Hit.InputNode == &Visual && Hit.InputIndex == Index)
					{
						const auto& Pin = Visual.View->Inputs[Index];
						if (!Pin.bActive) ImGui::SetTooltip("Inactive for the current shading/blend settings. Its connection and default are retained.");
						else ImGui::SetTooltip("%s%s%s\n%s%s", Pin.Name.c_str(), Pin.bRequired ? " (required)" : "",
							Pin.bMissing ? " (missing port)" : "", (Pin.InlineDefault.Kind == EMaterialInputDefaultKind::Literal
							? FormatGraphNumericValue(Pin.InlineDefault.Type, Pin.InlineDefault.Literal, 9)
							: DescribeFunctionDefault(Pin.Default)).c_str(), BroadcastHint(Pin));
					}
				}

			}
			if (Hit.OutputNode && DetailLevel != EMaterialGraphDetailLevel::Overview)
			{
				const auto& Pin = Hit.OutputNode->View->Outputs[Hit.OutputIndex];
				ImGui::SetTooltip("%s (%s)", Pin.Name.c_str(), GetProgramTypeName(Pin.Type));
			}
			if (HoveredNode && !Hit.bPinExpansion && !Hit.InputNode && !Hit.OutputNode && DetailLevel != EMaterialGraphDetailLevel::Overview)
			{
				ImGui::BeginTooltip();
				const auto Display = MakeGraphNodeDisplay(*HoveredNode->View, Material);
				ImGui::TextUnformatted(Display.Title.c_str());
				if (Display.Value) ImGui::TextUnformatted(FormatGraphNumericValue(HoveredNode->View->Node.ResultType, *Display.Value, 9).c_str());
				if (!Display.Subtitle.empty())
					ImGui::TextDisabled("%s", Display.Subtitle.c_str());
				ImGui::TextDisabled("Output: %s", GetProgramTypeName(HoveredNode->View->Node.ResultType));
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
				SelectedNodes = {PendingFrameNode};
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
