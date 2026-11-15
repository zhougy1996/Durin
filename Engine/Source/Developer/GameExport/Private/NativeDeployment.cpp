#include "NativeDeployment.h"
#include "Misc/FileHelper.h"
#include "Misc/RuntimePaths.h"
#include "Misc/StringHelper.h"

namespace Durin
{
    namespace
    {
        auto Need(bool Value,std::string Message) -> void { if (!Value) throw std::runtime_error(std::move(Message)); }
        auto Str(FJsonNodeView V,std::string_view K) -> std::string
        { const auto S=V.GetView(K); Need(S.IsString() && !S.GetString().empty(),"Receipt missing string: "+std::string(K)); return S.GetString(); }
        auto Strings(FJsonNodeView Node) -> std::vector<std::string>
        {
            Need(Node.IsArray() && Node.Num()<=65536,"Invalid receipt string array");
            std::vector<std::string> Result;
            for(size_t I=0;I<Node.Num();++I) { Need(Node.GetView(I).IsString() && !Node.GetView(I).GetString().empty(),"Invalid receipt array entry"); Result.push_back(Node.GetView(I).GetString()); }
            return Result;
        }
        auto Copy(FJsonNodeRef To,FJsonNodeView From) -> void
        {
            if (From.IsObject())
            {
                To.EnsureObject(); std::map<std::string,FJsonNodeView> Fields;
                From.ForEachObjectMember([&](std::string_view K,FJsonNodeView V){Fields.emplace(K,V);});
                for(const auto& [K,V]:Fields) Copy(To.AddObject(K),V);
            }
            else if (From.IsArray()) {To.EnsureArray(); for(size_t I=0;I<From.Num();++I) Copy(To.AppendObject(),From.GetView(I));}
            else if (From.IsString()) To.SetValue(From.GetString());
            else if (From.IsBool()) To.SetValue(From.GetBool());
            else if (From.IsInt()) To.SetValue(From.GetInt());
            else if (From.IsUInt()) To.SetValue(From.GetUInt());
            else To.SetValue(nullptr);
        }
        auto SourcePath(FJsonNodeView Ref,const FNativeReceiptRoots& Roots,bool Input) -> std::filesystem::path
        {
            const auto Root=Str(Ref,"Root"),Path=Str(Ref,"Path");
            if(Root=="Host") { Need(Input,"Host input cannot authorize a runtime product"); const auto File=std::filesystem::u8path(Path); Need(File.is_absolute(),"Host native input must be absolute"); return std::filesystem::canonical(File); }
            if(Root=="Workspace") { Need(Input,"Workspace input cannot authorize a runtime product"); return ResolveDeploymentPath(Roots.Engine.parent_path(),Path); }
            if(Root=="Engine") return ResolveDeploymentPath(Roots.Engine,Path);
            if(Root=="Project") return ResolveDeploymentPath(!Input && !Roots.ProjectProducts.empty()?Roots.ProjectProducts:Roots.Project.parent_path(),Path);
            if(Root=="Artifacts") return ResolveDeploymentPath(Roots.Artifacts,Path);
            Need(Root=="Dependencies","Invalid receipt source root"); return ResolveDeploymentPath(Roots.Dependencies,Path);
        }
        auto Reachable(const FNativeReceipt& Receipt,std::vector<std::string> Queue,const std::set<std::string>& Owners) -> std::set<std::string>
        {
            std::set<std::string> Selected;
            for(size_t I=0;I<Queue.size();++I)
            {
                if(!Selected.insert(Queue[I]).second) continue;
                Need(Receipt.Modules.contains(Queue[I]),"Missing declared native module: "+Queue[I]);
                const auto& Module=Receipt.Modules.at(Queue[I]);
                Need(Owners.contains(Module.Owner),"Inactive plugin/project native edge: "+Queue[I]);
                Queue.insert(Queue.end(),Module.Linked.begin(),Module.Linked.end());
                Queue.insert(Queue.end(),Module.Dynamic.begin(),Module.Dynamic.end());
            }
            return Selected;
        }
    }

