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
#if defined(_WIN32)
        char* Value = nullptr; size_t Size = 0;
        if (_dupenv_s(&Value, &Size, "DURIN_PYTHON_SCRIPT") == 0 && Value)
        {
            std::unique_ptr<char, decltype(&std::free)> Owner(Value, &std::free);
            if (Size > 1 && Size <= Path.size()) { std::memcpy(Path.data(), Value, Size); Run(); }
        }
#endif
    }
    auto FPythonScriptPanel::Run() -> void
    {
        auto& Module = PythonModule();
        if (!Module.GetSession()) Module.StartForEditor(Editor);
        if (!Module.GetSession()) { Message = "Python is unavailable. Check the editor log for setup details."; return; }
        auto Submitted = Module.GetRuntime().RunFile(Module.GetSession(), std::filesystem::u8path(Path.data()));
        if (!Submitted) { Message = Submitted.error(); DURIN_WARN("Python script admission failed: {}", Message); return; }
        Job = *Submitted; Reported = false; Result.reset(); Message.clear();
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
        if (!Open) return;
        ImGui::SetNextWindowSize(ImVec2(700, 420), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Python Scripts", &Open))
        {
            ImGui::TextUnformatted("Run a trusted Python script in this editor.");
            ImGui::InputText("Script path", Path.data(), Path.size());
            const bool Busy = Result && (Result->State == EPythonJobState::Queued || Result->State == EPythonJobState::Running);
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
