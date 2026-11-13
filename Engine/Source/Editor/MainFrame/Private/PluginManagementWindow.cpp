#include "PluginManagementWindow.h"
#include "Misc/StringHelper.h"
#include "Style/MonaImGuiStyle.h"
#include "ThirdParty/ImGui/imgui.h"

namespace Durin::MainFrame
{
    auto FPluginManagementWindow::Reload() -> void
    {
        FPluginManagementContext Context;
        FPluginEditSession Candidate;
        if (!MakePluginManagementContext(Context, Error) || !Candidate.Open(std::move(Context), Error))
        { return; }
        Session = std::move(Candidate); Error.clear();
    }
    auto FPluginManagementWindow::CheckRestartReady(std::string& Reason) -> bool
    {
        if (!Session) { Reason = "Open plugin management before restarting"; return false; }
        return Session->CheckRestartReady(Reason);
    }
    auto FPluginManagementWindow::Draw(bool& bOpen, const std::function<void()>& RequestRestart) -> void
    {
        if (!bOpen) { bWasOpen = false; return; }
        if (!bWasOpen) { Reload(); bWasOpen = true; }
        ImGui::SetNextWindowSize(ImVec2(MonaImGui::ScaleUI(620), MonaImGui::ScaleUI(460)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(MonaImGui::ScaleUI(420), MonaImGui::ScaleUI(320)),
            ImVec2(std::numeric_limits<float>::max(), std::numeric_limits<float>::max()));
        if (!ImGui::Begin("Plugins###Durin.Plugins", &bOpen)) { ImGui::End(); return; }
        const auto& Style = ImGui::GetStyle();
        const auto Warning = MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Warning);
        const auto Info = MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Info);
        const auto ErrorColor = MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Error);
        const auto Tooltip = [](std::string_view Text) {
            if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + MonaImGui::ScaleUI(460.0f));
            ImGui::TextUnformatted(Text.data(), Text.data() + Text.size());
            ImGui::PopTextWrapPos(); ImGui::EndTooltip();
        };
        const auto ButtonWidth = [&](const char* Label) { return ImGui::CalcTextSize(Label).x + Style.FramePadding.x * 2; };
        ImGui::TextDisabled("Choose plugins for the next editor start.");
        ImGui::Spacing();
        const float RefreshWidth = ButtonWidth("Refresh");
        ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - RefreshWidth - Style.ItemSpacing.x));
        ImGui::BeginDisabled(!Session);
        ImGui::InputTextWithHint("##PluginSearch", "Search plugins...", Filter.data(), Filter.size());
        ImGui::EndDisabled(); ImGui::SameLine();
        if (ImGui::Button("Refresh", ImVec2(RefreshWidth, 0))) Reload();
        Tooltip("Rescan plugins and reload saved settings. Unsaved checkbox edits will be discarded.");
        if (!Session)
        {
            ImGui::Spacing(); ImGui::PushStyleColor(ImGuiCol_Text, ErrorColor);
            ImGui::TextWrapped("%s", Error.c_str()); ImGui::PopStyleColor();
            ImGui::End(); return;
        }
        const auto& Rows = Session->GetRows();
        const auto Selected = std::ranges::count_if(Rows, [](const auto& Row) { return Row.bRequested; });
        ImGui::TextDisabled("%zu plugins  |  %zu selected", Rows.size(), static_cast<size_t>(Selected));
        const auto Search = StringUtils::FoldAscii(std::string(Filter.data()));
        // Reserve two status lines and the action row, independently of list length.
        const float FooterHeight = ImGui::GetFrameHeight() + ImGui::GetTextLineHeightWithSpacing() * 2 + Style.ItemSpacing.y * 3 + 1;
        const float ListHeight = std::max(ImGui::GetFrameHeight() * 2, ImGui::GetContentRegionAvail().y - FooterHeight);
        std::optional<std::pair<std::string, bool>> Change;
        if (ImGui::BeginChild("PluginList", ImVec2(0, ListHeight), ImGuiChildFlags_Borders))
        {
            size_t Visible = 0;
            for (const auto& Row : Rows)
            {
                if (!Search.empty() && StringUtils::FoldAscii(Row.Info.DisplayName + " " + Row.Info.Id).find(Search) == std::string::npos) continue;
                if (Visible++) ImGui::Separator();
                ImGui::Spacing(); ImGui::PushID(Row.Info.Id.c_str()); ImGui::BeginGroup();
                bool Enabled = Row.bRequested;
                if (ImGui::Checkbox("##Enabled", &Enabled)) Change = {{Row.Info.Id, Enabled}};
                ImGui::SameLine(); ImGui::BeginGroup();
                ImGui::TextWrapped("%s", Row.Info.DisplayName.c_str());
                if (ImGui::IsItemClicked()) Change = {{Row.Info.Id, !Row.bRequested}};
                if (Row.bRequested && !Row.bNextStart)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, Warning);
                    ImGui::TextWrapped("%s", Row.Reason.c_str()); ImGui::PopStyleColor();
                }
                else if (Row.bNextStart && !Row.bBuildAvailable)
                {
                    ImGui::TextColored(ErrorColor, "Cannot load on restart");
                    Tooltip(Row.Availability);
                }
                else if (Row.bActive != Row.bNextStart)
                    ImGui::TextDisabled("%s after restart", Row.bNextStart ? "Enabled" : "Disabled");
                ImGui::EndGroup(); ImGui::EndGroup();
                if (ImGui::IsItemHovered())
                    Tooltip(Row.Info.Id + "  " + Row.Info.Version + "\nRunning: " + (Row.bActive ? "Enabled" : "Disabled")
                        + (Row.Availability.empty() ? "" : "\n" + Row.Availability));
                ImGui::PopID(); ImGui::Spacing();
            }
            if (!Visible)
            {
                ImGui::Spacing();
                ImGui::TextUnformatted(Rows.empty() ? "No plugins found." : "No matching plugins.");
                ImGui::TextDisabled(Rows.empty() ? "Use Refresh to scan again." : "Try a different name or plugin ID.");
            }
        }
        ImGui::EndChild();
        // Apply edits after iteration; changing a checkbox rebuilds row state in memory.
        if (Change) (void)Session->SetEnabled(Change->first, Change->second, Error);
        ImGui::Separator();
        if (Session->IsDirty()) ImGui::TextColored(Warning, "Unsaved changes");
        else if (Session->RequiresRestart()) ImGui::TextColored(Info, "Restart required to apply saved changes");
        else ImGui::TextDisabled("No restart needed");
        if (!Error.empty())
        {
            ImGui::TextColored(ErrorColor, "Action failed. Hover for details."); Tooltip(Error);
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy diagnostic")) ImGui::SetClipboardText(Error.c_str());
        }
        else if (!Session->IsSelectionValid()) { ImGui::TextColored(ErrorColor, "Plugin selection needs attention."); Tooltip(Session->GetValidationError()); }
        else if (!Session->GetBuildError().empty())
        {
            ImGui::TextColored(ErrorColor, "Cannot load on restart."); Tooltip(Session->GetBuildError());
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy diagnostic")) ImGui::SetClipboardText(Session->GetBuildError().c_str());
        }
        else ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
        const float SaveWidth = ButtonWidth("Save");
        const float CancelWidth = ButtonWidth("Cancel");
        const float RestartWidth = ButtonWidth("Restart editor...");
        const float ActionsWidth = SaveWidth + CancelWidth + RestartWidth + Style.ItemSpacing.x * 2;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - ActionsWidth));
        ImGui::BeginDisabled(!Session->IsDirty() || !Session->IsSelectionValid());
        if (ImGui::Button("Save", ImVec2(SaveWidth, 0))) (void)Session->Save(Error);
        ImGui::EndDisabled(); ImGui::SameLine();
        ImGui::BeginDisabled(!Session->IsDirty() && Error.empty());
        if (ImGui::Button("Cancel", ImVec2(CancelWidth, 0))) (void)Session->Revert(Error);
        ImGui::EndDisabled(); ImGui::SameLine();
        ImGui::BeginDisabled(Session->IsDirty() || !Session->RequiresRestart()
            || !Session->IsSelectionValid());
        if (ImGui::Button("Restart editor...", ImVec2(RestartWidth, 0)))
        {
            if (Session->CheckRestartReady(Error)) RequestRestart();
        }
        ImGui::EndDisabled(); ImGui::End();
    }
}
