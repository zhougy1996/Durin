#include "Panels/RenderingDiagnosticsPanel.h"

#include "Client/SceneViewport.h"
#include "Editor/WorkspaceUI.h"
#include "EngineGlobals.h"
#include "MonaImGui.h"
#include "Panels/SceneViewportPanel.h"
#include "RDG.h"
#include "Viewport/ViewportPresentation.h"
#include "Workspace/LevelEditorWorkspace.h"
#include "Misc/StringHelper.h"

namespace Durin::Editor::Level
{
	namespace
	{
		auto PassTypeLabel(ERDGPassType Type) -> const char*
		{
			switch (Type)
			{
			case ERDGPassType::Graphics: return "Graphics";
			case ERDGPassType::Compute: return "Compute";
			case ERDGPassType::Copy: return "Copy";
			}
			return "Unknown";
		}

		auto UseLabel(ERDGUse Use) -> const char*
		{
			switch (Use)
			{
			case ERDGUse::Read: return "Read";
			case ERDGUse::Write: return "Write";
			case ERDGUse::ReadWrite: return "Read / Write";
			}
			return "Unknown";
		}

		auto DependencyKindLabel(ERDGDependencyKind Kind) -> const char*
		{
			switch (Kind)
			{
			case ERDGDependencyKind::Value: return "Value";
			case ERDGDependencyKind::Execution: return "Execution";
			case ERDGDependencyKind::Explicit: return "Explicit";
			}
			return "Unknown";
		}

		auto DrawValueRow(const char* Label, std::string_view Value,
			const char* Tooltip = nullptr) -> void
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextDisabled("%s", Label);
			if (Tooltip != nullptr && ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", Tooltip);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(Value.data(), Value.data() + Value.size());
		}

		template<typename Value>
		auto DrawCountRow(const char* Label, Value Count) -> void
		{
			DrawValueRow(Label, std::format("{}", Count));
		}

		auto DrawMetricTableBegin(const char* Id) -> bool
		{
			if (!ImGui::BeginTable(Id, 2,
				ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH
				| ImGuiTableFlags_SizingStretchProp)) return false;
			const float AvailableWidth = ImGui::GetContentRegionAvail().x;
			const float ValueWidth = std::clamp(
				AvailableWidth * 0.36f,
				MonaImGui::ScaleUI(180.0f),
				MonaImGui::ScaleUI(360.0f));
			ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed,
				ValueWidth);
			return true;
		}

		auto ContainsCaseInsensitive(
			std::string_view Text, std::string_view Filter) -> bool
		{
			return StringUtils::ContainsInsensitive(Text, Filter);
		}

