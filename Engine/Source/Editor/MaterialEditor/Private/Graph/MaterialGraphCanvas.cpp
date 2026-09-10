#include "Graph/MaterialGraphControls.h"
#include "Graph/MaterialGraphValueTypes.h"
#include "Graph/MaterialGraphCanvas.h"

#include "Editor/Transaction.h"
#include "MonaImGui.h"

namespace Durin::Editor::Material
{
	namespace
	{
		std::optional<FMaterialGraphClipboardPayload> GraphClipboard;

		const auto& Metrics = FMaterialGraphGeometry::GetMetrics();
		const float NodeWidth = Metrics.NodeWidth;
		const float NodeHeaderHeight = Metrics.HeaderHeight;
		const float PinSpacing = Metrics.PinRowHeight;
		const float NodePadding = Metrics.BodyPadding;
		constexpr float GraphBodyFontHeight = 14.0f;
		constexpr float GraphTitleFontHeight = 16.0f;
		constexpr float GraphSecondaryFontHeight = 13.0f;
		constexpr float GraphControlHorizontalPadding = 3.0f;
		constexpr float GraphControlVerticalPadding = 2.0f;

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

		auto SurfaceLinks(const FMaterialSurfaceOutputs& Outputs)
			-> std::array<const FMaterialProgramLink*, 9>
		{
			return {&Outputs.BaseColor, &Outputs.Normal, &Outputs.Metallic,
				&Outputs.Roughness, &Outputs.AmbientOcclusion, &Outputs.Emissive,
				&Outputs.Opacity, &Outputs.OpacityMask, &Outputs.Surface};
		}

		auto SurfaceGraphMinimum(const FMaterialGraphView& View) -> ImVec2
		{
			bool bFound = false;
			float MaximumX = 0.0f;
			float MinimumY = 0.0f;
			float MaximumY = 0.0f;
			for (const FMaterialGraphNodeView& Node : View.Nodes)
			{
				const float Y = static_cast<float>(Node.Presentation.Y);
				const float Height = FMaterialGraphGeometry::GetNodeHeight(
					static_cast<uint32>(Node.Inputs.size()));
				MaximumX = std::max(MaximumX,
					static_cast<float>(Node.Presentation.X) + Metrics.NodeWidth);
				if (!bFound) { MinimumY = Y; MaximumY = Y + Height; bFound = true; }
				else { MinimumY = std::min(MinimumY, Y); MaximumY = std::max(MaximumY, Y + Height); }
			}
			const float Height = Metrics.SurfaceHeaderHeight
				+ Metrics.PinRowHeight
					* (View.Outputs.Surface.SourceNodeId.IsValid() ? 1.0f : 8.0f)
				+ Metrics.BodyPadding;
			return {MaximumX + Metrics.ColumnGap,
				bFound ? (MinimumY + MaximumY - Height) * 0.5f : 0.0f};
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
		std::vector<ImVec2> InputPins;
	};

	struct FMaterialGraphCanvas::FVisualGraph
	{
		std::vector<FVisualNode> Nodes;
		std::unordered_map<FGuid, size_t> Indices;
	};

	FMaterialGraphCanvas::FMaterialGraphCanvas() = default;
	FMaterialGraphCanvas::~FMaterialGraphCanvas() = default;

	auto FMaterialGraphCanvas::SelectAndFrame(const FGuid& NodeId) -> bool
	{
		if (!NodeId.IsValid()) return false;
		SelectedSurfaceOutput.reset();
		bPendingFrameSurface = false;
		SelectedNodes = {NodeId};
		PendingFrameNode = NodeId;
		return true;
	}

	auto FMaterialGraphCanvas::SelectAndFrameDiagnostic(
		const FMaterialProgramDiagnostic& Diagnostic) -> bool
	{
		switch (Diagnostic.LocationKind)
		{
		case EMaterialProgramDiagnosticLocationKind::Node:
		case EMaterialProgramDiagnosticLocationKind::Input:
			SelectedSurfaceOutput.reset();
			return SelectAndFrame(Diagnostic.NodeId);
		case EMaterialProgramDiagnosticLocationKind::SurfaceOutput:
			if (Diagnostic.LocationIndex
				> static_cast<uint32>(EMaterialSurfaceOutput::OpacityMask)) return false;
			SelectedNodes = {EMaterialGraphTerminal::MaterialOutput};
			PendingFrameNode = {};
			SelectedSurfaceOutput =
				static_cast<EMaterialSurfaceOutput>(Diagnostic.LocationIndex);
			bPendingFrameSurface = true;
			return true;
		case EMaterialProgramDiagnosticLocationKind::Program:
			return false;
		}
		return false;
	}

	auto FMaterialGraphCanvas::CancelInteraction() -> void
	{
		if (ParameterEditSession.IsActive()) ParameterEditSession.Cancel();
		if (MoveSession.IsActive())
		{
			MoveSession.Cancel();
			SurfaceGraphPosition.reset();
		}
		Interaction = FIdleInteraction{};
		bSurfaceDefaultDraftInitialized.fill(false);
	}

	auto FMaterialGraphCanvas::ResetInteraction() -> void
	{
		Interaction = FIdleInteraction{};
	}

