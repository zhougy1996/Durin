#include "ProjectBrowser.h"

#include "Dialogs/FileDialog.h"
#include "Icons/FontAwesomeIcons.h"
#include "Misc/Project.h"
#include "MonaImGui.h"

namespace Durin
{
	namespace
	{
		constexpr float ResponsiveWidth = 900.0f;
		constexpr float BrandPanelMinimumWidth = 260.0f;
		constexpr float BrandPanelMaximumWidth = 360.0f;
		constexpr float CompactBrandHeight = 142.0f;
		constexpr float ProjectRowHeight = 74.0f;
		constexpr float BrandMarkAspectRatio = 0.82f;

		auto DrawBrandMark(ImDrawList* DrawList, const ImVec2& Min, float Height) -> void
		{
			const float Width = Height * BrandMarkAspectRatio;
			const auto Point = [&](float X, float Y) { return Min + ImVec2(Width * X, Height * Y); };

			// Match the authored branding silhouette while keeping the small browser mark
			// flat and theme-independent so it remains recognizable at every UI scale.
			const std::array Outer = {
				Point(0.00f, 0.00f), Point(0.63f, 0.00f), Point(1.00f, 0.30f), Point(1.00f, 0.70f),
				Point(0.65f, 1.00f), Point(0.00f, 1.00f), Point(0.00f, 0.77f), Point(0.63f, 0.77f),
				Point(0.76f, 0.62f), Point(0.76f, 0.35f), Point(0.62f, 0.23f), Point(0.00f, 0.23f),
			};
			DrawList->AddConcavePolyFilled(Outer.data(), static_cast<int>(Outer.size()), IM_COL32(24, 104, 232, 255));

			const std::array Highlight = {
				Point(0.00f, 0.00f), Point(0.63f, 0.00f), Point(1.00f, 0.30f),
				Point(1.00f, 0.46f), Point(0.76f, 0.35f), Point(0.62f, 0.23f), Point(0.00f, 0.23f),
			};
			DrawList->AddConcavePolyFilled(Highlight.data(), static_cast<int>(Highlight.size()), IM_COL32(28, 193, 235, 255));

			const std::array Wedge = {
				Point(0.00f, 0.17f), Point(0.29f, 0.42f), Point(0.29f, 0.66f), Point(0.00f, 0.83f),
			};
			DrawList->AddConvexPolyFilled(Wedge.data(), static_cast<int>(Wedge.size()), IM_COL32(137, 61, 226, 255));
		}

		auto StatusLabel(ERecentProjectStatus Status) -> const char*
		{
			switch (Status)
			{
			case ERecentProjectStatus::Missing: return "Missing";
			case ERecentProjectStatus::Invalid: return "Invalid";
			default: return "";
			}
		}

		auto StatusIcon(ERecentProjectStatus Status) -> const char*
		{
			return Status == ERecentProjectStatus::Missing ? Icons::Warning : Icons::Error;
		}

		auto StatusColor(ERecentProjectStatus Status) -> ImU32
		{
			return MonaImGui::GetThemeColorU32(Status == ERecentProjectStatus::Missing ? MonaImGui::EUIThemeColor::Warning : MonaImGui::EUIThemeColor::Error);
		}
	}

	FProjectBrowser::FProjectBrowser()
		: History(MakeDefaultProjectHistory())
	{
		History.Load(&Error);
	}

	auto FProjectBrowser::RecordCurrentProject() -> void
	{
		const FProjectInfo* Project = GetCurrentProject();
		if (!Project) return;
		std::string SaveError;
		if (!History.Record(Project->Name, Project->ProjectFile, &SaveError))
		{
			Error = std::move(SaveError);
			DURIN_WARN("{}", Error);
		}
		SelectedProject = 0;
	}

