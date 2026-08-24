#include "Panels/RenderingDiagnosticsPanel.h"

#include "Client/SceneViewport.h"
#include "Editor/WorkspaceUI.h"
#include "MonaImGui.h"
#include "Panels/SceneViewportPanel.h"
#include "RenderGraph.h"
#include "Workspace/LevelEditorWorkspace.h"

namespace Durin::Editor::Level
{
	namespace
	{
		auto PassTypeLabel(ERenderGraphPassType Type) -> const char*
		{
			switch (Type)
			{
			case ERenderGraphPassType::Graphics: return "Graphics";
			case ERenderGraphPassType::Compute: return "Compute";
			case ERenderGraphPassType::Copy: return "Copy";
			}
			return "Unknown";
		}

		auto UseLabel(ERenderGraphUse Use) -> const char*
		{
			switch (Use)
			{
			case ERenderGraphUse::Read: return "Read";
			case ERenderGraphUse::Write: return "Write";
			case ERenderGraphUse::ReadWrite: return "Read / Write";
			}
			return "Unknown";
		}

		auto DrawValueRow(const char* Label, std::string_view Value) -> void
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextDisabled("%s", Label);
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
			if (Filter.empty()) return true;
			return std::ranges::search(Text, Filter, [](char Left, char Right) {
				return std::tolower(static_cast<unsigned char>(Left))
					== std::tolower(static_cast<unsigned char>(Right));
			}).begin() != Text.end();
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
		if (bHasCapture)
			ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.45f, 1.0f),
				"Captured frame graph");
		else if (bCapturePending)
			ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.2f, 1.0f),
				"Waiting for the next rendered frame...");
		else
			ImGui::TextDisabled("Frame graph unavailable");
		ImGui::SameLine();
		if (ImGui::Button(bCapturePending ? "Capture pending" : "Capture next frame"))
			RequestCapture();
		ImGui::SameLine();
		ImGui::TextDisabled("Statistics revision %llu",
			static_cast<unsigned long long>(Statistics.Revision));

		const FRenderGraphCapture* Capture = bHasCapture
			? Graph.Capture.get() : nullptr;
		const ImGuiStyle& Style = ImGui::GetStyle();
		ImGui::PushStyleVar(ImGuiStyleVar_TabBorderSize,
			MonaImGui::ScaleUI(1.0f));
		ImGui::PushStyleColor(ImGuiCol_TabSelected,
			Style.Colors[ImGuiCol_HeaderActive]);
		ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline,
			Style.Colors[ImGuiCol_CheckMark]);
		if (ImGui::BeginTabBar("RenderingDiagnosticsTabs",
			ImGuiTabBarFlags_DrawSelectedOverline))
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
			ImGui::EndTabBar();
		}
		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar();
		ImGui::End();
	}

	auto FRenderingDiagnosticsPanel::DrawOverview(
		const FSceneViewportStatisticsSnapshot& Snapshot,
		const FRenderGraphCapture* Capture) -> void
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
			DrawValueRow("Editor frame time", std::format("{:.2f} ms",
				ImGui::GetIO().DeltaTime * 1000.0f));
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
		const FRenderGraphStatistics& Statistics = Capture->Statistics;
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
			const FRenderGraphBudget& Budget = Capture->Budget;
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
			DrawValueRow("Skeletal mesh", std::format("{} tris, {} draws",
				Statistics.SkeletalMesh.Triangles,
				Statistics.SkeletalMesh.DrawCalls));
			DrawValueRow("Terrain", std::format("{} patches, {} tris, {} draws",
				Statistics.Terrain.VisiblePatches, Statistics.Terrain.Triangles,
				Statistics.Terrain.DrawCalls));
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
		const FRenderGraphCapture* Capture) -> void
	{
		ImGui::Spacing();
		if (Capture == nullptr)
		{
			ImGui::TextDisabled("Waiting for an explicit frame capture.");
			return;
		}

		ImGui::SetNextItemWidth(MonaImGui::ScaleUI(260.0f));
		ImGui::InputTextWithHint("##RenderGraphPassFilter", "Filter passes...",
			PassFilter.data(), PassFilter.size());
		ImGui::SameLine();
		ImGui::TextDisabled("%zu passes  |  %zu resources  |  %zu dependencies",
			Capture->Passes.size(), Capture->Resources.size(),
			Capture->Dependencies.size());

		const float ListWidth = MonaImGui::ScaleUI(250.0f);
		const float GraphHeight = std::max(MonaImGui::ScaleUI(330.0f),
			ImGui::GetContentRegionAvail().y * 0.62f);
		if (ImGui::BeginChild("RenderGraphPassList", ImVec2(ListWidth, GraphHeight),
			ImGuiChildFlags_Borders))
		{
			for (size_t Index = 0; Index < Capture->Passes.size(); ++Index)
			{
				const FRenderGraphPassCapture& Pass = Capture->Passes[Index];
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
				const FRenderGraphPassCapture& Pass = Capture->Passes[SelectedPass];
				ImGui::SeparatorText("Selected pass");
				ImGui::TextWrapped("%s", Pass.Name.c_str());
				ImGui::TextDisabled("Declaration %u | %s",
					Pass.DeclarationIndex, PassTypeLabel(Pass.Type));
				for (const FRenderGraphUseCapture& Use : Capture->Uses)
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
		ImGui::SameLine();

		if (ImGui::BeginChild("RenderGraphCanvas", ImVec2(0.0f, GraphHeight),
			ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar))
		{
			constexpr float CanvasWidth = 760.0f;
			const float NodeWidth = MonaImGui::ScaleUI(190.0f);
			const float NodeHeight = MonaImGui::ScaleUI(48.0f);
			const float RowStep = MonaImGui::ScaleUI(70.0f);
			const float HeaderHeight = MonaImGui::ScaleUI(30.0f);
			const ImVec2 Origin = ImGui::GetCursorScreenPos();
			const float CanvasHeight = std::max(GraphHeight - MonaImGui::ScaleUI(20.0f),
				HeaderHeight + RowStep * static_cast<float>(Capture->Passes.size()));
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

			auto LaneOf = [](ERenderGraphPassType Type) -> uint32 {
				return Type == ERenderGraphPassType::Compute ? 1u
					: Type == ERenderGraphPassType::Copy ? 2u : 0u;
			};
			auto NodeMin = [&](size_t Index) -> ImVec2 {
				return ImVec2(
					Origin.x + MonaImGui::ScaleUI(15.0f)
						+ LaneStep * LaneOf(Capture->Passes[Index].Type),
					Origin.y + HeaderHeight + RowStep * static_cast<float>(Index));
			};
			auto FindPass = [&](uint32 DeclarationIndex) -> size_t {
				for (size_t Index = 0; Index < Capture->Passes.size(); ++Index)
					if (Capture->Passes[Index].DeclarationIndex == DeclarationIndex)
						return Index;
				return Capture->Passes.size();
			};

			for (const FRenderGraphDependency& Dependency : Capture->Dependencies)
			{
				const size_t Before = FindPass(Dependency.BeforePass);
				const size_t After = FindPass(Dependency.AfterPass);
				if (Before >= Capture->Passes.size() || After >= Capture->Passes.size())
					continue;
				const ImVec2 A = NodeMin(Before);
				const ImVec2 B = NodeMin(After);
				const ImVec2 Start(A.x + NodeWidth, A.y + NodeHeight * 0.5f);
				const ImVec2 End(B.x, B.y + NodeHeight * 0.5f);
				const bool bHighlighted = SelectedPass == static_cast<int32>(Before)
					|| SelectedPass == static_cast<int32>(After);
				const ImU32 Color = ImGui::GetColorU32(bHighlighted
					? ImGuiCol_CheckMark : ImGuiCol_Border);
				const float Bend = MonaImGui::ScaleUI(45.0f);
				DrawList->AddBezierCubic(Start,
					ImVec2(Start.x + Bend, Start.y),
					ImVec2(End.x - Bend, End.y), End, Color,
					bHighlighted ? 2.5f : 1.0f);
			}

			for (size_t Index = 0; Index < Capture->Passes.size(); ++Index)
			{
				const FRenderGraphPassCapture& Pass = Capture->Passes[Index];
				const ImVec2 Min = NodeMin(Index);
				const ImVec2 Max(Min.x + NodeWidth, Min.y + NodeHeight);
				const bool bSelected = SelectedPass == static_cast<int32>(Index);
				const bool bMatches = ContainsCaseInsensitive(
					Pass.Name, PassFilter.data());
				ImVec4 Fill = ImGui::GetStyleColorVec4(
					bSelected ? ImGuiCol_HeaderActive : ImGuiCol_FrameBg);
				if (!bMatches) Fill.w *= 0.25f;
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
				if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
					&& ImGui::IsMouseHoveringRect(Min, Max))
					SelectedPass = static_cast<int32>(Index);
			}
		}
		ImGui::EndChild();

		if (ImGui::CollapsingHeader("Resources and lifetimes"))
		{
			if (ImGui::BeginTable("RenderGraphResources", 5,
				ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders
				| ImGuiTableFlags_ScrollY, ImVec2(0.0f, MonaImGui::ScaleUI(240.0f))))
			{
				ImGui::TableSetupColumn("Resource");
				ImGui::TableSetupColumn("Kind");
				ImGui::TableSetupColumn("Backing");
				ImGui::TableSetupColumn("Lifetime");
				ImGui::TableSetupColumn("Preparation");
				ImGui::TableHeadersRow();
				for (size_t Index = 0; Index < Capture->Resources.size(); ++Index)
				{
					const FRenderGraphResourceCapture& Resource = Capture->Resources[Index];
					ImGui::TableNextRow();
					ImGui::TableNextColumn(); ImGui::TextUnformatted(Resource.Name.c_str());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(Resource.Kind == ERenderGraphResourceKind::Texture
						? "Texture" : Resource.Kind == ERenderGraphResourceKind::Buffer
							? "Buffer" : "Token");
					ImGui::TableNextColumn(); ImGui::TextUnformatted(Resource.BackingClass.c_str());
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