    auto ReadNativeReceipt(const std::filesystem::path& File,const FNativeReceiptRoots& Roots,
        FJsonNodeView Snapshot,FJsonNodeView Selection) -> FNativeReceipt
    {
        FNativeReceipt Result;
        const auto Bytes=FFileHelper::LoadFileToArray(File);
        Need(Bytes.has_value(),"Missing completed Game receipt; build the project GameReceipt target");
        Result.Bytes.assign(reinterpret_cast<const char*>(Bytes->data()),Bytes->size());
        Need(Result.Document.Parse(Result.Bytes).has_value(),"Invalid completed Game receipt");
        Need(BytesSha256(std::as_bytes(std::span(Result.Bytes.data(),Result.Bytes.size())))==Str(Snapshot,"TargetReceiptSha256"),"Cook target receipt differs; recook for the selected Game build");
        const auto R=Result.Document.GetRootView(),Identity=R.GetView("Identity");
        Need(R.GetView("SchemaVersion").GetInt()==1 && Str(R,"Status")=="succeeded","Unsupported/incomplete target receipt");
        for(const auto* Key:{"Platform","Configuration","ToolchainABI"}) Need(Str(Identity,Key)==Str(Snapshot,Key),"Wrong receipt target/ABI: "+std::string(Key));
        Need(Str(Identity,"Platform")=="Win64" && Str(Identity,"Architecture")=="x64" && Str(Identity,"RuntimeVariant")=="DurinGame"
            && Str(Identity,"Target")=="DurinLauncher" && Str(Identity,"Project")==Str(Snapshot.GetView("Project"),"Name"),"Wrong receipt project/runtime identity");
        Result.EngineBuild=Str(Identity,"EngineBuild");
        Need(Result.EngineBuild==Str(Snapshot,"GameBuild") && Str(R,"CookCompatibility")==Str(Snapshot,"CookCompatibility"),"Cook Game identity/compatibility differs from receipt");
        const auto Prerequisites=R.GetView("Prerequisites");
        Need(Str(Prerequisites,"SystemPolicy")=="win64-system-v1" && Str(Prerequisites,"CRT")== (Str(Identity,"Configuration")=="Debug"?"developer-debug-crt":"vcredist-x64"),"Wrong receipt prerequisite policy");
        for(const auto* Key:{"MinimumVersion","Provenance","License"}) (void)Str(Prerequisites,Key);
        const auto Inputs=R.GetView("Inputs");
        Need(Inputs.IsArray() && Inputs.Num()>0 && Inputs.Num()<=131072,"Missing/bounded receipt native inputs");
        std::string Fingerprint,Previous;
        for(size_t I=0;I<Inputs.Num();++I)
        {
            const auto Input=Inputs.GetView(I); const auto Key=Str(Input,"Root")+"/"+Str(Input,"Path"),Hash=Str(Input,"Sha256");
            Need(Previous.empty() || Previous<Key,"Duplicate/unsorted receipt native inputs"); Previous=Key;
            const auto Root=Str(Input,"Root");
            Need(Root=="Engine" || Root=="Project" || Root=="Artifacts" || Root=="Dependencies" || Root=="Workspace" || Root=="Host","Invalid native input root");
            if(!Roots.Prebuilt || Root=="Project") {
                const auto File=SourcePath(Input,Roots,true);
                if(!Roots.Prebuilt || std::filesystem::exists(File)) Need(FileSha256(File)==Hash,"Stale native build input: "+Key+"; rebuild receipt");
            }
            Fingerprint+=Key+":"+Hash+"\n";
        }
        Need(BytesSha256(std::as_bytes(std::span(Fingerprint.data(),Fingerprint.size())))==Str(R,"InputFingerprint"),"Invalid native input fingerprint");
        const auto Built=R.GetView("BuildInputs");
        Need(Built.IsArray(),"Missing receipt plugin build inputs; rebuild GameReceipt");
        std::map<std::string,std::string> BuiltInputs;
        for(size_t I=0;I<Built.Num();++I) Need(BuiltInputs.emplace(Str(Built.GetView(I),"Path"),Str(Built.GetView(I),"Hash")).second,"Duplicate plugin build input");
        const auto CurrentInputs=Selection.GetView("Inputs"),Plugins=Selection.GetView("Plugins");
        for(size_t I=0;I<CurrentInputs.Num() && Plugins.Num()>0;++I)
        {
            const auto Input=CurrentInputs.GetView(I); const auto Path=Str(Input,"Path");
            if(Path.starts_with("Plugin/"))
            {
                const auto Owner=Path.substr(7,Path.find('/',7)-7); bool Selected=false;
                for(size_t J=0;J<Plugins.Num();++J) if(Str(Plugins.GetView(J),"Id")==Owner) Selected=true;
                if(!Selected) continue;
            }
            else if(std::ranges::count(Path,'/')!=2) continue;
            Need(BuiltInputs.contains(Path) && BuiltInputs.at(Path)==Str(Input,"Hash"),"Stale plugin build input: "+Path+"; rebuild GameReceipt");
        }
        const auto Modules=R.GetView("Modules"),Products=R.GetView("Products");
        Need(Modules.IsArray() && Modules.Num()>0 && Modules.Num()<=65536 && Products.IsArray() && Products.Num()>0 && Products.Num()<=65536,"Invalid receipt closure");
        std::set<std::string> Names,Destinations;
        for(size_t I=0;I<Modules.Num();++I)
        {
            const auto M=Modules.GetView(I); FNativeReceiptModule Module;
            Module.Name=Str(M,"Name"); Module.Owner=Str(M,"Owner"); Module.Record=I;
            Need(Module.Name.size()<=128 && std::ranges::all_of(Module.Name,[](char C){return (C>='A' && C<='Z') || (C>='a' && C<='z') || (C>='0' && C<='9') || C=='_';})
                && Names.insert(StringUtils::FoldAscii(Module.Name)).second,"Duplicate/invalid receipt module");
            Need(Str(M,"Host")=="Runtime","Editor module in Game receipt");
            Module.Linked=Strings(M.GetView("Linked")); Module.Dynamic=Strings(M.GetView("Dynamic")); Module.RuntimeFiles=Strings(M.GetView("RuntimeFiles"));
            Module.Binary=M.GetView("Binary").GetString(); Module.Phase=M.GetView("Phase").GetString();
            Result.Modules.emplace(Module.Name,std::move(Module));
        }
        for(size_t I=0;I<Products.Num();++I)
        {
            const auto P=Products.GetView(I); FNativeReceiptProduct Product;
            Product.Module=Str(P,"Module"); Product.Destination=Str(P,"Destination"); Product.Kind=Str(P,"Kind"); Product.Sha256=Str(P,"Sha256"); Product.Record=I;
            (void)ResolveDeploymentPath(Roots.Artifacts,Product.Destination);
            Need(Destinations.insert(StringUtils::FoldAscii(Product.Destination)).second,"Colliding receipt destination: "+Product.Destination);
            Need(Str(P,"Host")=="Runtime" && Result.Modules.contains(Product.Module) && Str(P,"Owner")==Result.Modules.at(Product.Module).Owner,"Invalid receipt product ownership/host");
            Need(Product.Kind=="Executable" || Product.Kind=="NativeLibrary" || Product.Kind=="RuntimeData" || Product.Kind=="License","Invalid runtime category");
            Product.Source=SourcePath(P.GetView("Source"),Roots,false);
            if(P.Contains("License") || P.Contains("Provenance")) { (void)Str(P,"License"); (void)Str(P,"Provenance"); }
            Result.Products.push_back(std::move(Product));
        }
        return Result;
    }