	auto FProjectBrowser::Draw() -> void
	{
		ImGuiViewport* Viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(Viewport->WorkPos);
		ImGui::SetNextWindowSize(Viewport->WorkSize);
		ImGui::SetNextWindowViewport(Viewport->ID);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		const ImGuiWindowFlags Flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
			ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
		if (!ImGui::Begin("###Durin.ProjectBrowser", nullptr, Flags))
		{
			ImGui::PopStyleVar(3);
			ImGui::End();
			return;
		}
		ImGui::PopStyleVar(3);

		const ImVec2 Available = ImGui::GetContentRegionAvail();
		const bool bCompact = Available.x < MonaImGui::ScaleUI(ResponsiveWidth);
		const ImVec4 BrandBackground = ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg);
		ImGui::PushStyleColor(ImGuiCol_ChildBg, BrandBackground);
		if (bCompact)
		{
			if (ImGui::BeginChild("ProjectBrowserBrand", ImVec2(Available.x, MonaImGui::ScaleUI(CompactBrandHeight)))) DrawBrandPanel(true);
			ImGui::EndChild();
			ImGui::PopStyleColor();
			if (ImGui::BeginChild("ProjectBrowserContent", ImVec2(Available.x, 0.0f))) DrawProjectContent();
			ImGui::EndChild();
		}
		else
		{
			const float BrandWidth = std::clamp(Available.x * 0.30f, MonaImGui::ScaleUI(BrandPanelMinimumWidth), MonaImGui::ScaleUI(BrandPanelMaximumWidth));
			if (ImGui::BeginChild("ProjectBrowserBrand", ImVec2(BrandWidth, Available.y))) DrawBrandPanel(false);
			ImGui::EndChild();
			ImGui::PopStyleColor();
			ImGui::SameLine(0.0f, 0.0f);
			if (ImGui::BeginChild("ProjectBrowserContent", ImVec2(0.0f, Available.y))) DrawProjectContent();
			ImGui::EndChild();
		}

