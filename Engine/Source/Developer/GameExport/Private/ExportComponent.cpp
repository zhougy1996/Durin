#include "ExportComponent.h"
#include "Misc/RuntimePaths.h"
namespace Durin {
    auto ReadExportComponent(const std::filesystem::path& File) -> const FJsonDocument& {
        // Tool installations are stable for the lifetime of their client/worker process.
        static std::mutex Mutex;
        static std::map<std::filesystem::path,FJsonDocument> Components;
        const std::lock_guard Guard(Mutex);
        const auto Canonical=std::filesystem::weakly_canonical(File);
        if(const auto Found=Components.find(Canonical); Found!=Components.end()) return Found->second;
        FJsonDocument D;
        if(!D.LoadFromFile(File)) throw std::runtime_error("Missing export component: "+File.generic_string());
        const auto R=D.GetRootView(), Files=R.GetView("Files");
        if(R.GetView("SchemaVersion").GetInt()!=1 || R.GetView("ProtocolVersion").GetInt()!=1 || R.GetView("Name").GetString()!="DurinExportTool"
            || !Files.IsArray() || !Files.Num() || Files.Num()>131072) throw std::runtime_error("Invalid export component metadata");
        std::set<std::string> Names;
        for(size_t I=0;I<Files.Num();++I) {
            auto F=Files.GetView(I); auto P=F.GetView("Path").GetString();
            if(!Names.insert(P).second || FileSha256(ResolveDeploymentPath(File.parent_path(),P))!=F.GetView("Sha256").GetString())
                throw std::runtime_error("Missing/stale export component file: "+P);
        }
        for(auto Key:{"Worker","BackendMetadata"}) if(!Names.contains(R.GetView(Key).GetString())) throw std::runtime_error("Component omits required file: "+std::string(Key));
        return Components.emplace(Canonical,std::move(D)).first->second;
    }
}
