#include "DurinPythonModule.h"
#include "Editor/EditorEngine.h"
#include "Tools/EditorToolSubsystem.h"

namespace Durin
{
    auto FDurinPythonModule::StartForEditor(DEditorEngine& Editor) -> void
    {
        auto Started = Runtime.Start();
        if (!Started) { DURIN_WARN("Python unavailable: {}", Started.error()); return; }
        auto* Tools = Editor.GetEditorSubsystem<DEditorToolSubsystem>();
        auto Created = Runtime.CreateSession(Tools ? Tools->GetService() : nullptr);
        if (!Created) { DURIN_WARN("Python session unavailable: {}", Created.error()); Runtime.Stop(); return; }
        Session = *Created;
    }
    auto FDurinPythonModule::StopRuntime() noexcept -> void
    {
        Runtime.CloseSession(Session); Session = 0; Runtime.Stop();
    }
    IMPLEMENT_MODULE(FDurinPythonModule, DurinPython)
}
