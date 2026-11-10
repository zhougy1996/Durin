#include "DurinPythonModule.h"
#include "PythonToolset.h"
#include "PythonEditorSubsystem.h"
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
            Toolset = CreatePythonToolset(Runtime);
            ToolsetRegistration = FModularFeatureRegistry::Get().RegisterFeature(*Toolset);
        }
        return Runtime;
    }
    auto FDurinPythonModule::EnsureEditorSession(DEditorEngine& Editor) -> std::expected<void, std::string>
    {
        if (Session) return {};
        auto Started = EnsureRuntime();
        if (!Started) return std::unexpected(Started.error());
        auto* Tools = Editor.GetEditorSubsystem<DEditorToolSubsystem>();
        auto Created = Runtime->CreateSession(Tools ? Tools->GetService() : nullptr);
        if (!Created) return std::unexpected(Created.error());
        Session = *Created;
        return {};
    }
    auto FDurinPythonModule::RunFileForEditor(DEditorEngine& Editor, const std::filesystem::path& Path) -> std::expected<uint64, std::string>
    {
        if (auto Ready = EnsureEditorSession(Editor); !Ready) return std::unexpected(Ready.error());
        return Runtime->RunFile(Session, Path);
    }
    auto FDurinPythonModule::StopRuntime() noexcept -> void
    {
        StartupJob = 0; StartupScript.clear(); StartupError.clear();
        ToolsetRegistration.Reset(); Toolset.reset();
        Runtime->CloseSession(Session); Session = 0; Runtime->Stop(); Running = false;
    }
    auto FDurinPythonModule::StartupModule() -> void
    {
        Provider = FModuleStartup::GetModuleName();
        EditorRegistration = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{
            .Type = DPythonEditorSubsystem::StaticClass(), .Provider = FModuleStartup::GetModuleName()});
    }
    auto FDurinPythonModule::CanShutdownModule() const -> bool
    { return FEditorSubsystemCollection::CanUpdateActiveEditor(); }
    auto FDurinPythonModule::ShutdownModule() -> void
    {
        require(CanShutdownModule());
        FEditorSubsystemCollection::RemoveAllInstances(Provider);
        EditorRegistration.reset();
        StopRuntime();
    }
    auto FDurinPythonModule::SubmitStartupScript(DEditorEngine& Editor) -> void
    {
        std::string Script;
#if defined(_WIN32)
        char* Buffer = nullptr; size_t Size = 0;
        if (_dupenv_s(&Buffer, &Size, "DURIN_PYTHON_SCRIPT") == 0 && Buffer)
        {
            std::unique_ptr<char, decltype(&std::free)> Owner(Buffer, &std::free);
            Script = Buffer;
        }
#else
        if (const char* Value = std::getenv("DURIN_PYTHON_SCRIPT")) Script = Value;
#endif
        if (Script.empty()) return;
        StartupScript = std::filesystem::u8path(Script);
        auto Submitted = RunFileForEditor(Editor, StartupScript);
        if (!Submitted) { StartupError = Submitted.error(); DURIN_WARN("Startup Python script unavailable: {}", StartupError); return; }
        StartupJob = *Submitted;
    }
    DPythonEditorSubsystem::DPythonEditorSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}
    auto DPythonEditorSubsystem::Initialize(FSubsystemCollection& Collection) -> void
    {
        auto* Tools = Collection.InitializeDependency<DEditorToolSubsystem>();
        if (GetEditor()->IsShutdownRequested()) return;
        requiref(Tools, "Python requires the editor tools subsystem.");
        Module = static_cast<FDurinPythonModule*>(FModuleManager::Get().GetModule("DurinPython"));
        check(Module);
        Module->SubmitStartupScript(*GetEditor());
    }
    auto DPythonEditorSubsystem::Deinitialize() noexcept -> void
    {
        if (auto* Owner = std::exchange(Module, nullptr)) Owner->StopRuntime();
    }
    IMPLEMENT_MODULE(FDurinPythonModule, DurinPython)
}