    auto SelectNativeDeployment(const FNativeReceipt& Receipt,FJsonNodeView Snapshot,const FPluginResolution& Selection) -> FNativeDeployment
    {
        const auto R=Receipt.Document.GetRootView();
        std::set<std::string> Owners{"Engine",Str(R.GetView("Identity"),"Project")}; for(const auto& Id:Selection.Plugins) Owners.insert(Id);
        std::vector<std::string> Roots{"DurinLauncher"};
        const auto ProjectRoots=Strings(Snapshot.GetView("Project").GetView("BaseModules")); Roots.insert(Roots.end(),ProjectRoots.begin(),ProjectRoots.end());
        for(const auto& Module:Selection.Modules) Roots.push_back(Module.Name);
        const auto Selected=Reachable(Receipt,std::move(Roots),Owners);
        std::set<std::string> Required,Found;
        for(const auto& Name:Selected) for(const auto& File:Receipt.Modules.at(Name).RuntimeFiles) Need(Required.insert(File).second,"Duplicate declared runtime file");
        FNativeDeployment Result; Result.EngineBuild=Receipt.EngineBuild;
        FJsonDocument Filtered; auto Out=Filtered.GetMutableRoot(); Out.EnsureObject();
        for(const auto* Key:{"SchemaVersion","Status","Identity","Inputs","BuildInputs","InputFingerprint","Prerequisites","CookCompatibility"}) Copy(Out.AddObject(Key),R.GetView(Key));
        auto Products=Out.AddArray("Products"),Modules=Out.AddArray("Modules");
        for(const auto& Name:Selected) Copy(Modules.AppendObject(),R.GetView("Modules").GetView(Receipt.Modules.at(Name).Record));
        for(const auto& Product:Receipt.Products) if(Selected.contains(Product.Module))
        {
            Need(Required.contains(Product.Destination) && Found.insert(Product.Destination).second,"Undeclared receipt product: "+Product.Destination);
            Result.Products.push_back(Product); Copy(Products.AppendObject(),R.GetView("Products").GetView(Product.Record));
        }
        Need(Found==Required,"Missing required runtime files");
        for(const auto& Module:Selection.Modules)
        {
            const auto& Built=Receipt.Modules.at(Module.Name);
            Need(Built.Owner==Module.Owner && Module.Host=="Runtime" && Built.Phase==Module.Phase && !Built.Binary.empty(),"Plugin module binding differs from receipt: "+Module.Name+"; rebuild GameReceipt");
            const auto Main=std::ranges::find(Result.Products,Built.Binary,&FNativeReceiptProduct::Destination);
            Need(Main!=Result.Products.end() && Main->Module==Module.Name && Main->Kind=="NativeLibrary","Invalid plugin binary binding: "+Module.Name);
            FNativeDeploymentPlugin Plugin{Module,Built.Binary,{}};
            const auto Dependencies=Reachable(Receipt,{Module.Name},Owners);
            for(const auto& Product:Result.Products) if(Dependencies.contains(Product.Module) && Product.Kind=="NativeLibrary" && Product.Destination!=Built.Binary) Plugin.Dependencies.push_back(Product.Destination);
            Result.Plugins.push_back(std::move(Plugin));
        }
        Out.SetChildValue("SelectionFingerprint",Str(Snapshot,"SelectionFingerprint"));
        Copy(Result.SelectedReceipt.GetMutableRoot(),Filtered.GetRootView());
        return Result;
    }