		ImGui::End();
	}

	auto FProjectBrowser::DrawBrandPanel(bool bCompact) -> void
	{
		const float Padding = MonaImGui::ScaleUI(bCompact ? 24.0f : 36.0f);
		const ImVec2 Origin = ImGui::GetWindowPos();
		const ImVec2 Size = ImGui::GetWindowSize();
		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		const float MarkSize = MonaImGui::ScaleUI(bCompact ? 34.0f : 46.0f);
		const ImVec2 MarkMin = Origin + ImVec2(Padding, Padding);
		DrawBrandMark(DrawList, MarkMin, MarkSize);

		ImGui::SetCursorPos(ImVec2(Padding + MarkSize + MonaImGui::ScaleUI(14.0f), Padding + MonaImGui::ScaleUI(2.0f)));
		ImGui::PushFont(nullptr, bCompact ? 22.0f : 26.0f);
		ImGui::TextUnformatted("DURIN");
		ImGui::PopFont();
		ImGui::SetCursorPosX(Padding + MarkSize + MonaImGui::ScaleUI(14.0f));
		ImGui::TextDisabled("ENGINE");

		if (bCompact)
		{
			ImGui::SetCursorPos(ImVec2(Padding, Padding + MarkSize + MonaImGui::ScaleUI(16.0f)));
			ImGui::TextDisabled("Build worlds. Shape experiences.");
			return;
		}

		ImGui::SetCursorPos(ImVec2(Padding, Padding + MarkSize + MonaImGui::ScaleUI(76.0f)));
		ImGui::PushFont(nullptr, 22.0f);
		ImGui::TextUnformatted("Build worlds.");
		ImGui::SetCursorPosX(Padding);
		ImGui::TextUnformatted("Shape experiences.");
		ImGui::PopFont();
		ImGui::SetCursorPos(ImVec2(Padding, Size.y - Padding - MonaImGui::ScaleUI(34.0f)));
		ImGui::TextDisabled("DURIN PROJECT BROWSER");
	}

	auto FProjectBrowser::DrawProjectContent() -> void
	{
		const float Padding = MonaImGui::ScaleUI(32.0f);
		const float ButtonWidth = MonaImGui::ScaleUI(168.0f);
		ImGui::SetCursorPos(ImVec2(Padding, Padding));
		ImGui::PushFont(nullptr, 26.0f);
		ImGui::TextUnformatted("Recent Projects");
		ImGui::PopFont();

		ImGui::SetCursorPosX(Padding);
		ImGui::TextDisabled("Select a Durin project to continue.");
		ImGui::SetCursorPosX(Padding);
		if (ImGui::Button("Open Other Project...", ImVec2(ButtonWidth, 0.0f))) BrowseForProject();
		ImGui::SetCursorPosX(Padding);
		ImGui::Dummy(ImVec2(0.0f, MonaImGui::ScaleUI(12.0f)));
		ImGui::SetCursorPosX(Padding);
		ImGui::Separator();
		ImGui::SetCursorPosX(Padding);
		ImGui::Dummy(ImVec2(0.0f, MonaImGui::ScaleUI(10.0f)));
		ImGui::SetCursorPosX(Padding);
		ImGui::TextDisabled("Double-click a project or press Enter to open it.");
		ImGui::SetCursorPosX(Padding);
		ImGui::Dummy(ImVec2(0.0f, MonaImGui::ScaleUI(6.0f)));

		const float ErrorHeight = Error.empty() ? 0.0f : MonaImGui::ScaleUI(60.0f);
		ImGui::SetCursorPosX(Padding);
		DrawRecentProjects(std::max(0.0f, ImGui::GetContentRegionAvail().y - ErrorHeight));
		if (!Error.empty())
		{
			ImGui::SetCursorPosX(Padding);
			ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
			if (ImGui::BeginChild("ProjectBrowserError", ImVec2(-Padding, ErrorHeight), ImGuiChildFlags_Borders))
			{
				ImGui::PushStyleColor(ImGuiCol_Text, MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Error));
				ImGui::TextWrapped("%s  %s", Icons::Error, Error.c_str());
				ImGui::PopStyleColor();
			}
			ImGui::EndChild();
			ImGui::PopStyleColor();
		}
	}

	auto FProjectBrowser::DrawRecentProjects(float Height) -> void
	{
		const float RightPadding = MonaImGui::ScaleUI(32.0f);
		if (!ImGui::BeginChild("RecentProjectList", ImVec2(-RightPadding, Height), ImGuiChildFlags_None))
		{
			ImGui::EndChild();
			return;
		}

		const auto& Entries = History.GetEntries();
		if (Entries.empty())
		{
			const float EmptyWidth = MonaImGui::ScaleUI(260.0f);
			ImGui::SetCursorPos(ImVec2(std::max(0.0f, (ImGui::GetWindowWidth() - EmptyWidth) * 0.5f), std::max(0.0f, Height * 0.30f)));
			ImGui::BeginGroup();
			ImGui::PushFont(nullptr, 20.0f);
			ImGui::TextUnformatted("No recent projects");
			ImGui::PopFont();
			ImGui::TextDisabled("Open a .dproject file to get started.");
			if (ImGui::Button("Open Project...", ImVec2(EmptyWidth, 0.0f))) BrowseForProject();
			ImGui::EndGroup();
			ImGui::EndChild();
			return;
		}

		std::optional<std::string> ProjectToRemove;
		std::optional<std::string> ProjectToOpen;
		for (size_t Index = 0; Index < Entries.size(); ++Index)
		{
			if (DrawProjectRow(Index, Entries[Index])) ProjectToRemove = Entries[Index].ProjectFile;
			if (SelectedProject == static_cast<int32>(Index) && Entries[Index].Status == ERecentProjectStatus::Available &&
				ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Enter))
				ProjectToOpen = Entries[Index].ProjectFile;
			ImGui::Dummy(ImVec2(0.0f, MonaImGui::ScaleUI(5.0f)));
		}

		if (ProjectToRemove)
		{
			std::string RemoveError;
			if (!History.Remove(*ProjectToRemove, &RemoveError)) Error = std::move(RemoveError);
			else
			{
				Error.clear();
				SelectedProject = std::min<int32>(SelectedProject, static_cast<int32>(History.GetEntries().size()) - 1);
			}
		}
		else if (ProjectToOpen) OpenProjectFile(*ProjectToOpen);
		ImGui::EndChild();
	}

	auto FProjectBrowser::DrawProjectRow(size_t Index, const FRecentProjectInfo& Project) -> bool
	{
		ImGui::PushID(static_cast<int>(Index));
		const float RowHeight = MonaImGui::ScaleUI(ProjectRowHeight);
		const bool bSelected = SelectedProject == static_cast<int32>(Index);
		const ImVec2 RowStart = ImGui::GetCursorScreenPos();
		const bool bPressed = ImGui::Selectable("##Project", bSelected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, RowHeight));
		const bool bHovered = ImGui::IsItemHovered();
		const ImVec2 RowEnd = ImGui::GetItemRectMax();
		if (bPressed)
		{
			SelectedProject = static_cast<int32>(Index);
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && Project.Status == ERecentProjectStatus::Available)
				OpenProjectFile(Project.ProjectFile);
		}

		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		const float Left = MonaImGui::ScaleUI(18.0f);
		const float IconWidth = MonaImGui::ScaleUI(34.0f);
		const ImU32 TextColor = ImGui::GetColorU32(ImGuiCol_Text);
		const ImU32 DisabledColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
		const ImVec4 ClipRect(RowStart.x, RowStart.y, RowEnd.x - MonaImGui::ScaleUI(42.0f), RowEnd.y);
		DrawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), RowStart + ImVec2(Left, MonaImGui::ScaleUI(17.0f)),
			MonaImGui::GetThemeColorU32(MonaImGui::EUIThemeColor::SelectionPrimary), Icons::FolderOpen);
		DrawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), RowStart + ImVec2(Left + IconWidth, MonaImGui::ScaleUI(12.0f)),
			TextColor, Project.Name.c_str(), nullptr, 0.0f, &ClipRect);
		DrawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), RowStart + ImVec2(Left + IconWidth, MonaImGui::ScaleUI(40.0f)),
			DisabledColor, Project.ProjectFile.c_str(), nullptr, 0.0f, &ClipRect);

		if (Project.Status != ERecentProjectStatus::Available)
		{
			const std::string Label = std::format("{}  {}", StatusIcon(Project.Status), StatusLabel(Project.Status));
			const ImVec2 LabelSize = ImGui::CalcTextSize(Label.c_str());
			DrawList->AddText(RowStart + ImVec2(std::max(Left, RowEnd.x - RowStart.x - LabelSize.x - MonaImGui::ScaleUI(52.0f)), MonaImGui::ScaleUI(12.0f)),
				StatusColor(Project.Status), Label.c_str());
			if (bHovered && !Project.Error.empty()) ImGui::SetTooltip("%s", Project.Error.c_str());
		}
		else if (bHovered && ImGui::CalcTextSize(Project.ProjectFile.c_str()).x > ClipRect.z - RowStart.x - Left - IconWidth)
			ImGui::SetTooltip("%s", Project.ProjectFile.c_str());

		bool bRemove = false;
		const ImVec2 NextCursor = ImGui::GetCursorScreenPos();
		if (bHovered || bSelected)
		{
			const float Extent = MonaImGui::ScaleUI(28.0f);
			ImGui::SetCursorScreenPos(ImVec2(RowEnd.x - Extent - MonaImGui::ScaleUI(8.0f), RowStart.y + (RowHeight - Extent) * 0.5f));
			if (ImGui::Button(Icons::Close, ImVec2(Extent, Extent))) bRemove = true;
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove from recent projects");
			ImGui::SetCursorScreenPos(NextCursor);
		}
		ImGui::PopID();
		return bRemove;
	}

	auto FProjectBrowser::OpenProjectFile(std::string_view ProjectFile) -> void
	{
		if (!OpenProject) return;
		std::string OpenError;
		if (!OpenProject(ProjectFile, OpenError))
		{
			Error = std::move(OpenError);
			return;
		}
		Error.clear();
		RecordCurrentProject();
	}

	auto FProjectBrowser::BrowseForProject() -> void
	{
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Open a Durin Project";
		Request.Filters = {{"Durin Project", "*.dproject"}};
		const FFileDialogResult Result = OpenFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Selected) OpenProjectFile(Result.FilePath);
		else if (Result.Status == EFileDialogStatus::Error) Error = Result.ErrorMessage;
	}
}
