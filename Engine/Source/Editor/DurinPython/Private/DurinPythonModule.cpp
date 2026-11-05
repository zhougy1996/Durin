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
    auto FDurinPythonModule::RunFileForEditor(DEditorEngine& Editor, const std::filesystem::path& Path) -> std::expected<uint64, std::string>
    {
        StartForEditor(Editor);
        if (!Session) return std::unexpected("Python is unavailable. Check the editor log for setup details.");
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
        EditorRegistration = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{
            .Type = DPythonEditorSubsystem::StaticClass(), .Provider = FModuleStartup::GetModuleName(),
            .Dependencies = {DEditorToolSubsystem::StaticClass()}});
    }
    auto FDurinPythonModule::CanShutdownModule() const -> bool
    { return FEditorSubsystemCollection::CanSynchronizeActiveEditor(); }
    auto FDurinPythonModule::ShutdownModule() -> void
    {
        require(CanShutdownModule());
        EditorRegistration.reset();
        FEditorSubsystemCollection::SynchronizeActiveEditor();
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
    auto DPythonEditorSubsystem::Initialize() -> FSubsystemResult
    {
        FModuleManager::LoadModuleChecked<FDurinPythonModule>("DurinPython").SubmitStartupScript(*GetEditor());
        return {};
    }
    auto DPythonEditorSubsystem::Deinitialize() noexcept -> void
    {
        if (auto* Module = FModuleManager::Get().GetModule("DurinPython"))
            static_cast<FDurinPythonModule*>(Module)->StopRuntime();
    }
    IMPLEMENT_MODULE(FDurinPythonModule, DurinPython)
}
