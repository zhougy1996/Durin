#pragma once
#include "CoreMinimal.h"
#include "PythonRuntime.h"
#include "ContentBrowser/ContentBrowserContracts.h"

namespace Durin { class DEditorEngine; }
namespace Durin::Editor::MainFrame
{
    // GameThread presenter; jobs retain only the editor's detached tool endpoint.
    class FPythonScriptPanel final
    {
    public:
        explicit FPythonScriptPanel(DEditorEngine& Editor);
        FPythonScriptPanel(const FPythonScriptPanel&) = delete;
        auto operator=(const FPythonScriptPanel&) -> FPythonScriptPanel& = delete;
        auto RunFile(const std::filesystem::path& Script) -> bool;
        auto Tick() -> void;
        auto Draw(bool& Open) -> void;
    private:
        friend struct FPythonScriptPanelTestAccess;
        auto Run() -> bool;
        auto IsBusy() const -> bool;
        DEditorEngine& Editor;
        std::array<char, 4096> Path{};
        uint64 Job = 0;
        bool Reported = false;
        std::string Message;
        std::optional<FPythonJobResult> Result;
        bool OpenRequested = false;
        ContentBrowser::FScopedExtensionRegistration BrowserAction;
    };
}
