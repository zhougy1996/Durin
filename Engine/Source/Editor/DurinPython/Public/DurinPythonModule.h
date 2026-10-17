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
        // GameThread owner: initialize once, then share a lifetime-safe native handle.
        DURINPYTHON_API auto EnsureRuntime() -> std::expected<std::shared_ptr<FPythonRuntime>, std::string>;
        DURINPYTHON_API auto StartForEditor(DEditorEngine& Editor) -> void;
        DURINPYTHON_API auto StopRuntime() noexcept -> void;
        auto ShutdownModule() -> void override { StopRuntime(); }
        auto GetRuntime() -> FPythonRuntime& { return *Runtime; }
        auto GetSession() const -> uint64 { return Session; }
    private:
        std::shared_ptr<FPythonRuntime> Runtime = std::make_shared<FPythonRuntime>();
        bool Running = false;
        uint64 Session = 0;
        std::unique_ptr<IToolset> Toolset;
        FModularFeatureRegistration ToolsetRegistration;
    };
}
