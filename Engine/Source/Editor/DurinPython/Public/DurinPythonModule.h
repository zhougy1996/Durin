#pragma once
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "PythonRuntime.h"
#include "Editor/EditorSubsystem.h"

namespace Durin
{
    class DEditorEngine;
    // Binds the optional script runtime to the owning editor session.
    class FDurinPythonModule final : public IModuleInterface
    {
    public:
        // GameThread owner: initialize once, then share a lifetime-safe native handle.
        DURINPYTHON_API auto EnsureRuntime() -> std::expected<std::shared_ptr<FPythonRuntime>, std::string>;
        DURINPYTHON_API auto RunFileForEditor(DEditorEngine& Editor, const std::filesystem::path& Path) -> std::expected<uint64, std::string>;
        DURINPYTHON_API auto StartupModule() -> void override;
        DURINPYTHON_API auto CanShutdownModule() const -> bool override;
        DURINPYTHON_API auto ShutdownModule() -> void override;
        auto GetStartupJob() const -> uint64 { return StartupJob; }
        auto GetStartupError() const -> const std::string& { return StartupError; }
        auto GetStartupScript() const -> const std::filesystem::path& { return StartupScript; }
        auto GetRuntime() -> FPythonRuntime& { return *Runtime; }
        auto GetSession() const -> uint64 { return Session; }
    private:
        friend class DPythonEditorSubsystem;
        auto EnsureEditorSession(DEditorEngine& Editor) -> std::expected<void, std::string>;
        auto StopRuntime() noexcept -> void;
        auto SubmitStartupScript(DEditorEngine& Editor) -> void;
        std::unique_ptr<FEditorSubsystemRegistration> EditorRegistration;
        FName Provider;
        uint64 StartupJob = 0;
        std::filesystem::path StartupScript;
        std::string StartupError;
        std::shared_ptr<FPythonRuntime> Runtime = std::make_shared<FPythonRuntime>();
        bool Running = false;
        uint64 Session = 0;
        std::unique_ptr<IToolset> Toolset;
        FModularFeatureRegistration ToolsetRegistration;
    };
}
