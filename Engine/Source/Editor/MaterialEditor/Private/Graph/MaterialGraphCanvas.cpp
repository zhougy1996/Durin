#include "Graph/MaterialGraphCanvas.h"

#include "Editor/Transaction.h"
#include "MonaImGui.h"

namespace Durin::Editor::Material
{
	namespace
	{
		std::optional<FMaterialGraphClipboardPayload> GraphClipboard;

		constexpr float NodeWidth = 190.0f;
		constexpr float NodeHeaderHeight = 30.0f;
		constexpr float PinSpacing = 24.0f;
		constexpr float NodePadding = 12.0f;

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
			}
			return IM_COL32_WHITE;
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
			}
			return "Unknown";
		}

		auto SurfaceLinks(const FMaterialSurfaceOutputs& Outputs)
			-> std::array<const FMaterialProgramLink*, 8>
		{
			return {&Outputs.BaseColor, &Outputs.Normal, &Outputs.Metallic,
				&Outputs.Roughness, &Outputs.AmbientOcclusion, &Outputs.Emissive,
				&Outputs.Opacity, &Outputs.OpacityMask};
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

	auto FMaterialGraphCanvas::SelectAndFrame(const FGuid& NodeId) -> bool
	{
		if (!NodeId.IsValid()) return false;
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
			if (Diagnostic.LocationIndex >= 8) return false;
			SelectedNodes.clear();
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
		if (MoveSession.IsActive()) MoveSession.Cancel();
		LinkSourceNode = {};
		bMarqueeActive = false;
		DragStartPositions.clear();
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
			if (!SelectedNodes.empty() && !SelectedNodes.contains(Node.Node.Id)) continue;
			if (!Node.Presentation) continue;
			const ImVec2 Position(
				static_cast<float>(Node.Presentation->X),
				static_cast<float>(Node.Presentation->Y));
			const float Height = NodeHeaderHeight + NodePadding * 2.0f
				+ PinSpacing * std::max<size_t>(1, Node.Inputs.size());
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
			const bool bFrameRequested = ImGui::Button("Frame");
			ImGui::SameLine();
			if (ImGui::Button("Auto Layout"))
				ReportCommand(FMaterialGraphService::Layout(
					Material, {}, &Transactions), ReportError);
			ImGui::SameLine();
			ImGui::TextDisabled("Wheel: zoom  MMB: pan  LMB: select/drag  Shift: add/replace");

			FMaterialGraphView View = FMaterialGraphService::Inspect(Material);
			if (std::ranges::any_of(View.Nodes,
				[](const FMaterialGraphNodeView& Node) { return !Node.Presentation; }))
			{
				const FMaterialGraphCommandResult Layout =
					FMaterialGraphService::Layout(Material, {}, &Transactions);
				ReportCommand(Layout, ReportError);
				if (Layout) View = FMaterialGraphService::Inspect(Material);
			}
			const std::vector<FMaterialGraphCatalogEntry> Catalog =
				FMaterialGraphService::EnumerateCatalog(Material);

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
			if (bHovered && ImGui::GetIO().MouseWheel != 0.0f)
			{
				const ImVec2 GraphUnderMouse = Multiply(
					Subtract(Subtract(Mouse, CanvasMinimum), Pan), 1.0f / Zoom);
				Zoom = std::clamp(Zoom * (ImGui::GetIO().MouseWheel > 0.0f ? 1.12f : 0.89f),
					0.25f, 2.0f);
				Pan = Subtract(Subtract(Mouse, CanvasMinimum),
					Multiply(GraphUnderMouse, Zoom));
			}
			if (bHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
				Pan = Add(Pan, ImGui::GetIO().MouseDelta);

			const float GridStep = 32.0f * Zoom;
			if (GridStep >= 8.0f)
			{
				for (float X = std::fmod(Pan.x, GridStep); X < CanvasSize.x; X += GridStep)
					DrawList->AddLine(Add(CanvasMinimum, {X, 0.0f}),
						Add(CanvasMinimum, {X, CanvasSize.y}), IM_COL32(48, 52, 60, 90));
				for (float Y = std::fmod(Pan.y, GridStep); Y < CanvasSize.y; Y += GridStep)
					DrawList->AddLine(Add(CanvasMinimum, {0.0f, Y}),
						Add(CanvasMinimum, {CanvasSize.x, Y}), IM_COL32(48, 52, 60, 90));
			}

			std::vector<FVisualNode> VisualNodes;
			VisualNodes.reserve(View.Nodes.size());
			std::unordered_map<FGuid, size_t> VisualIndices;
			for (const FMaterialGraphNodeView& Node : View.Nodes)
			{
				if (!Node.Presentation) continue;
				const ImVec2 GraphPosition(
					static_cast<float>(Node.Presentation->X),
					static_cast<float>(Node.Presentation->Y));
				const float NodeHeight = NodeHeaderHeight + NodePadding * 2.0f
					+ PinSpacing * std::max<size_t>(1, Node.Inputs.size());
				FVisualNode Visual;
				Visual.View = &Node;
				Visual.Minimum = Add(CanvasMinimum, Add(Pan, Multiply(GraphPosition, Zoom)));
				Visual.Maximum = Add(Visual.Minimum,
					Multiply({NodeWidth, NodeHeight}, Zoom));
				Visual.OutputPin = {
					Visual.Maximum.x,
					Visual.Minimum.y + (NodeHeaderHeight + NodePadding) * Zoom};
				for (size_t Index = 0; Index < Node.Inputs.size(); ++Index)
					Visual.InputPins.push_back({
						Visual.Minimum.x,
						Visual.Minimum.y + (NodeHeaderHeight + NodePadding
							+ PinSpacing * Index) * Zoom});
				VisualIndices.emplace(Node.Node.Id, VisualNodes.size());
				VisualNodes.push_back(std::move(Visual));
			}

			for (const FVisualNode& Destination : VisualNodes)
				for (size_t InputIndex = 0; InputIndex < Destination.View->Inputs.size(); ++InputIndex)
				{
					const auto SourceIt = VisualIndices.find(
						Destination.View->Inputs[InputIndex].Link.SourceNodeId);
					if (SourceIt == VisualIndices.end()) continue;
					const ImVec2 A = VisualNodes[SourceIt->second].OutputPin;
					const ImVec2 B = Destination.InputPins[InputIndex];
					const float Tangent = std::max(40.0f, std::abs(B.x - A.x) * 0.45f);
					DrawList->AddBezierCubic(A, Add(A, {Tangent, 0.0f}),
						Subtract(B, {Tangent, 0.0f}), B,
						TypeColor(Destination.View->Inputs[InputIndex].SourceType), 2.0f);
				}

			constexpr std::array SurfaceNames{
				"Base Color", "Normal", "Metallic", "Roughness",
				"Ambient Occlusion", "Emissive", "Opacity", "Opacity Mask"};
			constexpr std::array SurfaceTypes{
				EMaterialProgramValueType::Float3,
				EMaterialProgramValueType::Float3,
				EMaterialProgramValueType::Float,
				EMaterialProgramValueType::Float,
				EMaterialProgramValueType::Float,
				EMaterialProgramValueType::Float3,
				EMaterialProgramValueType::Float,
				EMaterialProgramValueType::Float};
			const ImVec2 SurfaceMinimum(CanvasMaximum.x - 210.0f, CanvasMinimum.y + 32.0f);
			const ImVec2 SurfaceMaximum(CanvasMaximum.x - 16.0f,
				SurfaceMinimum.y + 38.0f + PinSpacing * SurfaceNames.size());
			std::array<ImVec2, 8> SurfacePins;
			const auto OutputLinks = SurfaceLinks(View.Outputs);
			for (size_t Index = 0; Index < SurfacePins.size(); ++Index)
			{
				SurfacePins[Index] = {SurfaceMinimum.x,
					SurfaceMinimum.y + 38.0f + PinSpacing * Index};
				const auto SourceIt = VisualIndices.find(OutputLinks[Index]->SourceNodeId);
				if (SourceIt == VisualIndices.end()) continue;
				const ImVec2 A = VisualNodes[SourceIt->second].OutputPin;
				const ImVec2 B = SurfacePins[Index];
				const float Tangent = std::max(40.0f, std::abs(B.x - A.x) * 0.45f);
				DrawList->AddBezierCubic(A, Add(A, {Tangent, 0.0f}),
					Subtract(B, {Tangent, 0.0f}), B, TypeColor(SurfaceTypes[Index]), 2.0f);
			}

			const FVisualNode* HoveredNode = nullptr;
			const FVisualNode* HoveredOutput = nullptr;
			const FVisualNode* HoveredInputNode = nullptr;
			uint32 HoveredInputIndex = 0;
			std::optional<EMaterialSurfaceOutput> HoveredSurfaceOutput;
			std::optional<EMaterialProgramValueType> LinkSourceType;
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
				std::string Label = Visual.View->Node.DisplayName;
				if (Label.empty())
				{
					const auto It = std::ranges::find_if(Catalog,
						[&](const FMaterialGraphCatalogEntry& Entry) {
							return Entry.NodeTemplate.Opcode == Visual.View->Node.Opcode
								&& Entry.NodeTemplate.ResultType == Visual.View->Node.ResultType;
						});
					Label = It == Catalog.end() ? "Material Node" : It->Name;
				}
				DrawList->AddText(Add(Visual.Minimum, {8.0f, 7.0f}),
					IM_COL32(235, 238, 242, 255), Label.c_str());
				DrawList->AddCircleFilled(Visual.OutputPin, 5.0f,
					TypeColor(Visual.View->Node.ResultType));
				for (size_t Index = 0; Index < Visual.InputPins.size(); ++Index)
				{
					DrawList->AddCircleFilled(Visual.InputPins[Index], 5.0f,
						TypeColor(Visual.View->Inputs[Index].SourceType));
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
					if (std::hypot(Mouse.x - Visual.InputPins[Index].x,
						Mouse.y - Visual.InputPins[Index].y) <= 8.0f)
					{
						HoveredInputNode = &Visual;
						HoveredInputIndex = static_cast<uint32>(Index);
					}
				}
				if (std::hypot(Mouse.x - Visual.OutputPin.x,
					Mouse.y - Visual.OutputPin.y) <= 8.0f) HoveredOutput = &Visual;
				if (Contains(Visual.Minimum, Visual.Maximum, Mouse)) HoveredNode = &Visual;
			}
			DrawList->AddRectFilled(SurfaceMinimum, SurfaceMaximum,
				IM_COL32(38, 42, 50, 245), 6.0f);
			DrawList->AddRect(SurfaceMinimum, SurfaceMaximum,
				IM_COL32(92, 100, 116, 255), 6.0f);
			DrawList->AddText(Add(SurfaceMinimum, {10.0f, 9.0f}),
				IM_COL32(235, 238, 242, 255), "Surface Outputs");
			for (size_t Index = 0; Index < SurfacePins.size(); ++Index)
			{
				if (SelectedSurfaceOutput
					&& static_cast<size_t>(*SelectedSurfaceOutput) == Index)
					DrawList->AddRectFilled(
						{SurfaceMinimum.x + 2.0f, SurfacePins[Index].y - 10.0f},
						{SurfaceMaximum.x - 2.0f, SurfacePins[Index].y + 10.0f},
						IM_COL32(190, 145, 55, 75));
				DrawList->AddCircleFilled(SurfacePins[Index], 5.0f,
					TypeColor(SurfaceTypes[Index]));
				DrawList->AddText(Add(SurfacePins[Index], {10.0f, -7.0f}),
					IM_COL32(210, 214, 222, 255), SurfaceNames[Index]);
				if (std::hypot(Mouse.x - SurfacePins[Index].x,
					Mouse.y - SurfacePins[Index].y) <= 8.0f)
					HoveredSurfaceOutput = static_cast<EMaterialSurfaceOutput>(Index);
			}

			if (bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				if (HoveredOutput)
				{
					LinkSourceNode = HoveredOutput->View->Node.Id;
				}
				else if (HoveredNode)
				{
					const FGuid Id = HoveredNode->View->Node.Id;
					if (ImGui::GetIO().KeyCtrl)
					{
						if (!SelectedNodes.erase(Id)) SelectedNodes.insert(Id);
					}
					else if (!SelectedNodes.contains(Id)) SelectedNodes = {Id};
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
				else
				{
					if (!ImGui::GetIO().KeyShift) SelectedNodes.clear();
					MarqueeStart = Mouse;
					bMarqueeActive = true;
				}
			}

			if (MoveSession.IsActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
			{
				const ImVec2 Delta = Multiply(Subtract(Mouse, DragStartMouse), 1.0f / Zoom);
				std::vector<FMaterialGraphNodePresentation> Positions;
				for (const auto& [Id, Start] : DragStartPositions)
					Positions.push_back({Id,
						static_cast<int32>(std::round(Start.X + Delta.x)),
						static_cast<int32>(std::round(Start.Y + Delta.y))});
				ReportCommand(MoveSession.Apply(Positions), ReportError);
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
						ReportCommand(FMaterialGraphService::Connect(Material, {
							.SourceNodeId = LinkSourceNode,
							.DestinationNodeId = HoveredInputNode->View->Node.Id,
							.DestinationInputIndex = HoveredInputIndex,
							.bReplaceExisting = ImGui::GetIO().KeyShift,
						}, &Transactions), ReportError);
					else if (HoveredSurfaceOutput)
						ReportCommand(FMaterialGraphService::AssignSurfaceOutput(Material, {
							.Output = *HoveredSurfaceOutput,
							.SourceNodeId = LinkSourceNode,
						}, &Transactions), ReportError);
					LinkSourceNode = {};
				}
			}

			if (bHovered && ImGui::GetIO().KeyCtrl
				&& ImGui::IsKeyPressed(ImGuiKey_A))
			{
				SelectedNodes.clear();
				for (const FMaterialGraphNodeView& Node : View.Nodes)
					SelectedNodes.insert(Node.Node.Id);
			}
			if (bHovered && ImGui::GetIO().KeyCtrl
				&& ImGui::IsKeyPressed(ImGuiKey_C) && !SelectedNodes.empty())
			{
				std::vector<FGuid> Selection(SelectedNodes.begin(), SelectedNodes.end());
				FMaterialGraphClipboardPayload Payload;
				const FMaterialGraphCommandResult Copied =
					FMaterialGraphService::CopySelection(Material, Selection, Payload);
				ReportCommand(Copied, ReportError);
				if (Copied) GraphClipboard = std::move(Payload);
			}
			if (bHovered && ImGui::GetIO().KeyCtrl
				&& ImGui::IsKeyPressed(ImGuiKey_X) && !SelectedNodes.empty())
			{
				std::vector<FGuid> Selection(SelectedNodes.begin(), SelectedNodes.end());
				FMaterialGraphClipboardPayload Payload;
				const FMaterialGraphCommandResult Cut = FMaterialGraphService::CutSelection(
					Material, Selection, Payload, &Transactions);
				ReportCommand(Cut, ReportError);
				if (Cut)
				{
					GraphClipboard = std::move(Payload);
					SelectedNodes.clear();
				}
			}
			if (bHovered && ImGui::GetIO().KeyCtrl
				&& ImGui::IsKeyPressed(ImGuiKey_D) && !SelectedNodes.empty())
			{
				std::vector<FGuid> Selection(SelectedNodes.begin(), SelectedNodes.end());
				const FMaterialGraphCommandResult Duplicated =
					FMaterialGraphService::DuplicateNodes(
						Material, Selection, 40, 40, &Transactions);
				ReportCommand(Duplicated, ReportError);
				if (Duplicated)
				{
					SelectedNodes.clear();
					SelectedNodes.insert(Duplicated.GeneratedNodeIds.begin(),
						Duplicated.GeneratedNodeIds.end());
				}
			}
			if (bHovered && ImGui::GetIO().KeyCtrl
				&& ImGui::IsKeyPressed(ImGuiKey_V) && GraphClipboard)
			{
				const ImVec2 GraphPosition = Multiply(
					Subtract(Subtract(Mouse, CanvasMinimum), Pan), 1.0f / Zoom);
				const FMaterialGraphCommandResult Pasted = FMaterialGraphService::Paste(
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
			if (bHovered && ImGui::IsKeyPressed(ImGuiKey_Delete)
				&& !SelectedNodes.empty())
			{
				std::vector<FGuid> Selection(SelectedNodes.begin(), SelectedNodes.end());
				const FMaterialGraphCommandResult Removed =
					FMaterialGraphService::RemoveNodes(Material, Selection, &Transactions);
				ReportCommand(Removed, ReportError);
				if (Removed) SelectedNodes.clear();
			}
			if (bHovered && ImGui::IsKeyPressed(ImGuiKey_F))
				FrameNodes(View, CanvasSize);
			if (bFrameRequested) FrameNodes(View, CanvasSize);
			if (PendingFrameNode.IsValid())
			{
				SelectedNodes = {PendingFrameNode};
				FrameNodes(View, CanvasSize);
				PendingFrameNode = {};
			}

			if (ImGui::BeginPopupContextItem("MaterialGraphContext"))
			{
				const ImVec2 GraphPosition = Multiply(
					Subtract(Subtract(ImGui::GetIO().MousePos, CanvasMinimum), Pan),
					1.0f / Zoom);
				if (HoveredNode)
				{
					std::vector<FGuid> ContextSelection;
					if (SelectedNodes.contains(HoveredNode->View->Node.Id))
						ContextSelection.assign(SelectedNodes.begin(), SelectedNodes.end());
					else ContextSelection = {HoveredNode->View->Node.Id};
					FMaterialProgramNode Edited = HoveredNode->View->Node;
					if (Edited.Opcode == EMaterialProgramOpcode::Constant)
					{
						float Value[4]{Edited.Literal.X, Edited.Literal.Y,
							Edited.Literal.Z, Edited.Literal.W};
						if (ImGui::DragFloat4("Value", Value, 0.01f))
						{
							Edited.Literal = {Value[0], Value[1], Value[2], Value[3]};
							ReportCommand(FMaterialGraphService::ReplaceNode(
								Material, Edited, &Transactions), ReportError);
						}
					}
					if (ImGui::BeginMenu("Parameter"))
					{
						for (const FMaterialGraphCatalogEntry& Entry
							: FMaterialGraphService::EnumerateCatalog(Material))
						{
							if (Entry.NodeTemplate.Opcode != Edited.Opcode
								|| !Entry.NodeTemplate.ParameterId.IsValid()) continue;
							ImGui::PushID(Entry.NodeTemplate.ParameterId.ToString().c_str());
							if (ImGui::MenuItem(Entry.Name.c_str()))
							{
								Edited.ParameterId = Entry.NodeTemplate.ParameterId;
								Edited.ResultType = Entry.NodeTemplate.ResultType;
								Edited.DisplayName = Entry.NodeTemplate.DisplayName;
								ReportCommand(FMaterialGraphService::ReplaceNode(
									Material, Edited, &Transactions), ReportError);
							}
							ImGui::PopID();
						}
						ImGui::EndMenu();
					}
					if (HoveredInputNode == HoveredNode
						&& ImGui::MenuItem(std::format(
							"Disconnect Input {}", HoveredInputIndex).c_str()))
						ReportCommand(FMaterialGraphService::DisconnectInput(
							Material, HoveredNode->View->Node.Id,
							HoveredInputIndex, &Transactions), ReportError);
					if (ImGui::MenuItem("Copy"))
					{
						FMaterialGraphClipboardPayload Payload;
						const FMaterialGraphCommandResult Copied =
							FMaterialGraphService::CopySelection(
								Material, ContextSelection, Payload);
						ReportCommand(Copied, ReportError);
						if (Copied) GraphClipboard = std::move(Payload);
					}
					if (ImGui::MenuItem("Duplicate"))
					{
						const FMaterialGraphCommandResult Duplicated =
							FMaterialGraphService::DuplicateNodes(
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
							FMaterialGraphService::CutSelection(
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
						const FGuid Id = HoveredNode->View->Node.Id;
						ReportCommand(FMaterialGraphService::RemoveNodes(
							Material, std::span(&Id, 1), &Transactions), ReportError);
					}
				}
				else
				{
					if (GraphClipboard && ImGui::MenuItem("Paste"))
					{
						const FMaterialGraphCommandResult Pasted =
							FMaterialGraphService::Paste(Material, *GraphClipboard,
								static_cast<int32>(std::round(GraphPosition.x)),
								static_cast<int32>(std::round(GraphPosition.y)),
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
						ReportCommand(FMaterialGraphService::Layout(
							Material, {}, &Transactions), ReportError);
					if (ImGui::BeginMenu("Create Node"))
					{
						const std::vector<FMaterialGraphCatalogEntry> Catalog =
							FMaterialGraphService::EnumerateCatalog(Material);
						for (size_t EntryIndex = 0; EntryIndex < Catalog.size(); ++EntryIndex)
						{
							const FMaterialGraphCatalogEntry& Entry = Catalog[EntryIndex];
							FMaterialProgramNode Candidate = Entry.NodeTemplate;
							bool bCanCreate = true;
							for (size_t InputIndex = 0;
								InputIndex < Entry.AcceptedInputTypes.size(); ++InputIndex)
							{
								const auto Source = std::ranges::find_if(View.Nodes,
									[&](const FMaterialGraphNodeView& Node) {
										return std::ranges::find(
											Entry.AcceptedInputTypes[InputIndex],
											Node.Node.ResultType)
											!= Entry.AcceptedInputTypes[InputIndex].end();
									});
								if (Source == View.Nodes.end()) { bCanCreate = false; break; }
								Candidate.Inputs[InputIndex] = {Source->Node.Id, 0};
							}
							ImGui::PushID(static_cast<int>(EntryIndex));
							const std::string Label = std::format("{} ({})",
								Entry.Name, TypeName(Entry.NodeTemplate.ResultType));
							if (!bCanCreate) ImGui::BeginDisabled();
							if (ImGui::MenuItem(Label.c_str()))
								ReportCommand(FMaterialGraphService::CreateNode(Material, {
									.Node = std::move(Candidate),
									.X = static_cast<int32>(std::round(GraphPosition.x)),
									.Y = static_cast<int32>(std::round(GraphPosition.y)),
								}, &Transactions), ReportError);
							if (!bCanCreate) ImGui::EndDisabled();
							ImGui::PopID();
						}
						ImGui::EndMenu();
					}
				}
				ImGui::EndPopup();
			}

			DrawList->PopClipRect();
		}
		ImGui::EndChild();
		ImGui::PopID();
	}
}
