#pragma once
#include "CoreMinimal.h"
#include "PythonRuntime.h"

namespace Durin { class DEditorEngine; }
namespace Durin::Editor::MainFrame
{
    // GameThread presenter; jobs retain only the editor's detached tool endpoint.
    class FPythonScriptPanel final
    {
    public:
        explicit FPythonScriptPanel(DEditorEngine& Editor);
        auto Tick() -> void;
        auto Draw(bool& Open) -> void;
    private:
        auto Run() -> void;
        DEditorEngine& Editor;
        std::array<char, 4096> Path{};
        uint64 Job = 0;
        bool Reported = false;
        std::string Message;
        std::optional<FPythonJobResult> Result;
    };
}
