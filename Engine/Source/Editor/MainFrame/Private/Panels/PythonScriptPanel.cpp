#include "Panels/PythonScriptPanel.h"
#include "DurinPythonModule.h"
#include "MonaImGui.h"

namespace Durin::Editor::MainFrame
{
    namespace
    {
        auto PythonModule() -> FDurinPythonModule&
        { return FModuleManager::LoadModuleChecked<FDurinPythonModule>("DurinPython"); }
        constexpr const char* States[] = {"Queued", "Running", "Succeeded", "Failed", "Cancelled"};
    }
    FPythonScriptPanel::FPythonScriptPanel(DEditorEngine& InEditor) : Editor(InEditor)
    {
        BrowserAction = ContentBrowser::RegisterExtension({
            .Id = "MainFrame.RunPythonScript",
            .Label = "Run Python Script",
            .Category = ContentBrowser::EExtensionCategory::ContextMenu,
            .Mutation = ContentBrowser::EContentMutation::MutatesContent,
            .IsApplicable = [](const ContentBrowser::FExtensionContext& Context) {
                if (Context.Selection.size() != 1 || !Context.PrimaryItem
                    || Context.PrimaryItem->Kind != ContentBrowser::EExtensionItemKind::File
                    || Context.PrimaryItem->StableId != Context.Selection.front().StableId
                    || Context.PrimaryItem->PhysicalPath.empty()) return false;
                auto Extension = std::filesystem::u8path(Context.PrimaryItem->PhysicalPath).extension().string();
                for (char& C : Extension) if (C >= 'A' && C <= 'Z') C += 'a' - 'A';
                return Extension == ".py";
            },
            .Invoke = [this](const ContentBrowser::FExtensionInvocation& Invocation) {
                if (!RunFile(std::filesystem::u8path(Invocation.Context.PrimaryItem->PhysicalPath)) && Invocation.ReportError)
                    Invocation.ReportError(Message);
            },
        }, Message);
        if (!BrowserAction.IsValid()) DURIN_WARN("Python browser action registration failed: {}", Message);
    }
    auto FPythonScriptPanel::IsBusy() const -> bool
    { return Job && (!Result || Result->State == EPythonJobState::Queued || Result->State == EPythonJobState::Running); }
    auto FPythonScriptPanel::RunFile(const std::filesystem::path& Script) -> bool
    {
        OpenRequested = true;
        if (IsBusy()) { Message = "A Python script is already running. Wait for it or cancel it in Python Scripts."; return false; }
        const auto Text = Script.u8string();
        if (Text.empty() || Text.size() >= Path.size() || Text.find(u8'\0') != std::u8string::npos)
        { Message = "The script path is empty, too long or contains NUL."; return false; }
        std::memcpy(Path.data(), Text.data(), Text.size()); Path[Text.size()] = '\0';
        return Run();
    }
    auto FPythonScriptPanel::Run() -> bool
    {
        if (IsBusy()) { Message = "A Python script is already running."; return false; }
        auto& Module = PythonModule();
        if (!Module.GetSession()) Module.StartForEditor(Editor);
        if (!Module.GetSession()) { Message = "Python is unavailable. Check the editor log for setup details."; return false; }
        auto Submitted = Module.GetRuntime().RunFile(Module.GetSession(), std::filesystem::u8path(Path.data()));
        if (!Submitted) { Message = Submitted.error(); DURIN_WARN("Python script admission failed: {}", Message); return false; }
        Job = *Submitted; Reported = false; Result.reset(); Message.clear();
        return true;
    }
    auto FPythonScriptPanel::Tick() -> void
    {
        if (!Job) return;
        auto Snapshot = PythonModule().GetRuntime().GetJobResult(Job);
        if (!Snapshot) { Message = Snapshot.error(); Job = 0; return; }
        Result = std::move(*Snapshot);
        if (!Reported && Result->State != EPythonJobState::Queued && Result->State != EPythonJobState::Running)
        {
            Reported = true;
            DURIN_INFO("Python job {}: {}\n{}\n{}", Job, States[static_cast<size_t>(Result->State)], Result->Output, Result->Error);
        }
    }
    auto FPythonScriptPanel::Draw(bool& Open) -> void
    {
        if (std::exchange(OpenRequested, false)) Open = true;
        if (!Open) return;
        ImGui::SetNextWindowSize(ImVec2(700, 420), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Python Scripts", &Open))
        {
            ImGui::TextUnformatted("Run a trusted Python script in this editor.");
            ImGui::InputText("Script path", Path.data(), Path.size());
            const bool Busy = IsBusy();
            ImGui::BeginDisabled(Busy || Path[0] == '\0');
            if (ImGui::Button("Run")) Run();
            ImGui::EndDisabled(); ImGui::SameLine(); ImGui::BeginDisabled(!Busy);
            if (ImGui::Button("Cancel")) PythonModule().GetRuntime().CancelJob(Job);
            ImGui::EndDisabled();
            if (!Message.empty()) ImGui::TextWrapped("%s", Message.c_str());
            if (Result)
            {
                ImGui::Text("Status: %s", States[static_cast<size_t>(Result->State)]);
                ImGui::Separator();
                ImGui::BeginChild("Output", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
                ImGui::TextUnformatted(Result->Output.c_str());
                if (!Result->Error.empty()) ImGui::TextUnformatted(Result->Error.c_str());
                ImGui::EndChild();
            }
        }
        ImGui::End();
    }
}