		auto DrawBudgetRow(
			const char* Label,
			uint64 Actual,
			uint64 RegressionLimit,
			uint64 SafetyLimit,
			bool bRegressionExceeded) -> void
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(Label);
			ImGui::TableNextColumn();
			if (bRegressionExceeded)
				ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.2f, 1.0f),
					"%llu / %llu regression",
					static_cast<unsigned long long>(Actual),
					static_cast<unsigned long long>(RegressionLimit));
			else
				ImGui::Text("%llu / %llu regression",
					static_cast<unsigned long long>(Actual),
					static_cast<unsigned long long>(RegressionLimit));
			ImGui::SameLine();
			ImGui::TextDisabled("  safety %llu",
				static_cast<unsigned long long>(SafetyLimit));
		}
	} // namespace

	FRenderingDiagnosticsPanel::FRenderingDiagnosticsPanel(
		FSceneViewportPanel& InViewportPanel)
		: ILevelEditorPanel(false)
		, ViewportPanel(InViewportPanel)
	{
	}

	auto FRenderingDiagnosticsPanel::RequestCapture() -> void
	{
		ViewportPanel.RequestRenderGraphCapture();
		bCapturePending = true;
	}

	auto FRenderingDiagnosticsPanel::Draw(FLevelEditorContext&) -> void
	{
		ImGui::SetNextWindowSize(
			ImVec2(MonaImGui::ScaleUI(980.0f), MonaImGui::ScaleUI(700.0f)),
			ImGuiCond_FirstUseEver);
		if (!::Durin::Editor::WorkspaceUI::BeginDockablePanel(
			Workspace::Type, "Rendering Diagnostics", "RenderingDiagnostics",
			GetOpenPtr()))
		{
			ImGui::End();
			return;
		}

		const FSceneViewportStatisticsSnapshot Statistics =
			ViewportPanel.GetRenderStatisticsSnapshot();
		const FSceneViewportRenderGraphSnapshot Graph =
			ViewportPanel.GetRenderGraphSnapshot();
		if (Graph.Revision != ObservedCaptureRevision)
		{
			ObservedCaptureRevision = Graph.Revision;
			bCapturePending = false;
			if (!Graph.bAvailable) SelectedPass = -1;
		}
		if (!Graph.bAvailable && !bCapturePending) RequestCapture();

		const bool bHasCapture = Graph.bAvailable && Graph.Capture != nullptr;
		if (ImGui::Button(bCapturePending ? "Capture pending" : "Capture next frame"))
			RequestCapture();
		const char* CaptureStatus = bHasCapture
			? "Captured frame graph"
			: bCapturePending ? "Waiting for the next rendered frame..."
				: "Frame graph unavailable";
		ImGui::SameLine();
		ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
			ImGui::GetWindowContentRegionMax().x
				- ImGui::CalcTextSize(CaptureStatus).x));
		if (bHasCapture)
			ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.45f, 1.0f), "%s",
				CaptureStatus);
		else if (bCapturePending)
			ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.2f, 1.0f), "%s",
				CaptureStatus);
		else
			ImGui::TextDisabled("%s", CaptureStatus);

		const FRDGCapture* Capture = bHasCapture
			? Graph.Capture.get() : nullptr;
		if (MonaImGui::BeginContentTabBar("RenderingDiagnosticsTabs"))
		{
			if (ImGui::BeginTabItem("Overview"))
			{
				DrawOverview(Statistics, Capture);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Scene"))
			{
				DrawScene(Statistics);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Render Graph"))
			{
				DrawRenderGraph(Capture);
				ImGui::EndTabItem();
			}
			MonaImGui::EndContentTabBar();
		}
		ImGui::End();
	}

	auto FRenderingDiagnosticsPanel::DrawOverview(
		const FSceneViewportStatisticsSnapshot& Snapshot,
		const FRDGCapture* Capture) -> void
	{
		ImGui::Spacing();
		ImGui::TextUnformatted("Frame");
		if (!Snapshot.bAvailable)
		{
			ImGui::TextDisabled("Scene statistics are unavailable.");
		}
		else if (DrawMetricTableBegin("OverviewFrame"))
		{
			const FSceneViewStatistics& Statistics = Snapshot.Statistics;
			const FEngineFrameTiming& Timing = GetStableEditorFrameTiming();
			DrawValueRow("Frame interval", std::format("{:.2f} ms",
				Timing.FrameIntervalMilliseconds),
				"Wall-clock interval between completed engine frames, including work and waits.");
			DrawValueRow("Game-thread work", std::format("{:.2f} ms",
				Timing.GameThreadWorkMilliseconds),
				"Frame interval outside the measured end-of-frame render synchronization wait.");
			DrawValueRow("Render sync wait", std::format("{:.2f} ms",
				Timing.RenderSyncWaitMilliseconds),
				"Time blocked on render-thread pacing; may include RHI, GPU, Present, and VSync backlog.");
			DrawCountRow("Triangles", Statistics.Summary.Triangles);
			DrawCountRow("Draw calls", Statistics.Summary.DrawCalls);
			DrawValueRow("Visible primitives", std::format("{} / {}",
				Statistics.Visibility.VisiblePrimitives,
				Statistics.Visibility.SubmittedPrimitives));
			ImGui::EndTable();
		}

		ImGui::Spacing();
		ImGui::SeparatorText("Render Graph");
		if (Capture == nullptr)
		{
			ImGui::TextDisabled("Capture a frame to inspect graph structure and budgets.");
			return;
		}
		const FRDGStatistics& Statistics = Capture->Statistics;
		if (DrawMetricTableBegin("OverviewRenderGraph"))
		{
			DrawValueRow("Passes", std::format("{} scheduled / {} declared",
				Statistics.ScheduledPasses, Statistics.DeclaredPasses));
			DrawCountRow("Culled passes", Statistics.CulledPasses);
			DrawCountRow("Dependencies", Statistics.Dependencies);
			DrawCountRow("Texture transitions", Statistics.TextureTransitions);
			DrawCountRow("Buffer transitions", Statistics.BufferTransitions);
			DrawValueRow("Compile CPU", std::format("{} us",
				Statistics.CompileMicroseconds));
			DrawValueRow("Execute CPU", std::format("{} us",
				Statistics.ExecuteMicroseconds));
			ImGui::EndTable();
		}

		ImGui::Spacing();
		ImGui::SeparatorText("Structural budgets");
		if (ImGui::BeginTable("RenderGraphBudgets", 2,
			ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH
			| ImGuiTableFlags_SizingStretchProp))
		{
			const float AvailableWidth = ImGui::GetContentRegionAvail().x;
			const float ValueWidth = std::clamp(
				AvailableWidth * 0.50f,
				MonaImGui::ScaleUI(260.0f),
				MonaImGui::ScaleUI(460.0f));
			ImGui::TableSetupColumn("Budget", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Usage", ImGuiTableColumnFlags_WidthFixed,
				ValueWidth);
			const FRDGBudget& Budget = Capture->Budget;
			DrawBudgetRow("Passes", Statistics.DeclaredPasses,
				Budget.RegressionMaxPasses, Budget.MaxPasses,
				Statistics.bPassRegressionBudgetExceeded);
			DrawBudgetRow("Dependencies", Statistics.Dependencies,
				Budget.RegressionMaxDependencies, Budget.MaxDependencies,
				Statistics.bDependencyRegressionBudgetExceeded);
			DrawBudgetRow("Texture transitions", Statistics.TextureTransitions,
				Budget.RegressionMaxTextureTransitions,
				Budget.MaxTextureTransitions,
				Statistics.bTextureTransitionRegressionBudgetExceeded);
			DrawBudgetRow("Buffer transitions", Statistics.BufferTransitions,
				Budget.RegressionMaxBufferTransitions,
				Budget.MaxBufferTransitions,
				Statistics.bBufferTransitionRegressionBudgetExceeded);
			ImGui::EndTable();
		}
	}

	auto FRenderingDiagnosticsPanel::DrawScene(
		const FSceneViewportStatisticsSnapshot& Snapshot) -> void
	{
		ImGui::Spacing();
		if (!Snapshot.bAvailable)
		{
			ImGui::TextDisabled("Scene statistics are unavailable.");
			return;
		}
		const FSceneViewStatistics& Statistics = Snapshot.Statistics;
		if (DrawMetricTableBegin("SceneStatistics"))
		{
			DrawValueRow("Visible primitives", std::format("{} / {}",
				Statistics.Visibility.VisiblePrimitives,
				Statistics.Visibility.SubmittedPrimitives));
			DrawValueRow("Static mesh", std::format("{} tris, {} draws",
				Statistics.StaticMesh.Triangles, Statistics.StaticMesh.DrawCalls));
			DrawValueRow("Spline mesh", std::format("{} tris",
				Statistics.SplineMesh.Triangles));
			DrawValueRow("Shadow", Statistics.Shadow.bEnabled
				? std::format("{} cascades, {} tris, {} draws",
					Statistics.Shadow.Cascades, Statistics.Shadow.Triangles,
					Statistics.Shadow.DrawCalls)
				: "Off");
			DrawValueRow("Lights (directional / point / spot)",
				std::format("{} / {} / {}", Statistics.Lights.Directional,
					Statistics.Lights.Point, Statistics.Lights.Spot));
			const FSceneViewVolumetricCloudStatistics& Cloud =
				Statistics.VolumetricCloud;
			DrawValueRow("Volumetric cloud", Cloud.bEnabled
				? std::format("{} x {}, {} samples, {} retained bytes",
					Cloud.TargetWidth, Cloud.TargetHeight,
					Cloud.PrimarySamples + Cloud.LightSamples + Cloud.ShadowSamples,
					Cloud.RetainedBytes + Cloud.ShadowRetainedBytes)
				: "Off");
			DrawValueRow("Cloud history", !Cloud.bHistoryAvailable
				? "Unavailable" : Cloud.bHistoryAccepted ? "Accepted" : "Rejected");
			ImGui::EndTable();
		}
	}

	auto FRenderingDiagnosticsPanel::DrawRenderGraph(
		const FRDGCapture* Capture) -> void
	{
		ImGui::Spacing();
		if (Capture == nullptr)
		{
			ImGui::TextDisabled("Waiting for an explicit frame capture.");
			return;
		}

		const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
		const float AvailableWidth = ImGui::GetContentRegionAvail().x;
		const float MinimumSidebarWidth = MonaImGui::ScaleUI(220.0f);
		const float MinimumCanvasWidth = MonaImGui::ScaleUI(360.0f);
		const float PaneWidth = std::max(
			0.0f, AvailableWidth - Metrics.SplitterThickness);
		const float ListWidth = std::clamp(
			PaneWidth * RenderGraphSidebarRatio,
			std::min(MinimumSidebarWidth, PaneWidth),
			std::max(std::min(MinimumSidebarWidth, PaneWidth),
				PaneWidth - MinimumCanvasWidth));

		ImGui::SetNextItemWidth(ListWidth);
		ImGui::InputTextWithHint("##RenderGraphPassFilter", "Filter passes...",
			PassFilter.data(), PassFilter.size());
		ImGui::SameLine();
		ImGui::Checkbox("Focus dependencies", &bFocusDependencies);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("When a pass is selected or hovered, hide unrelated edges.");
		ImGui::SameLine();
		ImGui::TextDisabled("%zu passes  |  %zu resources  |  %zu dependencies",
			Capture->Passes.size(), Capture->Resources.size(),
			Capture->Dependencies.size());

		const float GraphHeight = std::max(MonaImGui::ScaleUI(330.0f),
			ImGui::GetContentRegionAvail().y * 0.62f);
		if (ImGui::BeginChild("RenderGraphPassList", ImVec2(ListWidth, GraphHeight),
			ImGuiChildFlags_Borders))
		{
			for (size_t Index = 0; Index < Capture->Passes.size(); ++Index)
			{
				const FRDGPassCapture& Pass = Capture->Passes[Index];
				if (!ContainsCaseInsensitive(Pass.Name, PassFilter.data())) continue;
				ImGui::PushID(static_cast<int>(Index));
				if (ImGui::Selectable(Pass.Name.c_str(),
					SelectedPass == static_cast<int32>(Index)))
					SelectedPass = static_cast<int32>(Index);
				ImGui::TextDisabled("%s  |  T %u  B %u", PassTypeLabel(Pass.Type),
					Pass.TextureTransitions, Pass.BufferTransitions);
				ImGui::PopID();
			}
			if (SelectedPass >= 0
				&& static_cast<size_t>(SelectedPass) < Capture->Passes.size())
			{
				const FRDGPassCapture& Pass = Capture->Passes[SelectedPass];
				ImGui::SeparatorText("Selected pass");
				ImGui::TextWrapped("%s", Pass.Name.c_str());
				ImGui::TextDisabled("Declaration %u | %s",
					Pass.DeclarationIndex, PassTypeLabel(Pass.Type));
				for (const FRDGUseCapture& Use : Capture->Uses)
				{
					if (Use.PassDeclarationIndex != Pass.DeclarationIndex
						|| Use.ResourceId >= Capture->Resources.size()) continue;
					ImGui::BulletText("%s: %s",
						UseLabel(Use.Use),
						Capture->Resources[Use.ResourceId].Name.c_str());
				}
			}
		}
		ImGui::EndChild();
		ImGui::SameLine(0.0f, 0.0f);
		MonaImGui::DrawSplitter("RenderGraphSidebarSplitter",
			MonaImGui::EUISplitterAxis::X, GraphHeight, PaneWidth,
			MinimumSidebarWidth, MinimumCanvasWidth, RenderGraphSidebarRatio);
		ImGui::SameLine(0.0f, 0.0f);

		if (ImGui::BeginChild("RenderGraphCanvas", ImVec2(0.0f, GraphHeight),
			ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar))
		{
			constexpr float CanvasWidth = 760.0f;
			const float NodeWidth = MonaImGui::ScaleUI(190.0f);
			const float NodeHeight = MonaImGui::ScaleUI(48.0f);
			const float RowStep = MonaImGui::ScaleUI(70.0f);
			const float HeaderHeight = MonaImGui::ScaleUI(30.0f);
			const ImVec2 Origin = ImGui::GetCursorScreenPos();
			const std::string_view Filter(PassFilter.data());
			std::vector<bool> bVisible(Capture->Passes.size(), Filter.empty());
			std::vector<bool> bFilterMatch(Capture->Passes.size(), false);
			for (size_t Index = 0; Index < Capture->Passes.size(); ++Index)
			{
				bFilterMatch[Index] = ContainsCaseInsensitive(
					Capture->Passes[Index].Name, Filter);
				if (bFilterMatch[Index]) bVisible[Index] = true;
			}
			auto FindPass = [&](uint32 DeclarationIndex) -> size_t {
				for (size_t Index = 0; Index < Capture->Passes.size(); ++Index)
					if (Capture->Passes[Index].DeclarationIndex == DeclarationIndex)
						return Index;
				return Capture->Passes.size();
			};
			if (!Filter.empty())
			{
				for (const FRDGDependency& Dependency : Capture->Dependencies)
				{
					const size_t Before = FindPass(Dependency.BeforePass);
					const size_t After = FindPass(Dependency.AfterPass);
					if (Before >= Capture->Passes.size()
						|| After >= Capture->Passes.size()) continue;
					if (bFilterMatch[Before] || bFilterMatch[After])
					{
						bVisible[Before] = true;
						bVisible[After] = true;
					}
				}
			}
			std::vector<size_t> VisiblePasses;
			std::vector<size_t> VisibleRows(
				Capture->Passes.size(), Capture->Passes.size());
			for (size_t Index = 0; Index < Capture->Passes.size(); ++Index)
				if (bVisible[Index])
				{
					VisibleRows[Index] = VisiblePasses.size();
					VisiblePasses.push_back(Index);
				}
			const float CanvasHeight = std::max(GraphHeight - MonaImGui::ScaleUI(20.0f),
				HeaderHeight + RowStep * static_cast<float>(VisiblePasses.size()));
			ImGui::InvisibleButton("RenderGraphCanvasSurface",
				ImVec2(MonaImGui::ScaleUI(CanvasWidth), CanvasHeight));
			ImDrawList* DrawList = ImGui::GetWindowDrawList();
			const std::array<const char*, 3> LaneNames{"Graphics", "Compute", "Copy"};
			const float LaneStep = MonaImGui::ScaleUI(245.0f);
			for (size_t Lane = 0; Lane < LaneNames.size(); ++Lane)
			{
				const float X = Origin.x + MonaImGui::ScaleUI(15.0f)
					+ LaneStep * static_cast<float>(Lane);
				DrawList->AddText(ImVec2(X, Origin.y),
					ImGui::GetColorU32(ImGuiCol_TextDisabled), LaneNames[Lane]);
			}

			auto LaneOf = [](ERDGPassType Type) -> uint32 {
				return Type == ERDGPassType::Compute ? 1u
					: Type == ERDGPassType::Copy ? 2u : 0u;
			};
			auto NodeMin = [&](size_t Index) -> ImVec2 {
				return ImVec2(
					Origin.x + MonaImGui::ScaleUI(15.0f)
						+ LaneStep * LaneOf(Capture->Passes[Index].Type),
					Origin.y + HeaderHeight
						+ RowStep * static_cast<float>(VisibleRows[Index]));
			};
			int32 HoveredPass = -1;
			for (size_t Index : VisiblePasses)
			{
				const ImVec2 Min = NodeMin(Index);
				const ImVec2 Max(Min.x + NodeWidth, Min.y + NodeHeight);
				if (ImGui::IsMouseHoveringRect(Min, Max))
				{
					HoveredPass = static_cast<int32>(Index);
					break;
				}
			}
			const bool bSelectedPassVisible = SelectedPass >= 0
				&& static_cast<size_t>(SelectedPass) < bVisible.size()
				&& bVisible[SelectedPass];
			const int32 FocusedPass = HoveredPass >= 0
				? HoveredPass : bSelectedPassVisible ? SelectedPass : -1;

			for (const FRDGDependency& Dependency : Capture->Dependencies)
			{
				const size_t Before = FindPass(Dependency.BeforePass);
				const size_t After = FindPass(Dependency.AfterPass);
				if (Before >= Capture->Passes.size() || After >= Capture->Passes.size())
					continue;
				if (!bVisible[Before] || !bVisible[After]) continue;
				const bool bHighlighted = FocusedPass == static_cast<int32>(Before)
					|| FocusedPass == static_cast<int32>(After);
				if (bFocusDependencies && FocusedPass >= 0 && !bHighlighted) continue;
				const ImVec2 A = NodeMin(Before);
				const ImVec2 B = NodeMin(After);
				const uint32 BeforeLane = LaneOf(Capture->Passes[Before].Type);
				const uint32 AfterLane = LaneOf(Capture->Passes[After].Type);
				ImVec2 Start;
				ImVec2 End;
				ImVec2 ControlA;
				ImVec2 ControlB;
				if (BeforeLane == AfterLane)
				{
					Start = ImVec2(A.x + NodeWidth, A.y + NodeHeight * 0.5f);
					End = ImVec2(B.x + NodeWidth, B.y + NodeHeight * 0.5f);
					const size_t Span = VisibleRows[After] > VisibleRows[Before]
						? VisibleRows[After] - VisibleRows[Before]
						: VisibleRows[Before] - VisibleRows[After];
					const float GutterOffset = MonaImGui::ScaleUI(
						24.0f + 4.0f * static_cast<float>(Span % 5));
					ControlA = ImVec2(Start.x + GutterOffset, Start.y);
					ControlB = ImVec2(End.x + GutterOffset, End.y);
				}
				else
				{
					const bool bMovesRight = BeforeLane < AfterLane;
					Start = ImVec2(bMovesRight ? A.x + NodeWidth : A.x,
						A.y + NodeHeight * 0.5f);
					End = ImVec2(bMovesRight ? B.x : B.x + NodeWidth,
						B.y + NodeHeight * 0.5f);
					const float MiddleX = (Start.x + End.x) * 0.5f;
					ControlA = ImVec2(MiddleX, Start.y);
					ControlB = ImVec2(MiddleX, End.y);
				}
				ImVec4 Color = ImGui::GetStyleColorVec4(
					Dependency.Kind == ERDGDependencyKind::Explicit
						? ImGuiCol_CheckMark : Dependency.Kind == ERDGDependencyKind::Value
							? ImGuiCol_TextDisabled : ImGuiCol_Border);
				if (!bHighlighted) Color.w *= 0.38f;
				DrawList->AddBezierCubic(Start, ControlA, ControlB, End,
					ImGui::GetColorU32(Color), bHighlighted ? 2.5f : 1.0f);
				const float DirectionX = End.x - ControlB.x;
				const float DirectionY = End.y - ControlB.y;
				const float DirectionLength = std::sqrt(
					DirectionX * DirectionX + DirectionY * DirectionY);
				if (DirectionLength > 0.0f)
				{
					const float UnitX = DirectionX / DirectionLength;
					const float UnitY = DirectionY / DirectionLength;
					const float ArrowLength = MonaImGui::ScaleUI(7.0f);
					const float ArrowWidth = MonaImGui::ScaleUI(3.5f);
					const ImVec2 Base(End.x - UnitX * ArrowLength,
						End.y - UnitY * ArrowLength);
					DrawList->AddTriangleFilled(End,
						ImVec2(Base.x - UnitY * ArrowWidth,
							Base.y + UnitX * ArrowWidth),
						ImVec2(Base.x + UnitY * ArrowWidth,
							Base.y - UnitX * ArrowWidth),
						ImGui::GetColorU32(Color));
				}
			}

			for (size_t Index : VisiblePasses)
			{
				const FRDGPassCapture& Pass = Capture->Passes[Index];
				const ImVec2 Min = NodeMin(Index);
				const ImVec2 Max(Min.x + NodeWidth, Min.y + NodeHeight);
				const bool bSelected = SelectedPass == static_cast<int32>(Index);
				ImVec4 Fill = ImGui::GetStyleColorVec4(
					bSelected ? ImGuiCol_HeaderActive : ImGuiCol_FrameBg);
				if (!Filter.empty() && !bFilterMatch[Index]) Fill.w *= 0.45f;
				DrawList->AddRectFilled(Min, Max, ImGui::GetColorU32(Fill),
					MonaImGui::ScaleUI(4.0f));
				DrawList->AddRect(Min, Max,
					ImGui::GetColorU32(bSelected ? ImGuiCol_CheckMark : ImGuiCol_Border),
					MonaImGui::ScaleUI(4.0f), 0, bSelected ? 2.0f : 1.0f);
				DrawList->AddText(ImVec2(Min.x + MonaImGui::ScaleUI(8.0f),
					Min.y + MonaImGui::ScaleUI(7.0f)),
					ImGui::GetColorU32(ImGuiCol_Text), Pass.Name.c_str());
				const std::string Detail = std::format("T {}  B {}",
					Pass.TextureTransitions, Pass.BufferTransitions);
				DrawList->AddText(ImVec2(Min.x + MonaImGui::ScaleUI(8.0f),
					Min.y + MonaImGui::ScaleUI(25.0f)),
					ImGui::GetColorU32(ImGuiCol_TextDisabled), Detail.c_str());
				if (HoveredPass == static_cast<int32>(Index)
					&& ImGui::IsMouseClicked(ImGuiMouseButton_Left))
					SelectedPass = static_cast<int32>(Index);
				if (HoveredPass == static_cast<int32>(Index))
				{
					ImGui::BeginTooltip();
					ImGui::PushTextWrapPos(MonaImGui::ScaleUI(520.0f));
					ImGui::TextUnformatted(Pass.Name.c_str());
					constexpr size_t MaximumTooltipDependencies = 12;
					size_t DependencyCount = 0;
					for (const FRDGDependency& Dependency : Capture->Dependencies)
					{
						const size_t Before = FindPass(Dependency.BeforePass);
						const size_t After = FindPass(Dependency.AfterPass);
						if (Before == Index && After < Capture->Passes.size())
						{
							if (DependencyCount < MaximumTooltipDependencies)
								ImGui::BulletText("%s -> %s (%s)",
									DependencyKindLabel(Dependency.Kind),
									Capture->Passes[After].Name.c_str(),
									Dependency.Cause.c_str());
							++DependencyCount;
						}
						else if (After == Index && Before < Capture->Passes.size())
						{
							if (DependencyCount < MaximumTooltipDependencies)
								ImGui::BulletText("%s <- %s (%s)",
									DependencyKindLabel(Dependency.Kind),
									Capture->Passes[Before].Name.c_str(),
									Dependency.Cause.c_str());
							++DependencyCount;
						}
					}
					if (DependencyCount == 0) ImGui::TextDisabled("No dependencies");
					else if (DependencyCount > MaximumTooltipDependencies)
						ImGui::TextDisabled("+ %zu more dependencies",
							DependencyCount - MaximumTooltipDependencies);
					ImGui::PopTextWrapPos();
					ImGui::EndTooltip();
				}
			}
			if (VisiblePasses.empty())
				DrawList->AddText(ImVec2(Origin.x + MonaImGui::ScaleUI(15.0f),
					Origin.y + HeaderHeight), ImGui::GetColorU32(ImGuiCol_TextDisabled),
					"No passes match the filter.");
		}
		ImGui::EndChild();

		if (ImGui::CollapsingHeader("Resources and lifetimes"))
		{
			if (ImGui::BeginTable("RenderGraphResources", 4,
				ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders
				| ImGuiTableFlags_ScrollY, ImVec2(0.0f, MonaImGui::ScaleUI(240.0f))))
			{
				ImGui::TableSetupColumn("Resource");
				ImGui::TableSetupColumn("Kind");
				ImGui::TableSetupColumn("Lifetime");
				ImGui::TableSetupColumn("Preparation");
				ImGui::TableHeadersRow();
				for (size_t Index = 0; Index < Capture->Resources.size(); ++Index)
				{
					const FRDGResourceCapture& Resource = Capture->Resources[Index];
					ImGui::TableNextRow();
					ImGui::TableNextColumn(); ImGui::TextUnformatted(Resource.Name.c_str());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(Resource.Kind == ERDGResourceKind::Texture
						? "Texture" : Resource.Kind == ERDGResourceKind::Buffer
							? "Buffer" : "Token");
					ImGui::TableNextColumn();
					if (Index < Capture->ResourceLifetimes.size())
						ImGui::Text("%u - %u", Capture->ResourceLifetimes[Index].FirstPass,
							Capture->ResourceLifetimes[Index].LastPass);
					else ImGui::TextDisabled("Unavailable");
					ImGui::TableNextColumn(); ImGui::TextUnformatted(Resource.Preparation.c_str());
				}
				ImGui::EndTable();
			}
		}
	}
} // namespace Durin::Editor::Level
