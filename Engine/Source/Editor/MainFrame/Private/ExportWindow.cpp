#include "ExportWindow.h"
#include "GameExport.h"
#include "HAL/PlatformProcess.h"
#include "Json/Json.h"
#include "Misc/Paths.h"
#include "Misc/RuntimePaths.h"
#include "Project/Project.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditor.h"
#include "Documents/EditorDocumentManager.h"
#include "ThirdParty/ImGui/imgui.h"
namespace Durin::MainFrame {
    FExportWindow::~FExportWindow() { Stop(); }
    void FExportWindow::Stop() { Cancel=true; if(Worker.joinable()) Worker.join(); }
    void FExportWindow::Start(bool Preview) {
        if(Worker.joinable()) Worker.join();
        Cancel=false; Running=true;
        const auto Project=FPaths::ProjectFile(); const std::string Directory=Output;
        const std::string Config=Configuration?"Release":"Debug"; const bool Check=Verify, Zip=Archive;
        Worker=std::jthread([this,Project,Directory,Config,Check,Zip,Preview] {
            try {
                const auto* Override=std::getenv("DURIN_EXPORT_COMPONENT");
                const auto ComponentFile=Override?std::filesystem::u8path(Override):std::filesystem::path(FPlatformProcess::ExecutablePath()).parent_path()/"ExportComponent.json";
                if(!ComponentFile.is_absolute()) throw std::runtime_error("Export component path must be absolute.");
                const auto Root=ComponentFile.parent_path();
                FJsonDocument Component; if(!Component.LoadFromFile(ComponentFile)) throw std::runtime_error("Export tools are missing; build or install the export component.");
                const auto C=Component.GetRootView();
                const auto Path=[&](std::string_view K) { auto P=std::filesystem::u8path(C.GetView(K).GetString()); return (P.is_absolute()?P:Root/P).lexically_normal(); };
                const auto Backend=C.GetView("DefaultBackend").GetString("source");
                FJsonDocument Request; auto R=Request.GetMutableRoot(); R.EnsureObject();
                R.SetChildValue("ProtocolVersion",1); R.SetChildValue("Kind","request"); R.SetChildValue("RequestId","Editor1"); R.SetChildValue("Operation","export");
                R.SetChildValue("ProjectFile",Project); R.SetChildValue("EngineRoot",Path("EngineRoot").generic_string()); R.SetChildValue("BackendMetadata",Path("BackendMetadata").generic_string());
                R.SetChildValue("Output",Directory); R.SetChildValue("Platform","Win64"); R.SetChildValue("Configuration",Config); R.SetChildValue("Backend",Backend);
                R.SetChildValue("Build",Backend=="source"?"auto":"reuse"); R.SetChildValue("Cook",Backend=="source"?"auto":"reuse"); R.SetChildValue("Verify",Check); R.SetChildValue("Archive",Zip); R.SetChildValue("Preview",Preview);
                RunGameExportWorker(Path("Worker"),Request.ToString(EJsonFormat::Compact),[this]{return Cancel.load();},[this](std::string_view Line){
                    FJsonDocument Event; if(Event.Parse(Line)) {std::lock_guard Lock(Mutex); Message=Event.GetRootView().GetView("Message").GetString();}
                });
            } catch(const std::exception& E) {std::lock_guard Lock(Mutex); Message=E.what();}
            Running=false;
        });
    }
    void FExportWindow::Draw(bool& Open,DAssetEditorSubsystem* Editors) {
        if(!Open) return;
        if(!Output[0]) {
            const auto Root=std::filesystem::temp_directory_path()/"DurinExports"/std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
            const auto S=Root.generic_string(); std::snprintf(Output,sizeof(Output),"%s",S.c_str());
        }
        if(!ImGui::Begin("Export Game",&Open)) {ImGui::End(); return;}
        ImGui::BeginDisabled(Running);
        ImGui::Combo("Configuration",&Configuration,"Debug\0Release\0");
        ImGui::InputText("New export directory",Output,sizeof(Output));
        ImGui::Checkbox("Verify startup",&Verify); ImGui::SameLine(); ImGui::Checkbox("Create archive",&Archive);
        if(ImGui::Button("Preview")) Start(true);
        ImGui::SameLine(); if(ImGui::Button("Export")) ConfirmSave=true;
        if(ConfirmSave) {
            ImGui::TextWrapped("Save changes before exporting? Inputs must stay unchanged until export finishes.");
            if(ImGui::Button("Save All and Export")) {
                bool Ready=Editors!=nullptr;
                if(Editors) for(const auto& Toolkit:Editors->GetAssetToolkits()) Ready=Toolkit->GetDocumentManager()->PrepareForExit() && Ready;
                if(Ready) for(const auto& Toolkit:Editors->GetAssetToolkits()) Ready=Toolkit->GetDocumentManager()->SaveDocumentsForExit() && Ready;
                if(Ready) {ConfirmSave=false; Start(false);} else {std::lock_guard Lock(Mutex); Message="Save could not finish. Resolve pending changes and retry.";}
            }
            ImGui::SameLine(); if(ImGui::Button("Cancel export")) ConfirmSave=false;
        }
        ImGui::EndDisabled();
        if(Running && ImGui::Button("Cancel")) Cancel=true;
        {std::lock_guard Lock(Mutex); ImGui::TextWrapped("%s",Message.c_str());}
        ImGui::End();
    }
}
