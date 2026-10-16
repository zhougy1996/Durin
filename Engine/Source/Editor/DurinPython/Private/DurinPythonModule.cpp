#include "DurinPythonModule.h"
#include "Editor/EditorEngine.h"
#include "Tools/EditorToolSubsystem.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
    auto FDurinPythonModule::EnsureRuntime() -> std::expected<std::shared_ptr<FPythonRuntime>, std::string>
    {
        if (!IsInGameThread()) return std::unexpected("Python runtime ownership requires GameThread.");
        if (!Running)
        {
            auto Started = Runtime->Start();
            if (!Started) return std::unexpected(Started.error());
            Running = true;
        }
        return Runtime;
    }
    auto FDurinPythonModule::StartForEditor(DEditorEngine& Editor) -> void
    {
        if (Session) return;
        auto Started = EnsureRuntime();
        if (!Started) { DURIN_WARN("Python unavailable: {}", Started.error()); return; }
        auto* Tools = Editor.GetEditorSubsystem<DEditorToolSubsystem>();
        auto Created = Runtime->CreateSession(Tools ? Tools->GetService() : nullptr);
        if (!Created) { DURIN_WARN("Python session unavailable: {}", Created.error()); return; }
        Session = *Created;
    }
    auto FDurinPythonModule::StopRuntime() noexcept -> void
    {
        Runtime->CloseSession(Session); Session = 0; Runtime->Stop(); Running = false;
    }
    IMPLEMENT_MODULE(FDurinPythonModule, DurinPython)
}
