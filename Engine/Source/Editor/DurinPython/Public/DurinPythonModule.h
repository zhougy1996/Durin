#pragma once
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "PythonRuntime.h"

namespace Durin
{
    class DEditorEngine;
    // Binds the optional script runtime to the explicit MainFrame editor host.
    class FDurinPythonModule final : public IModuleInterface
    {
    public:
        DURINPYTHON_API auto StartForEditor(DEditorEngine& Editor) -> void;
        DURINPYTHON_API auto StopRuntime() noexcept -> void;
        auto ShutdownModule() -> void override { StopRuntime(); }
        auto GetRuntime() -> FPythonRuntime& { return Runtime; }
        auto GetSession() const -> uint64 { return Session; }
    private:
        FPythonRuntime Runtime;
        uint64 Session = 0;
    };
}