    auto ValidateNativeDeployment(const FNativeDeployment& Deployment,FJsonNodeView Snapshot) -> void
    {
        std::set<std::string> LoaderNames; size_t Executables=0; std::string CoreHash;
        const auto Project=Str(Snapshot.GetView("Project"),"Name");
        for(const auto& Product:Deployment.Products)
        {
            Need(FileSha256(Product.Source)==Product.Sha256,"Stale/missing native artifact: "+Product.Source.generic_string()+"; rebuild receipt");
            if(Product.Kind=="NativeLibrary" || Product.Kind=="Executable")
            {
                Need(LoaderNames.insert(StringUtils::FoldAscii(std::filesystem::path(Product.Destination).filename().generic_string())).second,"Colliding loader basename: "+Product.Destination);
                Need(std::filesystem::path(Product.Destination).parent_path()=="Binaries/Win64","Native product outside binary directory");
            }
            if(Product.Kind=="Executable") { ++Executables; Need(Product.Module=="DurinLauncher" && Product.Destination=="Binaries/Win64/"+Project+".exe","Wrong receipt executable"); }
            if(Product.Module=="Core" && Product.Destination=="Binaries/Win64/DurinGame-Core.dll") CoreHash=Product.Sha256;
        }
        Need(Executables==1,"Missing required runtime executable");
        Need(!CoreHash.empty() && Deployment.EngineBuild==Str(Snapshot,"EngineVersion")+":"+CoreHash,"Receipt Engine build differs from Core product");
    }
}
