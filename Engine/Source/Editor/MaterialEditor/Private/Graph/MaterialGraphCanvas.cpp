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

		auto TypeName(EMaterialProgramValueType Type) -> const char*
		{
			switch (Type)
			{
			case EMaterialProgramValueType::Float: return "Float";
			case EMaterialProgramValueType::Float2: return "Float2";
			case EMaterialProgramValueType::Float3: return "Float3";
			case EMaterialProgramValueType::Float4: return "Float4";
			case EMaterialProgramValueType::Texture2D: return "Texture2D";
			case EMaterialProgramValueType::Surface: return "Surface";
			}
			return "Unknown";
		}

		auto PaletteEntryKey(const FMaterialGraphCatalogEntry& Entry) -> std::string
		{
			return std::format("{}|{}|{}|{}", Entry.OperationName,
				Entry.SecondaryName, static_cast<uint32>(Entry.NodeTemplate.ResultType),
				Entry.NodeTemplate.ParameterId.ToString());
		}

		auto FormatInputSignature(const FMaterialGraphCatalogEntry& Entry) -> std::string
		{
			std::string Result;
			for (size_t Index = 0; Index < Entry.AcceptedInputTypes.size(); ++Index)
			{
				if (!Result.empty()) Result += ", ";
				Result += Index < Entry.InputNames.size()
					? Entry.InputNames[Index] : std::format("Input {}", Index + 1);
				Result += ": ";
				for (size_t TypeIndex = 0;
					TypeIndex < Entry.AcceptedInputTypes[Index].size(); ++TypeIndex)
				{
					if (TypeIndex != 0) Result += '/';
					Result += TypeName(Entry.AcceptedInputTypes[Index][TypeIndex]);
				}
			}
			return Result.empty() ? "No inputs" : Result;
		}

		auto DrawNumericDragEditor(const char* Label,
			EMaterialProgramValueType Type, float* Value) -> bool
		{
			constexpr float DragSpeed = 0.01f;
			switch (Type)
			{
			case EMaterialProgramValueType::Float:
				return ImGui::DragFloat(Label, Value, DragSpeed, 0.0f, 0.0f, "%.3f");
			case EMaterialProgramValueType::Float2:
				return ImGui::DragFloat2(Label, Value, DragSpeed, 0.0f, 0.0f, "%.3f");
			case EMaterialProgramValueType::Float3:
				return ImGui::DragFloat3(Label, Value, DragSpeed, 0.0f, 0.0f, "%.3f");
			case EMaterialProgramValueType::Float4:
				return ImGui::DragFloat4(Label, Value, DragSpeed, 0.0f, 0.0f, "%.3f");
			case EMaterialProgramValueType::Texture2D:
			case EMaterialProgramValueType::Surface:
				return false;
			}
			return false;
		}

		auto DrawNumericInputEditor(const char* Label,
			EMaterialProgramValueType Type, float* Value) -> bool
		{
			constexpr ImGuiInputTextFlags Flags = ImGuiInputTextFlags_EnterReturnsTrue;
			switch (Type)
			{
			case EMaterialProgramValueType::Float:
				return ImGui::InputFloat(Label, Value, 0.0f, 0.0f, "%.3f", Flags);
			case EMaterialProgramValueType::Float2:
				return ImGui::InputFloat2(Label, Value, "%.3f", Flags);
			case EMaterialProgramValueType::Float3:
				return ImGui::InputFloat3(Label, Value, "%.3f", Flags);
			case EMaterialProgramValueType::Float4:
				return ImGui::InputFloat4(Label, Value, "%.3f", Flags);
			case EMaterialProgramValueType::Texture2D:
			case EMaterialProgramValueType::Surface:
				return false;
			}
			return false;
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
				if (!Node.Presentation) continue;
				const float Y = static_cast<float>(Node.Presentation->Y);
				const float Height = FMaterialGraphGeometry::GetNodeHeight(
					static_cast<uint32>(Node.Inputs.size()));
				MaximumX = std::max(MaximumX,
					static_cast<float>(Node.Presentation->X) + Metrics.NodeWidth);
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

		auto ReportCommand(
			const FMaterialGraphCommandResult& Result,
			const FMaterialGraphCanvas::FReportError& ReportError) -> void
		{
			if (Result || !ReportError) return;
			std::string Message = Result.Message;
			if (!Result.Diagnostics.empty())
			{
				if (!Message.empty()) Message += " ";
				Message += Result.Diagnostics.front().Message;
			}
			ReportError(Message.empty()
				? "The material graph command failed." : std::move(Message));
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

	auto FMaterialGraphCanvas::SelectAndFrame(const FGuid& NodeId) -> bool
	{
		if (!NodeId.IsValid()) return false;
		bMaterialOutputSelected = false;
		SelectedSurfaceOutput.reset();
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
			bMaterialOutputSelected = false;
			SelectedNodes.clear();
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
		LinkSourceNode = {};
		PaletteSourceNode = {};
		ContextNode = {};
		ContextSurfaceOutput.reset();
		bPaletteOpenRequested = false;
		PaletteSelection = 0;
		ReconnectDestinationNode = {};
		ReconnectSurfaceOutput.reset();
		InlineEditNode = {};
		bSurfaceDefaultDraftInitialized.fill(false);
		bMarqueeActive = false;
		DragStartPositions.clear();
	}

	auto FMaterialGraphCanvas::PrepareView(
		DMaterial& Material,
		const FReportError& ReportError) -> FMaterialGraphView
	{
		const uint64 AuthoredRevision =
			Material.GetMaterialCompileStatus().AuthoredRevision;
		if (CatalogAuthoredRevision != AuthoredRevision || Catalog.empty())
		{
			Catalog = FMaterialGraphOperations::EnumerateCatalog(Material);
			CatalogAuthoredRevision = AuthoredRevision;
		}
		FMaterialGraphView View = FMaterialGraphOperations::Inspect(Material, Catalog);
		if (std::ranges::any_of(View.Nodes,
			[](const FMaterialGraphNodeView& Node) { return !Node.Presentation; }))
		{
			const FMaterialGraphPresentation& Source =
				Material.GetMaterialGraphPresentation();
			if (!bHasTransientLayout || TransientLayoutSource != Source
				|| TransientLayoutAuthoredRevision != AuthoredRevision)
			{
				const FMaterialGraphCommandResult Layout =
					FMaterialGraphOperations::CalculateLayout(
						Material, {}, TransientLayout);
				ReportCommand(Layout, ReportError);
				bHasTransientLayout = static_cast<bool>(Layout);
				if (bHasTransientLayout)
				{
					TransientLayoutSource = Source;
					TransientLayoutAuthoredRevision = AuthoredRevision;
				}
			}
			if (bHasTransientLayout)
			{
				std::unordered_map<FGuid, FMaterialGraphNodePresentation> Positions;
				for (const FMaterialGraphNodePresentation& Position : TransientLayout.Nodes)
					Positions.emplace(Position.NodeId, Position);
				for (FMaterialGraphNodeView& Node : View.Nodes)
					if (!Node.Presentation)
						if (const auto It = Positions.find(Node.Node.Id);
							It != Positions.end()) Node.Presentation = It->second;
				if (!View.MaterialOutputPosition
					&& TransientLayout.bHasMaterialOutputPosition)
					View.MaterialOutputPosition = {
						TransientLayout.MaterialOutputX,
						TransientLayout.MaterialOutputY};
			}
		}
		if (View.MaterialOutputPosition)
			SurfaceGraphPosition = {
				static_cast<float>(View.MaterialOutputPosition->first),
				static_cast<float>(View.MaterialOutputPosition->second)};
		else if (!SurfaceGraphPosition || SurfaceGraphRevision != AuthoredRevision)
			SurfaceGraphPosition = SurfaceGraphMinimum(View);
		SurfaceGraphRevision = AuthoredRevision;
		return View;
	}

	auto FMaterialGraphCanvas::BuildVisualGraph(
		const FMaterialGraphView& View,
		const ImVec2& CanvasMinimum) const -> FVisualGraph
	{
		FVisualGraph Result;
		Result.Nodes.reserve(View.Nodes.size());
		for (const FMaterialGraphNodeView& Node : View.Nodes)
		{
			if (!Node.Presentation) continue;
			const ImVec2 GraphPosition(
				static_cast<float>(Node.Presentation->X),
				static_cast<float>(Node.Presentation->Y));
			const float NodeHeight = FMaterialGraphGeometry::GetNodeHeight(
				static_cast<uint32>(Node.Inputs.size()));
			FVisualNode Visual;
			Visual.View = &Node;
			Visual.Minimum = Add(CanvasMinimum, Add(Pan, Multiply(GraphPosition, Zoom)));
			Visual.Maximum = Add(Visual.Minimum,
				Multiply({NodeWidth, NodeHeight}, Zoom));
			Visual.OutputPin = {
				Visual.Maximum.x,
				Visual.Minimum.y + (NodeHeaderHeight + Metrics.SecondaryHeight
					+ NodePadding) * Zoom};
			for (size_t Index = 0; Index < Node.Inputs.size(); ++Index)
				Visual.InputPins.push_back({
					Visual.Minimum.x,
					Visual.Minimum.y + (NodeHeaderHeight + Metrics.SecondaryHeight
						+ NodePadding + PinSpacing * Index) * Zoom});
			Result.Indices.emplace(Node.Node.Id, Result.Nodes.size());
			Result.Nodes.push_back(std::move(Visual));
		}
		return Result;
	}

	auto FMaterialGraphCanvas::FrameNodes(
		const FMaterialGraphView& View,
		const ImVec2& CanvasSize) -> void
	{
		bool bFound = false;
		ImVec2 Minimum{};
		ImVec2 Maximum{};
		for (const FMaterialGraphNodeView& Node : View.Nodes)
		{
			if (SelectedSurfaceOutput && SelectedNodes.empty()) continue;
			if (!SelectedNodes.empty() && !SelectedNodes.contains(Node.Node.Id)) continue;
			if (!Node.Presentation) continue;
			const ImVec2 Position(
				static_cast<float>(Node.Presentation->X),
				static_cast<float>(Node.Presentation->Y));
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
		if (SelectedNodes.empty())
		{
			const ImVec2 SurfaceMinimum = SurfaceGraphPosition.value_or(
				SurfaceGraphMinimum(View));
			const ImVec2 SurfaceMaximum = Add(SurfaceMinimum,
				{Metrics.SurfaceWidth, Metrics.SurfaceHeaderHeight
					+ Metrics.PinRowHeight * 8.0f + Metrics.BodyPadding});
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

	auto FMaterialGraphCanvas::Draw(
		DMaterial& Material,
		FTransactionManager& Transactions,
		float Height,
		const FReportError& ReportError) -> void
	{
		ImGui::PushID(this);
		if (ImGui::BeginChild("MaterialGraph", ImVec2(0.0f, Height),
			ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar
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
			ImGui::TextDisabled("%s | Wheel: zoom  MMB: pan  LMB: select/drag  Shift: add/replace",
				DetailName);

			FMaterialGraphView View = PrepareView(Material, ReportError);
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
						Add(CanvasMinimum, {X, CanvasSize.y}), IM_COL32(48, 52, 60, 90));
				for (float Y = std::fmod(Pan.y, GridStep); Y < CanvasSize.y; Y += GridStep)
					DrawList->AddLine(Add(CanvasMinimum, {0.0f, Y}),
						Add(CanvasMinimum, {CanvasSize.x, Y}), IM_COL32(48, 52, 60, 90));
			}

			FVisualGraph VisualGraph = BuildVisualGraph(View, CanvasMinimum);
			const std::vector<FVisualNode>& VisualNodes = VisualGraph.Nodes;
			const std::unordered_map<FGuid, size_t>& VisualIndices =
				VisualGraph.Indices;

			for (const FVisualNode& Destination : VisualNodes)
				for (size_t InputIndex = 0; InputIndex < Destination.View->Inputs.size(); ++InputIndex)
				{
					const auto SourceIt = VisualIndices.find(
						Destination.View->Inputs[InputIndex].Link.SourceNodeId);
					if (SourceIt == VisualIndices.end()) continue;
					const ImVec2 A = VisualNodes[SourceIt->second].OutputPin;
					const ImVec2 B = Destination.InputPins[InputIndex];
					const ImVec2 LinkMinimum(std::min(A.x, B.x), std::min(A.y, B.y));
					const ImVec2 LinkMaximum(std::max(A.x, B.x), std::max(A.y, B.y));
					if (!Intersects(LinkMinimum, LinkMaximum, CanvasMinimum, CanvasMaximum))
						continue;
					const float Tangent = std::max(40.0f, std::abs(B.x - A.x) * 0.45f);
					const bool bFocused = SelectedNodes.empty()
						|| SelectedNodes.contains(Destination.View->Node.Id)
						|| SelectedNodes.contains(VisualNodes[SourceIt->second].View->Node.Id);
					const ImU32 Color = TypeColor(Destination.View->Inputs[InputIndex].SourceType);
					DrawList->AddBezierCubic(A, Add(A, {Tangent, 0.0f}),
						Subtract(B, {Tangent, 0.0f}), B,
						bFocused ? Color : WithAlpha(Color, 72), bFocused ? 3.0f : 1.5f);
				}

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
				const ImVec2 LinkMinimum(std::min(A.x, B.x), std::min(A.y, B.y));
				const ImVec2 LinkMaximum(std::max(A.x, B.x), std::max(A.y, B.y));
				if (!Intersects(LinkMinimum, LinkMaximum, CanvasMinimum, CanvasMaximum))
					continue;
				const float Tangent = std::max(40.0f, std::abs(B.x - A.x) * 0.45f);
				const bool bFocused = SelectedNodes.empty()
					|| SelectedNodes.contains(VisualNodes[SourceIt->second].View->Node.Id)
					|| (SelectedSurfaceOutput && static_cast<size_t>(*SelectedSurfaceOutput) == Index);
				const ImU32 Color = TypeColor(SurfaceTypes[Index]);
				DrawList->AddBezierCubic(A, Add(A, {Tangent, 0.0f}),
					Subtract(B, {Tangent, 0.0f}), B,
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
						|| Visual.View->Node.Opcode == EMaterialProgramOpcode::TextureCoordinate
						|| Visual.View->Node.Opcode == EMaterialProgramOpcode::Swizzle);
				if (DetailLevel == EMaterialGraphDetailLevel::Editing
					&& !bInlineEditorVisible)
				{
					const std::string ResultLabel = TypeName(Visual.View->Node.ResultType);
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
				&& SelectedNodes.size() == 1)
			{
				const auto SelectedIt = VisualIndices.find(*SelectedNodes.begin());
				if (SelectedIt != VisualIndices.end())
				{
					const FVisualNode& Visual = VisualNodes[SelectedIt->second];
					if (Visual.View->Node.Opcode == EMaterialProgramOpcode::Constant
						&& Intersects(Visual.Minimum, Visual.Maximum, CanvasMinimum, CanvasMaximum))
					{
						if (InlineEditNode != Visual.View->Node.Id)
						{
							InlineEditNode = Visual.View->Node.Id;
							InlineConstantDraft = {Visual.View->Node.Literal.X,
								Visual.View->Node.Literal.Y, Visual.View->Node.Literal.Z,
								Visual.View->Node.Literal.W};
						}
						const ImVec2 SavedCursor = ImGui::GetCursorScreenPos();
						ImGui::SetCursorScreenPos(Add(Visual.Minimum,
							{10.0f * Zoom, (NodeHeaderHeight + Metrics.SecondaryHeight
								+ 4.0f) * Zoom}));
						ImGui::PushID(InlineEditNode.ToString().c_str());
						ImGui::SetNextItemWidth(std::max(80.0f,
							(NodeWidth - 20.0f) * Zoom));
						ImGui::PushFont(nullptr, GraphControlFontSizeBase);
						ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
							GraphControlFramePadding);
						ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing,
							GraphControlItemSpacing);
						const bool bValueSubmitted = DrawNumericInputEditor("##InlineConstant",
							Visual.View->Node.ResultType, InlineConstantDraft.data());
						bEmbeddedControlHoveredOrActive |=
							ImGui::IsItemHovered() || ImGui::IsItemActive();
						const bool bInlineActive = ImGui::IsItemActive();
						const bool bCancelInline = ImGui::IsKeyPressed(ImGuiKey_Escape)
							&& (bInlineActive || ImGui::IsItemFocused());
						if (bCancelInline)
							InlineEditNode = {};
						else if (bValueSubmitted || ImGui::IsItemDeactivatedAfterEdit())
						{
							FMaterialProgramNode Edited = Visual.View->Node;
							Edited.Literal = {InlineConstantDraft[0], InlineConstantDraft[1],
								InlineConstantDraft[2], InlineConstantDraft[3]};
							ReportCommand(FMaterialGraphOperations::ReplaceNode(
								Material, std::move(Edited), &Transactions), ReportError);
						}
						if (!bInlineActive) InlineEditNode = {};
						ImGui::PopStyleVar(2);
						ImGui::PopFont();
						ImGui::PopID();
						ImGui::SetCursorScreenPos(SavedCursor);
						ImGui::Dummy({0.0f, 0.0f});
					}
					else if ((Visual.View->Node.Opcode == EMaterialProgramOpcode::Parameter
						|| Visual.View->Node.Opcode == EMaterialProgramOpcode::TextureParameter
						|| Visual.View->Node.Opcode == EMaterialProgramOpcode::TextureCoordinate)
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
							if (InlineEditNode != Visual.View->Node.Id)
							{
								InlineEditNode = Visual.View->Node.Id;
								InlineConstantDraft = {Resolved.Value.ScalarValue,
									static_cast<float>(Resolved.Value.Vector2Value.y),
									static_cast<float>(Resolved.Value.VectorValue.z), 0.0f};
								if (Visual.View->Node.ResultType == EMaterialProgramValueType::Float2)
									InlineConstantDraft = {
										static_cast<float>(Resolved.Value.Vector2Value.x),
										static_cast<float>(Resolved.Value.Vector2Value.y), 0.0f, 0.0f};
								else if (Visual.View->Node.ResultType
									== EMaterialProgramValueType::Float3)
									InlineConstantDraft = {
										static_cast<float>(Resolved.Value.VectorValue.x),
										static_cast<float>(Resolved.Value.VectorValue.y),
										static_cast<float>(Resolved.Value.VectorValue.z), 0.0f};
							}
							const bool bValueChanged = DrawNumericDragEditor("##InlineParameterValue",
								Visual.View->Node.ResultType, InlineConstantDraft.data());
							bEmbeddedControlHoveredOrActive |=
								ImGui::IsItemHovered() || ImGui::IsItemActive();
							const bool bInlineActive = ImGui::IsItemActive();
							const bool bCancelInline = ImGui::IsKeyPressed(ImGuiKey_Escape)
								&& (bInlineActive || ImGui::IsItemFocused());
							if (bCancelInline)
							{
								if (ParameterEditSession.IsActive())
									ReportCommand(ParameterEditSession.Cancel(), ReportError);
								InlineEditNode = {};
							}
							else if (bValueChanged)
							{
								FMaterialParameterValue Value = Resolved.Value;
								if (Visual.View->Node.ResultType
									== EMaterialProgramValueType::Float)
									Value.ScalarValue = InlineConstantDraft[0];
								else if (Visual.View->Node.ResultType
									== EMaterialProgramValueType::Float2)
									Value.Vector2Value = {InlineConstantDraft[0],
										InlineConstantDraft[1]};
								else Value.VectorValue = {InlineConstantDraft[0],
									InlineConstantDraft[1], InlineConstantDraft[2]};
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
								InlineEditNode = {};
							}
							if (!bInlineActive) InlineEditNode = {};
						}
						else
						{
							const char* Preview = Visual.View->SecondaryLabel.empty()
								? "Select parameter" : Visual.View->SecondaryLabel.c_str();
							if (ImGui::BeginCombo("##InlineParameter", Preview))
							{
								for (const FMaterialGraphCatalogEntry& Entry : Catalog)
								{
									if (Entry.NodeTemplate.Opcode != Visual.View->Node.Opcode
										|| !Entry.NodeTemplate.ParameterId.IsValid()) continue;
									if (ImGui::Selectable(Entry.SecondaryName.c_str(),
										Entry.NodeTemplate.ParameterId == Visual.View->Node.ParameterId))
									{
										FMaterialProgramNode Edited = Visual.View->Node;
										Edited.ParameterId = Entry.NodeTemplate.ParameterId;
										Edited.ResultType = Entry.NodeTemplate.ResultType;
										Edited.DisplayName = Entry.SecondaryName;
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
						if (InlineEditNode != Visual.View->Node.Id)
						{
							InlineEditNode = Visual.View->Node.Id;
							InlineSwizzleDraft = {Visual.View->Node.SwizzleX,
								Visual.View->Node.SwizzleY, Visual.View->Node.SwizzleZ,
								Visual.View->Node.SwizzleW};
						}
						const ImVec2 SavedCursor = ImGui::GetCursorScreenPos();
						ImGui::SetCursorScreenPos(Add(Visual.Minimum,
							{10.0f * Zoom, (NodeHeaderHeight + Metrics.SecondaryHeight
								+ 4.0f) * Zoom}));
						ImGui::PushID(InlineEditNode.ToString().c_str());
						ImGui::SetNextItemWidth(std::max(80.0f, (NodeWidth - 20.0f) * Zoom));
						ImGui::PushFont(nullptr, GraphControlFontSizeBase);
						ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
							GraphControlFramePadding);
						ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing,
							GraphControlItemSpacing);
						ImGui::DragInt4("##InlineSwizzle", InlineSwizzleDraft.data(), 0.1f, 0, 3);
						bEmbeddedControlHoveredOrActive |=
							ImGui::IsItemHovered() || ImGui::IsItemActive();
						const bool bInlineActive = ImGui::IsItemActive();
						const bool bCancelInline = ImGui::IsKeyPressed(ImGuiKey_Escape)
							&& (bInlineActive || ImGui::IsItemFocused());
						if (bCancelInline)
							InlineEditNode = {};
						else if (ImGui::IsItemDeactivatedAfterEdit())
						{
							FMaterialProgramNode Edited = Visual.View->Node;
							Edited.SwizzleX = static_cast<uint8>(std::clamp(InlineSwizzleDraft[0], 0, 3));
							Edited.SwizzleY = static_cast<uint8>(std::clamp(InlineSwizzleDraft[1], 0, 3));
							Edited.SwizzleZ = static_cast<uint8>(std::clamp(InlineSwizzleDraft[2], 0, 3));
							Edited.SwizzleW = static_cast<uint8>(std::clamp(InlineSwizzleDraft[3], 0, 3));
							ReportCommand(FMaterialGraphOperations::ReplaceNode(
								Material, std::move(Edited), &Transactions), ReportError);
						}
						if (!bInlineActive) InlineEditNode = {};
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
				ImGui::TextDisabled("Output: %s", TypeName(HoveredNode->View->Node.ResultType));
				ImGui::EndTooltip();
			}
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
				if (DetailLevel == EMaterialGraphDetailLevel::Editing)
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
					if (Index < 8 && !OutputLinks[Index]->SourceNodeId.IsValid())
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
			const bool bOpenPaletteByDoubleClick = bCanvasPointerInteractionAvailable
				&& ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
				&& !HoveredNode && !HoveredOutput && !HoveredInputNode
				&& !HoveredSurfaceOutput && !bHoveredMaterialOutputHeader;
			if (bOpenPaletteByDoubleClick)
			{
				bMarqueeActive = false;
				PaletteSourceNode = {};
				PaletteGraphPosition = Multiply(
					Subtract(Subtract(Mouse, CanvasMinimum), Pan), 1.0f / Zoom);
				bPaletteOpenRequested = true;
			}
			if (bCanvasKeyboardInteractionAvailable && ImGui::IsKeyPressed(ImGuiKey_Escape))
			{
				if (MoveSession.IsActive())
				{
					ReportCommand(MoveSession.Cancel(), ReportError);
					SurfaceGraphPosition.reset();
				}
				ReconnectDestinationNode = {};
				ReconnectSurfaceOutput.reset();
				LinkSourceNode = {};
				bMarqueeActive = false;
				DragStartPositions.clear();
			}

			if (bCanvasPointerInteractionAvailable && !bOpenPaletteByDoubleClick
				&& ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				if (HoveredInputNode)
				{
					ReconnectDestinationNode = HoveredInputNode->View->Node.Id;
					ReconnectDestinationInputIndex = HoveredInputIndex;
				}
				else if (HoveredOutput)
				{
					LinkSourceNode = HoveredOutput->View->Node.Id;
				}
				else if (HoveredSurfaceOutput)
				{
					bMaterialOutputSelected = false;
					SelectedNodes.clear();
					SelectedSurfaceOutput = HoveredSurfaceOutput;
					ReconnectSurfaceOutput = HoveredSurfaceOutput;
				}
				else if (bHoveredMaterialOutputHeader)
				{
					SelectedNodes.clear();
					SelectedSurfaceOutput.reset();
					bMaterialOutputSelected = true;
					const FMaterialGraphCommandResult Begun =
						MoveSession.BeginMaterialOutput(Material, &Transactions);
					ReportCommand(Begun, ReportError);
					if (Begun)
					{
						DragStartMouse = Mouse;
						DragStartMaterialOutput = CurrentSurfaceGraphPosition;
						DragStartPositions.clear();
					}
				}
				else if (HoveredNode)
				{
					const FGuid Id = HoveredNode->View->Node.Id;
					bMaterialOutputSelected = false;
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
						std::vector<FGuid> Selection(SelectedNodes.begin(), SelectedNodes.end());
						const FMaterialGraphCommandResult Begun = MoveSession.Begin(
							Material, Selection, &Transactions);
						ReportCommand(Begun, ReportError);
						if (Begun)
						{
							DragStartMouse = Mouse;
							DragStartPositions.clear();
							for (const FVisualNode& Visual : VisualNodes)
								if (SelectedNodes.contains(Visual.View->Node.Id)
									&& Visual.View->Presentation)
									DragStartPositions.emplace(
										Visual.View->Node.Id, *Visual.View->Presentation);
						}
					}
				}
				else
				{
					if (!ImGui::GetIO().KeyShift)
					{
						bMaterialOutputSelected = false;
						SelectedNodes.clear();
						SelectedSurfaceOutput.reset();
					}
					MarqueeStart = Mouse;
					bMarqueeActive = true;
				}
			}

			if (MoveSession.IsActive()
				&& ImGui::IsMouseDragging(ImGuiMouseButton_Left))
			{
				const ImVec2 Delta = Multiply(Subtract(Mouse, DragStartMouse), 1.0f / Zoom);
				if (bMaterialOutputSelected)
				{
					const ImVec2 Position = Add(DragStartMaterialOutput, Delta);
					ReportCommand(MoveSession.ApplyMaterialOutput(
						static_cast<int32>(std::round(Position.x)),
						static_cast<int32>(std::round(Position.y))), ReportError);
					SurfaceGraphPosition = Position;
				}
				else
				{
					std::vector<FMaterialGraphNodePresentation> Positions;
					for (const auto& [Id, Start] : DragStartPositions)
						Positions.push_back({Id,
							static_cast<int32>(std::round(Start.X + Delta.x)),
							static_cast<int32>(std::round(Start.Y + Delta.y))});
					ReportCommand(MoveSession.Apply(Positions), ReportError);
				}
			}
			if (MoveSession.IsActive() && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
				ReportCommand(MoveSession.Commit(), ReportError);

			if (bMarqueeActive)
			{
				const ImVec2 Minimum(std::min(MarqueeStart.x, Mouse.x),
					std::min(MarqueeStart.y, Mouse.y));
				const ImVec2 Maximum(std::max(MarqueeStart.x, Mouse.x),
					std::max(MarqueeStart.y, Mouse.y));
				DrawList->AddRectFilled(Minimum, Maximum, IM_COL32(70, 140, 220, 35));
				DrawList->AddRect(Minimum, Maximum, IM_COL32(80, 160, 235, 180));
				if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
				{
					for (const FVisualNode& Visual : VisualNodes)
						if (Intersects(Minimum, Maximum, Visual.Minimum, Visual.Maximum))
							SelectedNodes.insert(Visual.View->Node.Id);
					bMarqueeActive = false;
				}
			}

			if (LinkSourceNode.IsValid())
			{
				const auto SourceIt = VisualIndices.find(LinkSourceNode);
				if (SourceIt != VisualIndices.end())
				{
					const ImVec2 A = VisualNodes[SourceIt->second].OutputPin;
					DrawList->AddBezierCubic(A, Add(A, {60.0f, 0.0f}),
						Subtract(Mouse, {60.0f, 0.0f}), Mouse,
						TypeColor(VisualNodes[SourceIt->second].View->Node.ResultType), 2.5f);
				}
				if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
				{
					if (HoveredInputNode)
						ReportCommand(FMaterialGraphOperations::Connect(Material, {
							.SourceNodeId = LinkSourceNode,
							.DestinationNodeId = HoveredInputNode->View->Node.Id,
							.DestinationInputIndex = HoveredInputIndex,
							.bReplaceExisting = ImGui::GetIO().KeyShift,
						}, &Transactions), ReportError);
					else if (HoveredSurfaceOutput)
					{
						if (static_cast<size_t>(*HoveredSurfaceOutput) == 8)
							ReportCommand(FMaterialGraphOperations::AssignAggregateSurface(
								Material, LinkSourceNode, &Transactions), ReportError);
						else ReportCommand(FMaterialGraphOperations::AssignSurfaceOutput(Material, {
							.Output = *HoveredSurfaceOutput,
							.SourceNodeId = LinkSourceNode,
						}, &Transactions), ReportError);
					}
					else if (bHovered)
					{
						PaletteSourceNode = LinkSourceNode;
						PaletteGraphPosition = Multiply(
							Subtract(Subtract(Mouse, CanvasMinimum), Pan), 1.0f / Zoom);
						bPaletteOpenRequested = true;
					}
					LinkSourceNode = {};
				}
			}
			if (ReconnectDestinationNode.IsValid())
			{
				const auto DestinationIt = VisualIndices.find(ReconnectDestinationNode);
				if (DestinationIt != VisualIndices.end()
					&& ReconnectDestinationInputIndex
						< VisualNodes[DestinationIt->second].InputPins.size())
				{
					const ImVec2 A = VisualNodes[DestinationIt->second]
						.InputPins[ReconnectDestinationInputIndex];
					DrawList->AddBezierCubic(A, Subtract(A, {60.0f, 0.0f}),
						Add(Mouse, {60.0f, 0.0f}), Mouse,
						IM_COL32(240, 210, 105, 255), 2.5f);
				}
				if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
				{
					if (HoveredOutput)
						ReportCommand(FMaterialGraphOperations::Connect(Material, {
							.SourceNodeId = HoveredOutput->View->Node.Id,
							.DestinationNodeId = ReconnectDestinationNode,
							.DestinationInputIndex = ReconnectDestinationInputIndex,
							.bReplaceExisting = true,
						}, &Transactions), ReportError);
					ReconnectDestinationNode = {};
				}
			}
			if (ReconnectSurfaceOutput)
			{
				const size_t OutputIndex = static_cast<size_t>(*ReconnectSurfaceOutput);
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
						if (static_cast<size_t>(*ReconnectSurfaceOutput) == 8)
							ReportCommand(FMaterialGraphOperations::AssignAggregateSurface(
								Material, HoveredOutput->View->Node.Id, &Transactions), ReportError);
						else ReportCommand(FMaterialGraphOperations::AssignSurfaceOutput(Material, {
							.Output = *ReconnectSurfaceOutput,
							.SourceNodeId = HoveredOutput->View->Node.Id,
						}, &Transactions), ReportError);
					}
					ReconnectSurfaceOutput.reset();
				}
			}

			if (bCanvasKeyboardInteractionAvailable && ImGui::GetIO().KeyCtrl
				&& ImGui::IsKeyPressed(ImGuiKey_A))
			{
				SelectedNodes.clear();
				for (const FMaterialGraphNodeView& Node : View.Nodes)
					SelectedNodes.insert(Node.Node.Id);
			}
			if (bCanvasKeyboardInteractionAvailable && ImGui::GetIO().KeyCtrl
				&& ImGui::IsKeyPressed(ImGuiKey_C) && !SelectedNodes.empty())
			{
				std::vector<FGuid> Selection(SelectedNodes.begin(), SelectedNodes.end());
				FMaterialGraphClipboardPayload Payload;
				const FMaterialGraphCommandResult Copied =
					FMaterialGraphOperations::CopySelection(Material, Selection, Payload);
				ReportCommand(Copied, ReportError);
				if (Copied) GraphClipboard = std::move(Payload);
			}
			if (bCanvasKeyboardInteractionAvailable && ImGui::GetIO().KeyCtrl
				&& ImGui::IsKeyPressed(ImGuiKey_X) && !SelectedNodes.empty())
			{
				std::vector<FGuid> Selection(SelectedNodes.begin(), SelectedNodes.end());
				FMaterialGraphClipboardPayload Payload;
				const FMaterialGraphCommandResult Cut = FMaterialGraphOperations::CutSelection(
					Material, Selection, Payload, &Transactions);
				ReportCommand(Cut, ReportError);
				if (Cut)
				{
					GraphClipboard = std::move(Payload);
					SelectedNodes.clear();
				}
			}
			if (bCanvasKeyboardInteractionAvailable && ImGui::GetIO().KeyCtrl
				&& ImGui::IsKeyPressed(ImGuiKey_D) && !SelectedNodes.empty())
			{
				std::vector<FGuid> Selection(SelectedNodes.begin(), SelectedNodes.end());
				const FMaterialGraphCommandResult Duplicated =
					FMaterialGraphOperations::DuplicateNodes(
						Material, Selection, 40, 40, &Transactions);
				ReportCommand(Duplicated, ReportError);
				if (Duplicated)
				{
					SelectedNodes.clear();
					SelectedNodes.insert(Duplicated.GeneratedNodeIds.begin(),
						Duplicated.GeneratedNodeIds.end());
				}
			}
			if (bCanvasKeyboardInteractionAvailable && ImGui::GetIO().KeyCtrl
				&& ImGui::IsKeyPressed(ImGuiKey_V) && GraphClipboard)
			{
				const ImVec2 GraphPosition = Multiply(
					Subtract(Subtract(Mouse, CanvasMinimum), Pan), 1.0f / Zoom);
				const FMaterialGraphCommandResult Pasted = FMaterialGraphOperations::Paste(
					Material, *GraphClipboard,
					static_cast<int32>(std::round(GraphPosition.x)),
					static_cast<int32>(std::round(GraphPosition.y)), &Transactions);
				ReportCommand(Pasted, ReportError);
				if (Pasted)
				{
					SelectedNodes.clear();
					SelectedNodes.insert(Pasted.GeneratedNodeIds.begin(),
						Pasted.GeneratedNodeIds.end());
				}
			}
			if (bCanvasKeyboardInteractionAvailable && ImGui::IsKeyPressed(ImGuiKey_Delete)
				&& !SelectedNodes.empty())
			{
				std::vector<FGuid> Selection(SelectedNodes.begin(), SelectedNodes.end());
				const FMaterialGraphCommandResult Removed =
					FMaterialGraphOperations::RemoveNodes(Material, Selection, &Transactions);
				ReportCommand(Removed, ReportError);
				if (Removed) SelectedNodes.clear();
			}
			if (bCanvasKeyboardInteractionAvailable && ImGui::IsKeyPressed(ImGuiKey_F))
				FrameNodes(View, CanvasSize);
			if (bFrameSelectionRequested
				&& (!SelectedNodes.empty() || SelectedSurfaceOutput
					|| bMaterialOutputSelected))
				FrameNodes(View, CanvasSize);
			if (bFrameAllRequested)
			{
				const auto SavedSelection = SelectedNodes;
				const auto SavedSurface = SelectedSurfaceOutput;
				SelectedNodes.clear();
				SelectedSurfaceOutput.reset();
				FrameNodes(View, CanvasSize);
				SelectedNodes = SavedSelection;
				SelectedSurfaceOutput = SavedSurface;
			}
			if (PendingFrameNode.IsValid())
			{
				SelectedNodes = {PendingFrameNode};
				FrameNodes(View, CanvasSize);
				PendingFrameNode = {};
			}
			if (bPendingFrameSurface)
			{
				FrameNodes(View, CanvasSize);
				bPendingFrameSurface = false;
			}
			DetailLevel = FMaterialGraphGeometry::SelectDetailLevel(Zoom, DetailLevel);
			if (bCanvasPointerInteractionAvailable
				&& ImGui::IsMouseClicked(ImGuiMouseButton_Right))
			{
				PaletteSourceNode = HoveredOutput
					? HoveredOutput->View->Node.Id : FGuid{};
				ContextNode = HoveredNode ? HoveredNode->View->Node.Id
					: HoveredInputNode ? HoveredInputNode->View->Node.Id : FGuid{};
				ContextSurfaceOutput = HoveredSurfaceOutput;
				PaletteGraphPosition = Multiply(
					Subtract(Subtract(Mouse, CanvasMinimum), Pan), 1.0f / Zoom);
				ImGui::OpenPopup("MaterialGraphContext");
			}
			if (bCanvasKeyboardInteractionAvailable && ImGui::IsKeyPressed(ImGuiKey_Space))
			{
				PaletteSourceNode = {};
				ContextNode = {};
				ContextSurfaceOutput.reset();
				PaletteGraphPosition = Multiply(
					Subtract(Subtract(Mouse, CanvasMinimum), Pan), 1.0f / Zoom);
				bPaletteOpenRequested = true;
			}

			if (ImGui::BeginPopup("MaterialGraphContext"))
			{
				const auto ContextNodeIt = std::ranges::find(View.Nodes, ContextNode,
					[](const FMaterialGraphNodeView& Node) { return Node.Node.Id; });
				const FMaterialGraphNodeView* ContextNodeView =
					ContextNodeIt == View.Nodes.end() ? nullptr : &*ContextNodeIt;
				if (ContextNodeView)
				{
					std::vector<FGuid> ContextSelection;
					if (SelectedNodes.contains(ContextNodeView->Node.Id))
						ContextSelection.assign(SelectedNodes.begin(), SelectedNodes.end());
					else ContextSelection = {ContextNodeView->Node.Id};
					FMaterialProgramNode Edited = ContextNodeView->Node;
					if (ImGui::BeginMenu("Parameter"))
					{
						for (const FMaterialGraphCatalogEntry& Entry : Catalog)
						{
							if (Entry.NodeTemplate.Opcode != Edited.Opcode
								|| !Entry.NodeTemplate.ParameterId.IsValid()) continue;
							ImGui::PushID(Entry.NodeTemplate.ParameterId.ToString().c_str());
							if (ImGui::MenuItem(Entry.Name.c_str()))
							{
								Edited.ParameterId = Entry.NodeTemplate.ParameterId;
								Edited.ResultType = Entry.NodeTemplate.ResultType;
								Edited.DisplayName = Entry.NodeTemplate.DisplayName;
								ReportCommand(FMaterialGraphOperations::ReplaceNode(
									Material, Edited, &Transactions), ReportError);
							}
							ImGui::PopID();
						}
						ImGui::EndMenu();
					}
					if (ImGui::MenuItem("Copy"))
					{
						FMaterialGraphClipboardPayload Payload;
						const FMaterialGraphCommandResult Copied =
							FMaterialGraphOperations::CopySelection(
								Material, ContextSelection, Payload);
						ReportCommand(Copied, ReportError);
						if (Copied) GraphClipboard = std::move(Payload);
					}
					if (ImGui::MenuItem("Duplicate"))
					{
						const FMaterialGraphCommandResult Duplicated =
							FMaterialGraphOperations::DuplicateNodes(
								Material, ContextSelection, 40, 40, &Transactions);
						ReportCommand(Duplicated, ReportError);
						if (Duplicated)
						{
							SelectedNodes.clear();
							SelectedNodes.insert(Duplicated.GeneratedNodeIds.begin(),
								Duplicated.GeneratedNodeIds.end());
						}
					}
					if (ImGui::MenuItem("Cut"))
					{
						FMaterialGraphClipboardPayload Payload;
						const FMaterialGraphCommandResult Cut =
							FMaterialGraphOperations::CutSelection(
								Material, ContextSelection, Payload, &Transactions);
						ReportCommand(Cut, ReportError);
						if (Cut)
						{
							GraphClipboard = std::move(Payload);
							SelectedNodes.clear();
						}
					}
					if (ImGui::MenuItem("Delete"))
					{
						const FMaterialGraphCommandResult Removed =
							FMaterialGraphOperations::RemoveNodes(
								Material, ContextSelection, &Transactions);
						ReportCommand(Removed, ReportError);
						if (Removed) SelectedNodes.clear();
					}
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
				else
				{
					if (GraphClipboard && ImGui::MenuItem("Paste"))
					{
						const FMaterialGraphCommandResult Pasted =
							FMaterialGraphOperations::Paste(Material, *GraphClipboard,
								static_cast<int32>(std::round(PaletteGraphPosition.x)),
								static_cast<int32>(std::round(PaletteGraphPosition.y)),
								&Transactions);
						ReportCommand(Pasted, ReportError);
						if (Pasted)
						{
							SelectedNodes.clear();
							SelectedNodes.insert(Pasted.GeneratedNodeIds.begin(),
								Pasted.GeneratedNodeIds.end());
						}
					}
					if (ImGui::MenuItem("Auto Layout"))
					{
						const FMaterialGraphCommandResult Layout =
							FMaterialGraphOperations::Layout(
								Material, {}, &Transactions);
						ReportCommand(Layout, ReportError);
						if (Layout) SurfaceGraphPosition.reset();
					}
					if (ImGui::MenuItem("Create Node...", "Space"))
						bPaletteOpenRequested = true;
				}
				ImGui::EndPopup();
			}

			if (bPaletteOpenRequested)
			{
				PaletteSearch.fill('\0');
				PaletteSelection = 0;
				ImGui::OpenPopup("MaterialNodePalette");
				bPaletteOpenRequested = false;
			}
			const ImGuiViewport* MainViewport = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(
				{MainViewport->Pos.x + MainViewport->Size.x * 0.5f,
					MainViewport->Pos.y + MainViewport->Size.y * 0.5f},
				ImGuiCond_Appearing, {0.5f, 0.5f});
			ImGui::SetNextWindowSize({MonaImGui::ScaleUI(660.0f),
				MonaImGui::ScaleUI(520.0f)}, ImGuiCond_Appearing);
			if (ImGui::BeginPopup("MaterialNodePalette",
				ImGuiWindowFlags_NoSavedSettings))
			{
				if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
				ImGui::SetNextItemWidth(-FLT_MIN);
				const bool bSearchSubmitted = ImGui::InputTextWithHint(
					"##NodePaletteSearch", "Search nodes, parameters, categories, or types",
					PaletteSearch.data(), PaletteSearch.size(),
					ImGuiInputTextFlags_EnterReturnsTrue);
				if (ImGui::IsItemEdited()) PaletteSelection = 0;

				std::optional<EMaterialProgramValueType> PaletteSourceType;
				const auto PaletteSource = std::ranges::find(View.Nodes,
					PaletteSourceNode,
					[](const FMaterialGraphNodeView& Node) { return Node.Node.Id; });
				if (PaletteSourceNode.IsValid() && PaletteSource != View.Nodes.end())
					PaletteSourceType = PaletteSource->Node.ResultType;
				std::vector<FMaterialGraphCatalogEntry> PaletteCatalog =
					FMaterialGraphOperations::SearchCatalog(
						Catalog, PaletteSearch.data(), PaletteSourceType);
				if (PaletteSearch.front() == '\0')
				{
					const auto RecentRank = [this](const FMaterialGraphCatalogEntry& Entry) {
						const std::string Key = PaletteEntryKey(Entry);
						const auto It = std::ranges::find(RecentPaletteEntries, Key);
						return It == RecentPaletteEntries.end()
							? RecentPaletteEntries.size()
							: static_cast<size_t>(It - RecentPaletteEntries.begin());
					};
					std::ranges::stable_sort(PaletteCatalog,
						[this, &RecentRank](const FMaterialGraphCatalogEntry& A,
							const FMaterialGraphCatalogEntry& B) {
							const bool bFavoriteA = FavoritePaletteEntries.contains(PaletteEntryKey(A));
							const bool bFavoriteB = FavoritePaletteEntries.contains(PaletteEntryKey(B));
							if (bFavoriteA != bFavoriteB) return bFavoriteA;
							return RecentRank(A) < RecentRank(B);
						});
				}
				PaletteSelection = std::clamp(PaletteSelection, 0,
					std::max(static_cast<int32>(PaletteCatalog.size()) - 1, 0));
				if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && !PaletteCatalog.empty())
					PaletteSelection = std::min(PaletteSelection + 1,
						static_cast<int32>(PaletteCatalog.size()) - 1);
				if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && !PaletteCatalog.empty())
					PaletteSelection = std::max(PaletteSelection - 1, 0);

				if (PaletteSourceType)
					ImGui::TextDisabled("Compatible with %s output", TypeName(*PaletteSourceType));
				else ImGui::TextDisabled("Favorites and recent nodes appear first");
				ImGui::Separator();
				bool bActivateSelection = bSearchSubmitted;
				if (ImGui::BeginChild("NodePaletteResults", {0.0f, -ImGui::GetFrameHeightWithSpacing()}))
				{
					for (size_t EntryIndex = 0; EntryIndex < PaletteCatalog.size(); ++EntryIndex)
					{
						const FMaterialGraphCatalogEntry& Entry = PaletteCatalog[EntryIndex];
						const std::string Key = PaletteEntryKey(Entry);
						ImGui::PushID(static_cast<int>(EntryIndex));
						const bool bFavorite = FavoritePaletteEntries.contains(Key);
						if (ImGui::SmallButton(bFavorite ? "*" : "+"))
						{
							if (bFavorite) FavoritePaletteEntries.erase(Key);
							else FavoritePaletteEntries.insert(Key);
						}
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip(bFavorite ? "Remove from favorites" : "Add to favorites");
						ImGui::SameLine();
						const std::string Label = std::format("{}{}{}  -> {}\n{}\n{} | {}",
							bFavorite ? "* " : "", Entry.OperationName,
							Entry.SecondaryName.empty() ? ""
								: std::format(" - {}", Entry.SecondaryName),
							TypeName(Entry.NodeTemplate.ResultType), Entry.Description,
							Entry.Category,
							FormatInputSignature(Entry));
						if (ImGui::Selectable(Label.c_str(),
							PaletteSelection == static_cast<int32>(EntryIndex),
							ImGuiSelectableFlags_AllowDoubleClick))
						{
							PaletteSelection = static_cast<int32>(EntryIndex);
							bActivateSelection = true;
						}
						ImGui::Separator();
						ImGui::PopID();
					}
				}
				ImGui::EndChild();

				if (bActivateSelection && !PaletteCatalog.empty())
				{
					const FMaterialGraphCatalogEntry& Entry =
						PaletteCatalog[static_cast<size_t>(PaletteSelection)];
					FMaterialProgramNode Candidate = Entry.NodeTemplate;
					if (PaletteSourceType) Candidate.Inputs.front() = {PaletteSourceNode, 0};
					const FMaterialGraphCommandResult Created =
						FMaterialGraphOperations::CreateNodeWithDefaultInputs(Material, {
							.Node = std::move(Candidate),
							.X = static_cast<int32>(std::round(PaletteGraphPosition.x)),
							.Y = static_cast<int32>(std::round(PaletteGraphPosition.y)),
						}, Entry.AcceptedInputTypes, &Transactions);
					ReportCommand(Created, ReportError);
					if (Created)
					{
						if (!Created.GeneratedNodeIds.empty())
							SelectedNodes = {Created.GeneratedNodeIds.front()};
						const std::string Key = PaletteEntryKey(Entry);
						std::erase(RecentPaletteEntries, Key);
						RecentPaletteEntries.insert(RecentPaletteEntries.begin(), Key);
						if (RecentPaletteEntries.size() > 8) RecentPaletteEntries.resize(8);
						PaletteSourceNode = {};
						ImGui::CloseCurrentPopup();
					}
				}
				if (ImGui::IsKeyPressed(ImGuiKey_Escape))
				{
					PaletteSourceNode = {};
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				ImGui::TextDisabled("Up/Down navigate   Enter create   Esc close");
				ImGui::EndPopup();
			}
			else PaletteSourceNode = {};

			DrawList->PopClipRect();
		}
		ImGui::EndChild();
		ImGui::PopID();
	}
}