	auto FMaterialGraphCanvas::PrepareView(DMaterial& Material)
		-> const FMaterialGraphView&
	{
		const uint64 ProgramRevision = Material.GetMaterialProgramRevision();
		const uint64 PresentationRevision =
			Material.GetMaterialGraphPresentationRevision();
		const uint64 SchemaRevision =
			Material.GetParameterDefinitionSchemaRevision();
		if (Catalog.empty())
		{
			Catalog = FMaterialGraphOperations::EnumerateCatalog();
			++CatalogRevision;
		}
		if (CachedMaterial != &Material
			|| CachedProgramRevision != ProgramRevision)
		{
			CachedView = FMaterialGraphOperations::Inspect(Material, Catalog);
			CachedNodeIndices.clear();
			CachedNodeIndices.reserve(CachedView.Nodes.size());
			for (size_t Index = 0; Index < CachedView.Nodes.size(); ++Index)
				CachedNodeIndices.emplace(CachedView.Nodes[Index].Node.Id, Index);
			CachedMaterial = &Material;
			CachedProgramRevision = ProgramRevision;
			CachedPresentationRevision = PresentationRevision;
			CachedSchemaRevision = SchemaRevision;
			bVisualGraphTopologyStale = true;
		}
		if (CachedSchemaRevision != SchemaRevision)
		{
			for (FMaterialGraphNodeView& Node : CachedView.Nodes)
			{
				Node.SecondaryLabel = Node.Node.DisplayName;
				if (Node.Node.ParameterId.IsValid())
					if (const auto* Definition = Material.FindParameterDefinition(Node.Node.ParameterId))
						Node.SecondaryLabel = Definition->DisplayName;
			}
			CachedSchemaRevision = SchemaRevision;
		}
		if (CachedPresentationRevision != PresentationRevision)
		{
			const auto& Presentation = Material.GetMaterialGraphPresentation();
			for (const auto& Position : Presentation.Nodes)
			{
				const auto It = CachedNodeIndices.find(Position.NodeId);
				check(It != CachedNodeIndices.end());
				CachedView.Nodes[It->second].Presentation = Position;
			}
			CachedView.MaterialOutputPosition = {
				Presentation.MaterialOutputX, Presentation.MaterialOutputY};
			CachedPresentationRevision = PresentationRevision;
		}
		SurfaceGraphPosition = {
			static_cast<float>(CachedView.MaterialOutputPosition.first),
			static_cast<float>(CachedView.MaterialOutputPosition.second)};
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
			const float NodeHeight = FMaterialGraphGeometry::GetNodeHeight(
				static_cast<uint32>(Node.Inputs.size()));
			Visual.Minimum = Add(CanvasMinimum, Add(Pan, Multiply(GraphPosition, Zoom)));
			Visual.Maximum = Add(Visual.Minimum,
				Multiply({NodeWidth, NodeHeight}, Zoom));
			Visual.OutputPin = {
				Visual.Maximum.x,
				Visual.Minimum.y + (NodeHeaderHeight + Metrics.SecondaryHeight
					+ NodePadding) * Zoom};
			for (size_t Index = 0; Index < Node.Inputs.size(); ++Index)
				Visual.InputPins[Index] = {
					Visual.Minimum.x,
					Visual.Minimum.y + (NodeHeaderHeight + Metrics.SecondaryHeight
						+ NodePadding + PinSpacing * Index) * Zoom};
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
			const float Height = FMaterialGraphGeometry::GetNodeHeight(
				static_cast<uint32>(Node.Inputs.size()));
			if (!bFound)
			{
				Minimum = Position;
				Maximum = Add(Position, {NodeWidth, Height});
				bFound = true;
			}
			else
			{
				Minimum.x = std::min(Minimum.x, Position.x);
				Minimum.y = std::min(Minimum.y, Position.y);
				Maximum.x = std::max(Maximum.x, Position.x + NodeWidth);
				Maximum.y = std::max(Maximum.y, Position.y + Height);
			}
		}
		if (Scope == EFrameScope::All
			|| SelectedNodes.contains(EMaterialGraphTerminal::MaterialOutput))
		{
			const ImVec2 SurfaceMinimum = SurfaceGraphPosition.value_or(
				SurfaceGraphMinimum(View));
			const ImVec2 SurfaceMaximum = Add(SurfaceMinimum,
				{Metrics.SurfaceWidth, Metrics.SurfaceHeaderHeight
					+ Metrics.PinRowHeight
						* (View.Outputs.Surface.SourceNodeId.IsValid() ? 1.0f : 8.0f)
					+ Metrics.BodyPadding});
			if (!bFound) { Minimum = SurfaceMinimum; Maximum = SurfaceMaximum; bFound = true; }
			else
			{
				Minimum.x = std::min(Minimum.x, SurfaceMinimum.x);
				Minimum.y = std::min(Minimum.y, SurfaceMinimum.y);
				Maximum.x = std::max(Maximum.x, SurfaceMaximum.x);
				Maximum.y = std::max(Maximum.y, SurfaceMaximum.y);
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
				const ImVec2 A = VisualGraph.Nodes[SourceIt->second].OutputPin;
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
		DMaterial& Material,
		DTransactor& Transactions,
		const FMaterialGraphView& View,
		const ImVec2& CanvasMinimum,
		const ImVec2& CanvasSize,
		const ImVec2& Mouse,
		bool bInputAvailable,
		const FReportError& ReportError) -> void
	{
		if (!bInputAvailable) return;
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
			CopyNodes(Material, Selection, ReportError);
		}
		if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X) && !SelectedNodes.empty())
		{
			const std::vector<FGuid> Selection = GetSelectedProgramNodes();
			CutNodes(Material, Transactions, Selection, ReportError);
		}
		if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D) && !SelectedNodes.empty())
		{
			const std::vector<FGuid> Selection = GetSelectedProgramNodes();
			DuplicateNodes(Material, Transactions, Selection, ReportError);
		}
		if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V) && GraphClipboard)
		{
			const ImVec2 GraphPosition = Multiply(
				Subtract(Subtract(Mouse, CanvasMinimum), Pan), 1.0f / Zoom);
			PasteNodes(Material, Transactions, GraphPosition, ReportError);
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !SelectedNodes.empty())
		{
			const std::vector<FGuid> Selection = GetSelectedProgramNodes();
			RemoveNodes(Material, Transactions, Selection, ReportError);
		}
		if (ImGui::IsKeyPressed(ImGuiKey_F))
			FrameNodes(View, CanvasSize, SelectedNodes.empty()
				? EFrameScope::All : EFrameScope::Selection);
	}

	auto FMaterialGraphCanvas::CopyNodes(
		DMaterial& Material,
		std::span<const FGuid> NodeIds,
		const FReportError& ReportError) -> void
	{
		if (NodeIds.empty()) return;
		FMaterialGraphClipboardPayload Payload;
		const FMaterialGraphCommandResult Copied =
			FMaterialGraphOperations::CopySelection(Material, NodeIds, Payload);
		ReportCommand(Copied, ReportError);
		if (Copied) GraphClipboard = std::move(Payload);
	}

	auto FMaterialGraphCanvas::CutNodes(
		DMaterial& Material,
		DTransactor& Transactions,
		std::span<const FGuid> NodeIds,
		const FReportError& ReportError) -> void
	{
		if (NodeIds.empty()) return;
		FMaterialGraphClipboardPayload Payload;
		const FMaterialGraphCommandResult Cut = FMaterialGraphOperations::CutSelection(
			Material, NodeIds, Payload, &Transactions);
		ReportCommand(Cut, ReportError);
		if (!Cut) return;
		GraphClipboard = std::move(Payload);
		SelectedNodes.clear();
	}

	auto FMaterialGraphCanvas::DuplicateNodes(
		DMaterial& Material,
		DTransactor& Transactions,
		std::span<const FGuid> NodeIds,
		const FReportError& ReportError) -> void
	{
		if (NodeIds.empty()) return;
		const FMaterialGraphCommandResult Duplicated =
			FMaterialGraphOperations::DuplicateNodes(
				Material, NodeIds, 40, 40, &Transactions);
		ReportCommand(Duplicated, ReportError);
		if (!Duplicated) return;
		SelectedNodes.clear();
		SelectedNodes.insert(Duplicated.GeneratedNodeIds.begin(),
			Duplicated.GeneratedNodeIds.end());
	}

	auto FMaterialGraphCanvas::PasteNodes(
		DMaterial& Material,
		DTransactor& Transactions,
		const ImVec2& GraphPosition,
		const FReportError& ReportError) -> void
	{
		if (!GraphClipboard) return;
		const FMaterialGraphCommandResult Pasted = FMaterialGraphOperations::Paste(
			Material, *GraphClipboard,
			static_cast<int32>(std::round(GraphPosition.x)),
			static_cast<int32>(std::round(GraphPosition.y)), &Transactions);
		ReportCommand(Pasted, ReportError);
		if (!Pasted) return;
		SelectedNodes.clear();
		SelectedNodes.insert(Pasted.GeneratedNodeIds.begin(),
			Pasted.GeneratedNodeIds.end());
	}

	auto FMaterialGraphCanvas::RemoveNodes(
		DMaterial& Material,
		DTransactor& Transactions,
		std::span<const FGuid> NodeIds,
		const FReportError& ReportError) -> void
	{
		if (NodeIds.empty()) return;
		const FMaterialGraphCommandResult Removed =
			FMaterialGraphOperations::RemoveNodes(Material, NodeIds, &Transactions);
		ReportCommand(Removed, ReportError);
		if (Removed) SelectedNodes.clear();
	}

	auto FMaterialGraphCanvas::HasClipboard() const -> bool
	{
		return GraphClipboard.has_value();
	}

	auto FMaterialGraphCanvas::DrawContextMenu(
		DMaterial& Material,
		DTransactor& Transactions,
		const FMaterialGraphView& View,
		const FReportError& ReportError) -> void
	{
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
		if (ContextNodeView)
		{
			std::vector<FGuid> ContextSelection;
			if (SelectedNodes.contains(ContextNodeView->Node.Id))
				ContextSelection = GetSelectedProgramNodes();
			else ContextSelection = {ContextNodeView->Node.Id};
			FMaterialProgramNode Edited = ContextNodeView->Node;
			if ((Edited.Opcode == EMaterialProgramOpcode::Parameter
				|| Edited.Opcode == EMaterialProgramOpcode::TextureParameter) && ImGui::BeginMenu("Parameter"))
			{
				for (const auto& Definition : Material.GetParameterDefinitions())
				{
					if (GetProgramType(Definition.Type) != Edited.ResultType) continue;
					ImGui::PushID(Definition.Id.ToString().c_str());
					if (ImGui::MenuItem(Definition.Name.ToString().c_str()))
					{
						Edited.ParameterId = Definition.Id;
						Edited.DisplayName = Definition.DisplayName;
						ReportCommand(FMaterialGraphOperations::ReplaceNode(
							Material, Edited, &Transactions), ReportError);
					}
					ImGui::PopID();
				}
				ImGui::EndMenu();
			}
			if (Edited.Opcode == EMaterialProgramOpcode::Constant && ImGui::BeginMenu("Type"))
			{
				for (EMaterialProgramValueType Type : {EMaterialProgramValueType::Float,
					EMaterialProgramValueType::Float2, EMaterialProgramValueType::Float3,
					EMaterialProgramValueType::Float4})
				{
					if (ImGui::MenuItem(GetProgramTypeName(Type), nullptr, Edited.ResultType == Type))
					{
						Edited.ResultType = Type;
						ReportCommand(FMaterialGraphOperations::ReplaceNode(
							Material, Edited, &Transactions), ReportError);
					}
				}
				ImGui::EndMenu();
			}
			if (Edited.Opcode == EMaterialProgramOpcode::Constant && ImGui::BeginMenu("Promote to Parameter"))
			{
				ImGui::InputTextWithHint("##ParameterName", "Parameter name", PromotionNameDraft.data(), PromotionNameDraft.size());
				if (ImGui::MenuItem("Create / Reuse"))
					ReportCommand(FMaterialGraphOperations::PromoteConstantToParameter(
						Material, Edited.Id, FName(PromotionNameDraft.data()), &Transactions), ReportError);
				ImGui::EndMenu();
			}
			if (ImGui::MenuItem("Copy"))
				CopyNodes(Material, ContextSelection, ReportError);
			if (ImGui::MenuItem("Duplicate"))
				DuplicateNodes(Material, Transactions, ContextSelection, ReportError);
			if (ImGui::MenuItem("Cut"))
				CutNodes(Material, Transactions, ContextSelection, ReportError);
			if (ImGui::MenuItem("Delete"))
				RemoveNodes(Material, Transactions, ContextSelection, ReportError);
		}
		else if (ContextSurfaceOutput)
		{
			if (static_cast<size_t>(*ContextSurfaceOutput) == 8)
			{
				if (ImGui::MenuItem("Disconnect Surface"))
					ReportCommand(FMaterialGraphOperations::DisconnectAggregateSurface(
						Material, &Transactions), ReportError);
			}
			else
			{
				const FMaterialProgramLink& Link = GetMaterialSurfaceOutputLink(
					View.Outputs, *ContextSurfaceOutput);
				const ImVec2 SurfacePosition = SurfaceGraphPosition.value_or(
					SurfaceGraphMinimum(View));
				const FMaterialGraphSurfaceNodeRequest NodeRequest{
					.Output = *ContextSurfaceOutput,
					.X = static_cast<int32>(std::round(SurfacePosition.x
						- Metrics.NodeWidth - Metrics.ColumnGap)),
					.Y = static_cast<int32>(std::round(SurfacePosition.y)),
				};
				if (Link.SourceNodeId.IsValid()
					&& ImGui::MenuItem("Disconnect to Default"))
					ReportCommand(FMaterialGraphOperations::DisconnectSurfaceOutput(
						Material, *ContextSurfaceOutput, &Transactions), ReportError);
				if (ImGui::MenuItem("Reset Default"))
					ReportCommand(FMaterialGraphOperations::ResetSurfaceDefault(
						Material, *ContextSurfaceOutput, &Transactions), ReportError);
				if (!Link.SourceNodeId.IsValid()
					&& ImGui::MenuItem("Promote to Parameter"))
					ReportCommand(
						FMaterialGraphOperations::PromoteSurfaceOutputToParameter(
							Material, NodeRequest, &Transactions), ReportError);
				if (ImGui::MenuItem("Add Texture"))
					ReportCommand(FMaterialGraphOperations::AddTextureToSurfaceOutput(
						Material, NodeRequest, &Transactions), ReportError);
			}
		}
		ImGui::EndPopup();
	}

	auto FMaterialGraphCanvas::Draw(
		DMaterial& Material,
		DTransactor& Transactions,
		float Height,
		const FReportError& ReportError) -> void
	{
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
				const FMaterialGraphCommandResult Layout = FMaterialGraphOperations::Layout(
					Material, {}, &Transactions);
				ReportCommand(Layout, ReportError);
				if (Layout) SurfaceGraphPosition.reset();
			}
			ImGui::SameLine();
			const char* DetailName = DetailLevel == EMaterialGraphDetailLevel::Overview
				? "Overview" : DetailLevel == EMaterialGraphDetailLevel::Editing
					? "Editing" : "Readable";
			ImGui::TextDisabled("%s", DetailName);
			ImGui::SameLine();
			ImGui::TextDisabled("(?)");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Wheel: zoom\nMMB: pan\nLMB: select / drag\nShift: add selection / replace link");

			const FMaterialGraphView& View = PrepareView(Material);
			const ImVec2 CanvasMinimum = ImGui::GetCursorScreenPos();
			ImVec2 CanvasSize = ImGui::GetContentRegionAvail();
			CanvasSize.x = std::max(CanvasSize.x, 64.0f);
			CanvasSize.y = std::max(CanvasSize.y, 64.0f);
			const ImVec2 CanvasMaximum = Add(CanvasMinimum, CanvasSize);
			ImGui::SetNextItemAllowOverlap();
			ImGui::InvisibleButton("##Canvas", CanvasSize,
				ImGuiButtonFlags_MouseButtonLeft
					| ImGuiButtonFlags_MouseButtonMiddle
					| ImGuiButtonFlags_MouseButtonRight);
			const bool bHovered = ImGui::IsItemHovered();
			ImDrawList* DrawList = ImGui::GetWindowDrawList();
			DrawList->PushClipRect(CanvasMinimum, CanvasMaximum, true);
			DrawList->AddRectFilled(CanvasMinimum, CanvasMaximum,
				IM_COL32(24, 27, 32, 255));
			const FMaterialCompileStatus& CompileStatus = Material.GetMaterialCompileStatus();
			if (CompileStatus.State == EMaterialCompileState::Pending
				|| CompileStatus.State == EMaterialCompileState::Running
				|| CompileStatus.State == EMaterialCompileState::Failed
				|| CompileStatus.State == EMaterialCompileState::Rejected)
			{
				const bool bFailed = CompileStatus.State == EMaterialCompileState::Failed
					|| CompileStatus.State == EMaterialCompileState::Rejected;
				const char* Label = bFailed
					? (CompileStatus.bLastKnownGoodDisplayed
						? "Compile failed - preview is last known good"
						: "Compile failed - preview uses fallback")
					: (CompileStatus.bLastKnownGoodDisplayed
						? "Compiling - preview is last known good"
						: "Compiling material graph");
				DrawList->AddText(Add(CanvasMinimum, {12.0f, 10.0f}),
					bFailed ? IM_COL32(245, 110, 105, 255)
						: IM_COL32(235, 190, 85, 255), Label);
			}

			const ImVec2 Mouse = ImGui::GetIO().MousePos;
			if (bHovered && !ImGui::IsAnyItemActive()
				&& ImGui::GetIO().MouseWheel != 0.0f)
			{
				const ImVec2 GraphUnderMouse = Multiply(
					Subtract(Subtract(Mouse, CanvasMinimum), Pan), 1.0f / Zoom);
				Zoom = std::clamp(Zoom * (ImGui::GetIO().MouseWheel > 0.0f ? 1.12f : 0.89f),
					0.25f, 2.0f);
				DetailLevel = FMaterialGraphGeometry::SelectDetailLevel(Zoom, DetailLevel);
				Pan = Subtract(Subtract(Mouse, CanvasMinimum),
					Multiply(GraphUnderMouse, Zoom));
			}
			if (bHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
				Pan = Add(Pan, ImGui::GetIO().MouseDelta);
			const float GraphBodyFontSize = GraphBodyFontHeight * Zoom;
			const float GraphTitleFontSize = GraphTitleFontHeight * Zoom;
			const float GraphSecondaryFontSize = GraphSecondaryFontHeight * Zoom;
			const float GlobalFontScale = ImGui::GetFontSize()
				/ std::max(ImGui::GetStyle().FontSizeBase, 1.0f);
			const float GraphControlFontSizeBase = GraphBodyFontSize / GlobalFontScale;
			const ImVec2 GraphControlFramePadding{
				GraphControlHorizontalPadding * Zoom,
				GraphControlVerticalPadding * Zoom};
			const ImVec2 GraphControlItemSpacing{4.0f * Zoom, 4.0f * Zoom};
			const float GraphControlHeight = GraphBodyFontSize
				+ GraphControlFramePadding.y * 2.0f;

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

			constexpr std::array SurfaceNames{
				"Base Color", "Normal", "Metallic", "Roughness",
				"Ambient Occlusion", "Emissive", "Opacity", "Opacity Mask",
				"Surface"};
			constexpr std::array SurfaceTypes{
				EMaterialProgramValueType::Float3,
				EMaterialProgramValueType::Float3,
				EMaterialProgramValueType::Float,
				EMaterialProgramValueType::Float,
				EMaterialProgramValueType::Float,
				EMaterialProgramValueType::Float3,
				EMaterialProgramValueType::Float,
				EMaterialProgramValueType::Float,
				EMaterialProgramValueType::Surface};
			const FLinkingInteraction* Linking =
				std::get_if<FLinkingInteraction>(&Interaction);
			const FGuid LinkSourceNode = Linking ? Linking->SourceNode : FGuid{};
			const bool bConnectingAggregate = std::ranges::any_of(View.Nodes,
				[&](const FMaterialGraphNodeView& Node) {
					return Node.Node.Id == LinkSourceNode
						&& Node.Node.ResultType == EMaterialProgramValueType::Surface;
				});
			const bool bAggregateOutput = View.Outputs.Surface.SourceNodeId.IsValid()
				|| bConnectingAggregate;
			std::vector<size_t> ActiveSurfaceIndices;
			if (bAggregateOutput) ActiveSurfaceIndices = {8};
			else ActiveSurfaceIndices = {0, 1, 2, 3, 4, 5, 6, 7};
			const ImVec2 CurrentSurfaceGraphPosition = *SurfaceGraphPosition;
			const ImVec2 SurfaceMinimum = Add(CanvasMinimum,
				Add(Pan, Multiply(CurrentSurfaceGraphPosition, Zoom)));
			const ImVec2 SurfaceMaximum = Add(SurfaceMinimum, Multiply({
				Metrics.SurfaceWidth,
				Metrics.SurfaceHeaderHeight + PinSpacing * ActiveSurfaceIndices.size()
					+ NodePadding}, Zoom));
			std::array<ImVec2, 9> SurfacePins;
			const auto OutputLinks = SurfaceLinks(View.Outputs);
			for (size_t Row = 0; Row < ActiveSurfaceIndices.size(); ++Row)
			{
				const size_t Index = ActiveSurfaceIndices[Row];
				SurfacePins[Index] = {SurfaceMinimum.x,
					SurfaceMinimum.y + FMaterialGraphGeometry::GetSurfacePinOffset(
						static_cast<uint32>(Row)) * Zoom};
				const auto SourceIt = VisualIndices.find(OutputLinks[Index]->SourceNodeId);
				if (SourceIt == VisualIndices.end()) continue;
				const ImVec2 A = VisualNodes[SourceIt->second].OutputPin;
				const ImVec2 B = SurfacePins[Index];
				const bool bFocused = SelectedNodes.empty()
					|| SelectedNodes.contains(VisualNodes[SourceIt->second].View->Node.Id)
					|| (SelectedNodes.contains(EMaterialGraphTerminal::MaterialOutput)
						&& (!SelectedSurfaceOutput
							|| static_cast<size_t>(*SelectedSurfaceOutput) == Index));
				const ImU32 Color = TypeColor(SurfaceTypes[Index]);
				DrawCulledLink(*DrawList, A, B, CanvasMinimum, CanvasMaximum,
					bFocused ? Color : WithAlpha(Color, 72), bFocused ? 3.0f : 1.5f);
			}

			const FVisualNode* HoveredNode = nullptr;
			const FVisualNode* HoveredOutput = nullptr;
			const FVisualNode* HoveredInputNode = nullptr;
			uint32 HoveredInputIndex = 0;
			std::optional<EMaterialSurfaceOutput> HoveredSurfaceOutput;
			const bool bHoveredMaterialOutputHeader =
				Contains(SurfaceMinimum,
					{SurfaceMaximum.x,
						SurfaceMinimum.y + Metrics.SurfaceHeaderHeight * Zoom}, Mouse)
				&& Mouse.y < SurfaceMinimum.y + Metrics.SurfaceHeaderHeight * Zoom;
			std::optional<EMaterialProgramValueType> LinkSourceType;
			bool bEmbeddedControlHoveredOrActive = false;
			if (const auto It = VisualIndices.find(LinkSourceNode);
				It != VisualIndices.end())
				LinkSourceType = VisualNodes[It->second].View->Node.ResultType;
			for (const FVisualNode& Visual : VisualNodes)
			{
				if (!Intersects(Visual.Minimum, Visual.Maximum,
					CanvasMinimum, CanvasMaximum)) continue;
				const bool bSelected = SelectedNodes.contains(Visual.View->Node.Id);
				DrawList->AddRectFilled(Visual.Minimum, Visual.Maximum,
					bSelected ? IM_COL32(55, 72, 94, 255) : IM_COL32(42, 46, 54, 255),
					6.0f);
				DrawList->AddRect(Visual.Minimum, Visual.Maximum,
					bSelected ? IM_COL32(90, 170, 245, 255) : IM_COL32(78, 84, 96, 255),
					6.0f, 0, bSelected ? 2.5f : 1.0f);
				DrawList->AddRectFilled(Visual.Minimum,
					{Visual.Maximum.x, Visual.Minimum.y + NodeHeaderHeight * Zoom},
					IM_COL32(57, 62, 74, 255), 6.0f, ImDrawFlags_RoundCornersTop);
				if (DetailLevel != EMaterialGraphDetailLevel::Overview)
				{
					const float FontSize = GraphTitleFontSize;
					const std::string Label = Ellipsize(Visual.View->PrimaryLabel,
						(NodeWidth - NodePadding * 2.0f) * Zoom
							* ImGui::GetFontSize() / FontSize);
					const ImVec4 Clip(Visual.Minimum.x + 5.0f, Visual.Minimum.y,
						Visual.Maximum.x - 5.0f,
						Visual.Minimum.y + NodeHeaderHeight * Zoom);
					DrawList->AddText(ImGui::GetFont(), FontSize,
						Add(Visual.Minimum,
							{8.0f * Zoom, (NodeHeaderHeight * Zoom - FontSize) * 0.5f}),
						IM_COL32(235, 238, 242, 255), Label.c_str(), nullptr, 0.0f, &Clip);
					if (DetailLevel == EMaterialGraphDetailLevel::Editing
						&& !Visual.View->SecondaryLabel.empty())
					{
						const std::string Secondary = Ellipsize(Visual.View->SecondaryLabel,
							(NodeWidth - NodePadding * 2.0f) * Zoom
								* ImGui::GetFontSize() / GraphSecondaryFontSize);
						const ImVec4 SecondaryClip(Visual.Minimum.x + 5.0f,
							Visual.Minimum.y + NodeHeaderHeight * Zoom,
							Visual.Maximum.x - 5.0f,
							Visual.Minimum.y + (NodeHeaderHeight + Metrics.SecondaryHeight) * Zoom);
						DrawList->AddText(ImGui::GetFont(), GraphSecondaryFontSize,
							Add(Visual.Minimum, {8.0f * Zoom, NodeHeaderHeight * Zoom}),
							IM_COL32(165, 172, 186, 255), Secondary.c_str(), nullptr, 0.0f,
							&SecondaryClip);
					}
				}
				const float PinRadius = std::max(2.0f, 5.0f * Zoom);
				DrawList->AddCircleFilled(Visual.OutputPin, PinRadius,
					TypeColor(Visual.View->Node.ResultType));
				const bool bInlineEditorVisible =
					DetailLevel == EMaterialGraphDetailLevel::Editing
					&& SelectedNodes.size() == 1
					&& SelectedNodes.contains(Visual.View->Node.Id)
					&& (Visual.View->Node.Opcode == EMaterialProgramOpcode::Constant
						|| Visual.View->Node.Opcode == EMaterialProgramOpcode::Parameter
						|| Visual.View->Node.Opcode == EMaterialProgramOpcode::TextureParameter
						|| Visual.View->Node.Opcode == EMaterialProgramOpcode::Swizzle);
				if (DetailLevel == EMaterialGraphDetailLevel::Editing
					&& !bInlineEditorVisible)
				{
					const std::string ResultLabel = GetProgramTypeName(Visual.View->Node.ResultType);
					const float ResultLabelWidth = ImGui::GetFont()->CalcTextSizeA(
						GraphBodyFontSize, FLT_MAX, 0.0f, ResultLabel.c_str()).x;
					DrawList->AddText(ImGui::GetFont(), GraphBodyFontSize,
						Add(Visual.OutputPin,
							{-8.0f * Zoom - ResultLabelWidth,
								-GraphBodyFontSize * 0.5f}),
						IM_COL32(185, 190, 202, 255), ResultLabel.c_str());
				}
				for (size_t Index = 0; Index < Visual.InputPins.size(); ++Index)
				{
					DrawList->AddCircleFilled(Visual.InputPins[Index], PinRadius,
						TypeColor(Visual.View->Inputs[Index].SourceType));
					if (DetailLevel == EMaterialGraphDetailLevel::Editing)
						DrawList->AddText(ImGui::GetFont(), GraphBodyFontSize,
							Add(Visual.InputPins[Index],
								{9.0f * Zoom, -GraphBodyFontSize * 0.5f}),
							IM_COL32(205, 210, 220, 255),
							Visual.View->Inputs[Index].Name.c_str());
					if (LinkSourceType)
					{
						const bool bAccepted = std::ranges::find(
							Visual.View->Inputs[Index].AcceptedTypes,
							*LinkSourceType)
							!= Visual.View->Inputs[Index].AcceptedTypes.end();
						DrawList->AddCircle(Visual.InputPins[Index], 8.0f,
							bAccepted ? IM_COL32(90, 220, 125, 230)
								: IM_COL32(235, 90, 90, 230), 0, 1.5f);
					}
					if (DetailLevel != EMaterialGraphDetailLevel::Overview
						&& std::hypot(Mouse.x - Visual.InputPins[Index].x,
						Mouse.y - Visual.InputPins[Index].y) <= 8.0f)
					{
						HoveredInputNode = &Visual;
						HoveredInputIndex = static_cast<uint32>(Index);
					}
				}
				if (DetailLevel != EMaterialGraphDetailLevel::Overview
					&& std::hypot(Mouse.x - Visual.OutputPin.x,
					Mouse.y - Visual.OutputPin.y) <= 8.0f) HoveredOutput = &Visual;
				if (Contains(Visual.Minimum, Visual.Maximum, Mouse)) HoveredNode = &Visual;
			}
			if (DetailLevel == EMaterialGraphDetailLevel::Editing
				&& SelectedNodes.size() == 1
				&& std::holds_alternative<FGuid>(*SelectedNodes.begin()))
			{
				const auto SelectedIt = VisualIndices.find(std::get<FGuid>(*SelectedNodes.begin()));
				if (SelectedIt != VisualIndices.end())
				{
					const FVisualNode& Visual = VisualNodes[SelectedIt->second];
					if (Visual.View->Node.Opcode == EMaterialProgramOpcode::Constant
						&& Intersects(Visual.Minimum, Visual.Maximum, CanvasMinimum, CanvasMaximum))
					{
						std::array ConstantDraft{Visual.View->Node.Literal.X,
							Visual.View->Node.Literal.Y, Visual.View->Node.Literal.Z,
							Visual.View->Node.Literal.W};
						if (const auto* Inline =
							std::get_if<FInlineEditingInteraction>(&Interaction);
							Inline && Inline->Node == Visual.View->Node.Id)
							ConstantDraft = Inline->ConstantDraft;
						const ImVec2 SavedCursor = ImGui::GetCursorScreenPos();
						ImGui::SetCursorScreenPos(Add(Visual.Minimum,
							{10.0f * Zoom, (NodeHeaderHeight + Metrics.SecondaryHeight
								+ 4.0f) * Zoom}));
						ImGui::PushID(Visual.View->Node.Id.ToString().c_str());
						ImGui::SetNextItemWidth(std::max(80.0f,
							(NodeWidth - 20.0f) * Zoom));
						ImGui::PushFont(nullptr, GraphControlFontSizeBase);
						ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
							GraphControlFramePadding);
						ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing,
							GraphControlItemSpacing);
						const bool bValueSubmitted = DrawNumericInputEditor("##InlineConstant",
							Visual.View->Node.ResultType, ConstantDraft.data());
						bEmbeddedControlHoveredOrActive |=
							ImGui::IsItemHovered() || ImGui::IsItemActive();
						const bool bInlineActive = ImGui::IsItemActive();
						const bool bCancelInline = ImGui::IsKeyPressed(ImGuiKey_Escape)
							&& (bInlineActive || ImGui::IsItemFocused());
						if (bCancelInline) ResetInteraction();
						else if (bValueSubmitted || ImGui::IsItemDeactivatedAfterEdit())
						{
							FMaterialProgramNode Edited = Visual.View->Node;
							Edited.Literal = {ConstantDraft[0], ConstantDraft[1],
								ConstantDraft[2], ConstantDraft[3]};
							ReportCommand(FMaterialGraphOperations::ReplaceNode(
								Material, std::move(Edited), &Transactions), ReportError);
						}
						if (bInlineActive && !bCancelInline)
							Interaction = FInlineEditingInteraction{
								.Node = Visual.View->Node.Id,
								.ConstantDraft = ConstantDraft};
						else if (std::holds_alternative<FInlineEditingInteraction>(Interaction))
							ResetInteraction();
						ImGui::PopStyleVar(2);
						ImGui::PopFont();
						ImGui::PopID();
						ImGui::SetCursorScreenPos(SavedCursor);
						ImGui::Dummy({0.0f, 0.0f});
					}
					else if ((Visual.View->Node.Opcode == EMaterialProgramOpcode::Parameter
						|| Visual.View->Node.Opcode == EMaterialProgramOpcode::TextureParameter)
						&& Intersects(Visual.Minimum, Visual.Maximum, CanvasMinimum, CanvasMaximum))
					{
						const ImVec2 SavedCursor = ImGui::GetCursorScreenPos();
						ImGui::SetCursorScreenPos(Add(Visual.Minimum,
							{10.0f * Zoom, (NodeHeaderHeight + Metrics.SecondaryHeight
								+ 4.0f) * Zoom}));
						ImGui::PushID(Visual.View->Node.Id.ToString().c_str());
						ImGui::SetNextItemWidth(std::max(80.0f, (NodeWidth - 20.0f) * Zoom));
						ImGui::PushFont(nullptr, GraphControlFontSizeBase);
						ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
							GraphControlFramePadding);
						ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing,
							GraphControlItemSpacing);
						FResolvedMaterialParameter Resolved;
						const bool bEditValue =
							Visual.View->Node.Opcode == EMaterialProgramOpcode::Parameter
							&& SelectedNodes.contains(Visual.View->Node.Id)
							&& Material.ResolveParameterValue(
								Visual.View->Node.ParameterId, Resolved);
						if (bEditValue)
						{
							const auto Literal = ReadParameterLiteral(Visual.View->Node.ResultType, Resolved.Value);
							std::array ConstantDraft{Literal.X, Literal.Y, Literal.Z, Literal.W};
							if (const auto* Inline =
								std::get_if<FInlineEditingInteraction>(&Interaction);
								Inline && Inline->Node == Visual.View->Node.Id)
							{
								ConstantDraft = Inline->ConstantDraft;
							}
							const bool bValueChanged = DrawNumericDragEditor("##InlineParameterValue",
								Visual.View->Node.ResultType, ConstantDraft.data());
							bEmbeddedControlHoveredOrActive |=
								ImGui::IsItemHovered() || ImGui::IsItemActive();
							const bool bInlineActive = ImGui::IsItemActive();
							const bool bCancelInline = ImGui::IsKeyPressed(ImGuiKey_Escape)
								&& (bInlineActive || ImGui::IsItemFocused());
							if (bCancelInline)
							{
								if (ParameterEditSession.IsActive())
									ReportCommand(ParameterEditSession.Cancel(), ReportError);
								ResetInteraction();
							}
							else if (bValueChanged)
							{
								const auto Value = MakeParameterValue(Visual.View->Node.ResultType,
									{ConstantDraft[0], ConstantDraft[1], ConstantDraft[2], ConstantDraft[3]});
								if (!ParameterEditSession.IsActive())
									ReportCommand(ParameterEditSession.Begin(Material,
										Visual.View->Node.ParameterId, &Transactions), ReportError);
								if (ParameterEditSession.IsActive())
									ReportCommand(ParameterEditSession.Apply(std::move(Value)), ReportError);
							}
							if (!bCancelInline && ImGui::IsItemDeactivatedAfterEdit())
							{
								if (ParameterEditSession.IsActive())
									ReportCommand(ParameterEditSession.Commit(), ReportError);
								ResetInteraction();
							}
							if (bInlineActive && !bCancelInline)
								Interaction = FInlineEditingInteraction{
									.Node = Visual.View->Node.Id,
									.ConstantDraft = ConstantDraft};
							else if (std::holds_alternative<FInlineEditingInteraction>(Interaction))
								ResetInteraction();
						}
						else
						{
							const char* Preview = Visual.View->SecondaryLabel.empty()
								? "Select parameter" : Visual.View->SecondaryLabel.c_str();
							if (ImGui::BeginCombo("##InlineParameter", Preview))
							{
								for (const auto& Definition : Material.GetParameterDefinitions())
								{
									if (GetProgramType(Definition.Type) != Visual.View->Node.ResultType) continue;
									if (ImGui::Selectable((Definition.DisplayName + "##" + Definition.Id.ToString()).c_str(),
										Definition.Id == Visual.View->Node.ParameterId))
									{
										FMaterialProgramNode Edited = Visual.View->Node;
										Edited.ParameterId = Definition.Id;
										Edited.DisplayName = Definition.DisplayName;
										ReportCommand(FMaterialGraphOperations::ReplaceNode(
											Material, std::move(Edited), &Transactions), ReportError);
									}
								}
								ImGui::EndCombo();
							}
							bEmbeddedControlHoveredOrActive |=
								ImGui::IsItemHovered() || ImGui::IsItemActive();
						}
						ImGui::PopStyleVar(2);
						ImGui::PopFont();
						ImGui::PopID();
						ImGui::SetCursorScreenPos(SavedCursor);
						ImGui::Dummy({0.0f, 0.0f});
					}
					else if (Visual.View->Node.Opcode == EMaterialProgramOpcode::Swizzle
						&& Intersects(Visual.Minimum, Visual.Maximum, CanvasMinimum, CanvasMaximum))
					{
						std::array SwizzleDraft{
							static_cast<int>(Visual.View->Node.SwizzleX),
							static_cast<int>(Visual.View->Node.SwizzleY),
							static_cast<int>(Visual.View->Node.SwizzleZ),
							static_cast<int>(Visual.View->Node.SwizzleW)};
						if (const auto* Inline =
							std::get_if<FInlineEditingInteraction>(&Interaction);
							Inline && Inline->Node == Visual.View->Node.Id)
							SwizzleDraft = Inline->SwizzleDraft;
						const ImVec2 SavedCursor = ImGui::GetCursorScreenPos();
						ImGui::SetCursorScreenPos(Add(Visual.Minimum,
							{10.0f * Zoom, (NodeHeaderHeight + Metrics.SecondaryHeight
								+ 4.0f) * Zoom}));
						ImGui::PushID(Visual.View->Node.Id.ToString().c_str());
						ImGui::SetNextItemWidth(std::max(80.0f, (NodeWidth - 20.0f) * Zoom));
						ImGui::PushFont(nullptr, GraphControlFontSizeBase);
						ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
							GraphControlFramePadding);
						ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing,
							GraphControlItemSpacing);
						ImGui::DragInt4("##InlineSwizzle", SwizzleDraft.data(), 0.1f, 0, 3);
						bEmbeddedControlHoveredOrActive |=
							ImGui::IsItemHovered() || ImGui::IsItemActive();
						const bool bInlineActive = ImGui::IsItemActive();
						const bool bCancelInline = ImGui::IsKeyPressed(ImGuiKey_Escape)
							&& (bInlineActive || ImGui::IsItemFocused());
						if (bCancelInline) ResetInteraction();
						else if (ImGui::IsItemDeactivatedAfterEdit())
						{
							FMaterialProgramNode Edited = Visual.View->Node;
							Edited.SwizzleX = static_cast<uint8>(std::clamp(SwizzleDraft[0], 0, 3));
							Edited.SwizzleY = static_cast<uint8>(std::clamp(SwizzleDraft[1], 0, 3));
							Edited.SwizzleZ = static_cast<uint8>(std::clamp(SwizzleDraft[2], 0, 3));
							Edited.SwizzleW = static_cast<uint8>(std::clamp(SwizzleDraft[3], 0, 3));
							ReportCommand(FMaterialGraphOperations::ReplaceNode(
								Material, std::move(Edited), &Transactions), ReportError);
						}
						if (bInlineActive && !bCancelInline)
							Interaction = FInlineEditingInteraction{
								.Node = Visual.View->Node.Id,
								.SwizzleDraft = SwizzleDraft};
						else if (std::holds_alternative<FInlineEditingInteraction>(Interaction))
							ResetInteraction();
						ImGui::PopStyleVar(2);
						ImGui::PopFont();
						ImGui::PopID();
						ImGui::SetCursorScreenPos(SavedCursor);
						ImGui::Dummy({0.0f, 0.0f});
					}
				}
			}
			if (HoveredNode && DetailLevel != EMaterialGraphDetailLevel::Overview
				&& !bEmbeddedControlHoveredOrActive)
			{
				ImGui::BeginTooltip();
				ImGui::TextUnformatted(HoveredNode->View->PrimaryLabel.c_str());
				if (!HoveredNode->View->SecondaryLabel.empty())
					ImGui::TextDisabled("%s", HoveredNode->View->SecondaryLabel.c_str());
				ImGui::TextDisabled("Output: %s", GetProgramTypeName(HoveredNode->View->Node.ResultType));
				ImGui::EndTooltip();
			}
			const bool bMaterialOutputSelected =
				SelectedNodes.contains(EMaterialGraphTerminal::MaterialOutput);
			DrawList->AddRectFilled(SurfaceMinimum, SurfaceMaximum,
				bMaterialOutputSelected ? IM_COL32(55, 72, 94, 255)
					: IM_COL32(38, 42, 50, 245), 6.0f);
			DrawList->AddRect(SurfaceMinimum, SurfaceMaximum,
				bMaterialOutputSelected ? IM_COL32(90, 170, 245, 255)
					: IM_COL32(92, 100, 116, 255), 6.0f, 0,
				bMaterialOutputSelected ? 2.5f : 1.0f);
			DrawList->AddRectFilled(SurfaceMinimum,
				{SurfaceMaximum.x,
					SurfaceMinimum.y + Metrics.SurfaceHeaderHeight * Zoom},
				IM_COL32(57, 62, 74, 255), 6.0f, ImDrawFlags_RoundCornersTop);
			if (DetailLevel != EMaterialGraphDetailLevel::Overview)
			{
				const float FontSize = GraphTitleFontSize;
				const float SecondaryFontSize = GraphSecondaryFontSize;
				const std::string MaterialName = Ellipsize(Material.GetName(),
					(Metrics.SurfaceWidth - 20.0f) * Zoom
						* ImGui::GetFontSize() / FontSize);
				const ImVec4 Clip(SurfaceMinimum.x + 5.0f, SurfaceMinimum.y,
					SurfaceMaximum.x - 5.0f,
					SurfaceMinimum.y + Metrics.SurfaceHeaderHeight * Zoom);
				DrawList->AddText(ImGui::GetFont(), FontSize,
					Add(SurfaceMinimum, {10.0f * Zoom, 6.0f * Zoom}),
					IM_COL32(235, 238, 242, 255), MaterialName.c_str(), nullptr, 0.0f, &Clip);
				DrawList->AddText(ImGui::GetFont(), SecondaryFontSize,
					Add(SurfaceMinimum,
						{10.0f * Zoom, 24.0f * Zoom}),
					IM_COL32(165, 172, 186, 255), "Material Output", nullptr, 0.0f, &Clip);
			}
			for (size_t Index : ActiveSurfaceIndices)
			{
				if (SelectedSurfaceOutput
					&& static_cast<size_t>(*SelectedSurfaceOutput) == Index)
					DrawList->AddRectFilled(
						{SurfaceMinimum.x + 2.0f, SurfacePins[Index].y - 10.0f},
						{SurfaceMaximum.x - 2.0f, SurfacePins[Index].y + 10.0f},
						IM_COL32(190, 145, 55, 75));
				DrawList->AddCircleFilled(SurfacePins[Index], std::max(2.0f, 5.0f * Zoom),
					TypeColor(SurfaceTypes[Index]));
				if (DetailLevel != EMaterialGraphDetailLevel::Overview)
				{
					const ImVec4 LabelClip(
						SurfaceMinimum.x + NodePadding * Zoom,
						SurfacePins[Index].y - PinSpacing * 0.5f * Zoom,
						SurfaceMinimum.x
							+ (NodePadding + Metrics.SurfaceLabelWidth) * Zoom,
						SurfacePins[Index].y + PinSpacing * 0.5f * Zoom);
					DrawList->AddText(ImGui::GetFont(), GraphBodyFontSize,
						Add(SurfacePins[Index],
							{NodePadding * Zoom, -GraphBodyFontSize * 0.5f}),
						IM_COL32(210, 214, 222, 255), SurfaceNames[Index],
						nullptr, 0.0f, &LabelClip);
					if (DetailLevel == EMaterialGraphDetailLevel::Readable
						&& Index < 8 && !OutputLinks[Index]->SourceNodeId.IsValid())
					{
						const auto& Value = GetMaterialSurfaceOutputDefault(View.Outputs,
							static_cast<EMaterialSurfaceOutput>(Index));
						const std::string Text = SurfaceTypes[Index] == EMaterialProgramValueType::Float3
							? std::format("{:.3g}, {:.3g}, {:.3g}", Value.X, Value.Y, Value.Z)
							: std::format("{:.3g}", Value.X);
						const float ValueX = SurfaceMinimum.x + (NodePadding
							+ Metrics.SurfaceLabelWidth + Metrics.SurfaceValueGap) * Zoom;
						const ImVec4 ValueClip(ValueX, LabelClip.y,
							SurfaceMaximum.x - NodePadding * Zoom, LabelClip.w);
						DrawList->AddText(ImGui::GetFont(), GraphBodyFontSize,
							{ValueX, SurfacePins[Index].y - GraphBodyFontSize * 0.5f},
							IM_COL32(165, 172, 186, 255), Text.c_str(), nullptr, 0.0f, &ValueClip);
					}
					if (DetailLevel == EMaterialGraphDetailLevel::Editing
						&& Index < 8 && !OutputLinks[Index]->SourceNodeId.IsValid())
					{
						const EMaterialSurfaceOutput Output =
							static_cast<EMaterialSurfaceOutput>(Index);
						if (!bSurfaceDefaultDraftInitialized[Index])
						{
							const FMaterialProgramLiteral& Value =
								GetMaterialSurfaceOutputDefault(View.Outputs, Output);
							SurfaceDefaultDrafts[Index] =
								{Value.X, Value.Y, Value.Z, Value.W};
							bSurfaceDefaultDraftInitialized[Index] = true;
						}
						const ImVec2 SavedCursor = ImGui::GetCursorScreenPos();
						ImGui::SetCursorScreenPos(
							{SurfaceMinimum.x + (NodePadding
								+ Metrics.SurfaceLabelWidth
								+ Metrics.SurfaceValueGap) * Zoom,
								SurfacePins[Index].y - GraphControlHeight * 0.5f});
						ImGui::PushID(static_cast<int>(Index) + 9000);
						ImGui::SetNextItemWidth(Metrics.SurfaceValueWidth * Zoom);
						ImGui::PushFont(nullptr, GraphControlFontSizeBase);
						ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
							GraphControlFramePadding);
						ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing,
							GraphControlItemSpacing);
						const bool bValueSubmitted = DrawNumericInputEditor(
							"##SurfaceDefault", SurfaceTypes[Index],
							SurfaceDefaultDrafts[Index].data());
						bEmbeddedControlHoveredOrActive |=
							ImGui::IsItemHovered() || ImGui::IsItemActive();
						const bool bInlineActive = ImGui::IsItemActive();
						const bool bCancelInline = ImGui::IsKeyPressed(ImGuiKey_Escape)
							&& (bInlineActive || ImGui::IsItemFocused());
						if (bCancelInline)
							bSurfaceDefaultDraftInitialized[Index] = false;
						else if (bValueSubmitted || ImGui::IsItemDeactivatedAfterEdit())
						{
							ReportCommand(FMaterialGraphOperations::SetSurfaceDefault(
								Material, {.Output = Output, .Value = {
									SurfaceDefaultDrafts[Index][0],
									SurfaceDefaultDrafts[Index][1],
									SurfaceDefaultDrafts[Index][2],
									SurfaceDefaultDrafts[Index][3]}},
								&Transactions), ReportError);
							bSurfaceDefaultDraftInitialized[Index] = false;
						}
						if (!bInlineActive)
							bSurfaceDefaultDraftInitialized[Index] = false;
						ImGui::PopStyleVar(2);
						ImGui::PopFont();
						ImGui::PopID();
						ImGui::SetCursorScreenPos(SavedCursor);
						ImGui::Dummy({0.0f, 0.0f});
					}
				}
				if (DetailLevel != EMaterialGraphDetailLevel::Overview
					&& std::hypot(Mouse.x - SurfacePins[Index].x,
					Mouse.y - SurfacePins[Index].y) <= 8.0f)
					HoveredSurfaceOutput = static_cast<EMaterialSurfaceOutput>(Index);
			}
			const bool bCanvasPointerInteractionAvailable = bHovered
				&& !bEmbeddedControlHoveredOrActive;
			const bool bCanvasKeyboardInteractionAvailable =
				bCanvasPointerInteractionAvailable && !ImGui::GetIO().WantTextInput;
			if (bCanvasPointerInteractionAvailable
				&& (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
					|| ImGui::IsMouseClicked(ImGuiMouseButton_Middle)
					|| ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
				ImGui::SetWindowFocus();
			const bool bOpenCreationMenuByDoubleClick = bCanvasPointerInteractionAvailable
				&& ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
				&& !HoveredNode && !HoveredOutput && !HoveredInputNode
				&& !HoveredSurfaceOutput && !bHoveredMaterialOutputHeader;
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
					ReportCommand(MoveSession.Cancel(), ReportError);
					SurfaceGraphPosition.reset();
				}
				ResetInteraction();
			}

			if (bCanvasPointerInteractionAvailable && !bOpenCreationMenuByDoubleClick
				&& ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				if (HoveredInputNode)
				{
					Interaction = FReconnectingInputInteraction{
						.DestinationNode = HoveredInputNode->View->Node.Id,
						.DestinationInputIndex = HoveredInputIndex};
				}
				else if (HoveredOutput)
				{
					Interaction = FLinkingInteraction{HoveredOutput->View->Node.Id};
				}
				else if (HoveredSurfaceOutput)
				{
					SelectedNodes = {EMaterialGraphTerminal::MaterialOutput};
					SelectedSurfaceOutput = HoveredSurfaceOutput;
					Interaction = FReconnectingSurfaceInteraction{*HoveredSurfaceOutput};
				}
				else if (bHoveredMaterialOutputHeader)
				{
					SelectedNodes.clear();
					SelectedSurfaceOutput.reset();
					SelectedNodes.insert(EMaterialGraphTerminal::MaterialOutput);
					const FMaterialGraphCommandResult Begun =
						MoveSession.BeginMaterialOutput(Material, &Transactions);
					ReportCommand(Begun, ReportError);
					if (Begun)
					{
						Interaction = FMovingInteraction{
							.StartMouse = Mouse,
							.MaterialOutputStart = CurrentSurfaceGraphPosition};
					}
				}
				else if (HoveredNode)
				{
					const FGuid Id = HoveredNode->View->Node.Id;
					SelectedNodes.erase(EMaterialGraphTerminal::MaterialOutput);
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
						const FMaterialGraphCommandResult Begun = MoveSession.Begin(
							Material, Selection, &Transactions);
						ReportCommand(Begun, ReportError);
						if (Begun)
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

			if (auto* Moving = std::get_if<FMovingInteraction>(&Interaction);
				Moving && MoveSession.IsActive()
				&& ImGui::IsMouseDragging(ImGuiMouseButton_Left))
			{
				const ImVec2 Delta = Multiply(Subtract(Mouse, Moving->StartMouse), 1.0f / Zoom);
				if (Moving->MaterialOutputStart)
				{
					const ImVec2 Position = Add(*Moving->MaterialOutputStart, Delta);
					ReportCommand(MoveSession.ApplyMaterialOutput(
						static_cast<int32>(std::round(Position.x)),
						static_cast<int32>(std::round(Position.y))), ReportError);
					SurfaceGraphPosition = Position;
				}
				else
				{
					std::vector<FMaterialGraphNodePresentation> Positions;
					for (const auto& [Id, Start] : Moving->StartPositions)
						Positions.push_back({Id,
							static_cast<int32>(std::round(Start.X + Delta.x)),
							static_cast<int32>(std::round(Start.Y + Delta.y))});
					ReportCommand(MoveSession.Apply(Positions), ReportError);
				}
			}
			if (std::holds_alternative<FMovingInteraction>(Interaction)
				&& MoveSession.IsActive() && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
			{
				ReportCommand(MoveSession.Commit(), ReportError);
				ResetInteraction();
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
				const auto SourceIt = VisualIndices.find(SourceNode);
				if (SourceIt != VisualIndices.end())
				{
					const ImVec2 A = VisualNodes[SourceIt->second].OutputPin;
					DrawList->AddBezierCubic(A, Add(A, {60.0f, 0.0f}),
						Subtract(Mouse, {60.0f, 0.0f}), Mouse,
						TypeColor(VisualNodes[SourceIt->second].View->Node.ResultType), 2.5f);
				}
				if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
				{
					ResetInteraction();
					if (HoveredInputNode)
						ReportCommand(FMaterialGraphOperations::Connect(Material, {
							.SourceNodeId = SourceNode,
							.DestinationNodeId = HoveredInputNode->View->Node.Id,
							.DestinationInputIndex = HoveredInputIndex,
							.bReplaceExisting = ImGui::GetIO().KeyShift,
						}, &Transactions), ReportError);
					else if (HoveredSurfaceOutput)
					{
						if (static_cast<size_t>(*HoveredSurfaceOutput) == 8)
							ReportCommand(FMaterialGraphOperations::AssignAggregateSurface(
								Material, SourceNode, &Transactions), ReportError);
						else ReportCommand(FMaterialGraphOperations::AssignSurfaceOutput(Material, {
							.Output = *HoveredSurfaceOutput,
							.SourceNodeId = SourceNode,
						}, &Transactions), ReportError);
					}
					else if (bCanvasPointerInteractionAvailable && !HoveredNode
						&& !HoveredOutput && !Contains(SurfaceMinimum, SurfaceMaximum, Mouse))
					{
						Interaction = FNodeCreationMenuInteraction{
							.SourceNode = SourceNode,
							.GraphPosition = Multiply(
								Subtract(Subtract(Mouse, CanvasMinimum), Pan), 1.0f / Zoom)};
					}
				}
			}
			if (const auto* Reconnecting =
				std::get_if<FReconnectingInputInteraction>(&Interaction))
			{
				const FGuid DestinationNode = Reconnecting->DestinationNode;
				const uint32 DestinationInputIndex = Reconnecting->DestinationInputIndex;
				const auto DestinationIt = VisualIndices.find(DestinationNode);
				if (DestinationIt != VisualIndices.end()
					&& DestinationInputIndex
						< VisualNodes[DestinationIt->second].InputPins.size())
				{
					const ImVec2 A = VisualNodes[DestinationIt->second]
						.InputPins[DestinationInputIndex];
					DrawList->AddBezierCubic(A, Subtract(A, {60.0f, 0.0f}),
						Add(Mouse, {60.0f, 0.0f}), Mouse,
						IM_COL32(240, 210, 105, 255), 2.5f);
				}
				if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
				{
					if (HoveredOutput)
						ReportCommand(FMaterialGraphOperations::Connect(Material, {
							.SourceNodeId = HoveredOutput->View->Node.Id,
							.DestinationNodeId = DestinationNode,
							.DestinationInputIndex = DestinationInputIndex,
							.bReplaceExisting = true,
						}, &Transactions), ReportError);
					ResetInteraction();
				}
			}
			if (const auto* Reconnecting =
				std::get_if<FReconnectingSurfaceInteraction>(&Interaction))
			{
				const EMaterialSurfaceOutput ReconnectSurfaceOutput = Reconnecting->Output;
				const size_t OutputIndex = static_cast<size_t>(ReconnectSurfaceOutput);
				if (OutputIndex < SurfacePins.size())
				{
					const ImVec2 A = SurfacePins[OutputIndex];
					DrawList->AddBezierCubic(A, Subtract(A, {60.0f, 0.0f}),
						Add(Mouse, {60.0f, 0.0f}), Mouse,
						IM_COL32(240, 210, 105, 255), 2.5f);
				}
				if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
				{
					if (HoveredOutput)
					{
						if (static_cast<size_t>(ReconnectSurfaceOutput) == 8)
							ReportCommand(FMaterialGraphOperations::AssignAggregateSurface(
								Material, HoveredOutput->View->Node.Id, &Transactions), ReportError);
						else ReportCommand(FMaterialGraphOperations::AssignSurfaceOutput(Material, {
							.Output = ReconnectSurfaceOutput,
							.SourceNodeId = HoveredOutput->View->Node.Id,
						}, &Transactions), ReportError);
					}
					ResetInteraction();
				}
			}

			HandleKeyboardInput(Material, Transactions, View, CanvasMinimum,
				CanvasSize, Mouse, bCanvasKeyboardInteractionAvailable, ReportError);
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
			if (bPendingFrameSurface)
			{
				FrameNodes(View, CanvasSize, EFrameScope::Selection);
				bPendingFrameSurface = false;
			}
			DetailLevel = FMaterialGraphGeometry::SelectDetailLevel(Zoom, DetailLevel);
			if (bCanvasPointerInteractionAvailable
				&& ImGui::IsMouseClicked(ImGuiMouseButton_Right))
			{
				if (!HoveredNode && !HoveredInputNode && !HoveredSurfaceOutput)
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
						.SurfaceOutput = HoveredSurfaceOutput};
					ImGui::OpenPopup("MaterialGraphContext");
				}
			}
			if (bCanvasKeyboardInteractionAvailable && ImGui::IsKeyPressed(ImGuiKey_Space))
			{
				Interaction = FNodeCreationMenuInteraction{
					.SourceNode = {},
					.GraphPosition = Multiply(
						Subtract(Subtract(Mouse, CanvasMinimum), Pan), 1.0f / Zoom)};
			}

			DrawContextMenu(Material, Transactions, View, ReportError);
			DrawCreationMenu(Material, Transactions, View, ReportError);

			DrawList->PopClipRect();
		}
		ImGui::EndChild();
		ImGui::PopID();
	}
}
