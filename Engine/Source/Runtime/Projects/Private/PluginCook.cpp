#include "Plugins/PluginCook.h"
#include "Plugins/PluginManagement.h"
#include "Project/Project.h"
#include "ProjectDescriptor.h"
#include "Project/ProjectContextInternal.h"
#include "Misc/Paths.h"
#include "Misc/RuntimePaths.h"
#include "Misc/FileHelper.h"
#include "Misc/Version.h"
#include "Misc/StringHelper.h"
#include "Misc/AppConfig.h"
#include "Json/Json.h"


namespace Durin
{
    auto PluginFileSha256(const std::filesystem::path& File) -> std::string { return FileSha256(File); }
    namespace
    {
        auto ValidateGameCookTarget(const FGameCookTarget& Target,std::string_view Platform,const FPluginManagementContext& Host) -> void
        {
            auto Architecture=Host.Identity.Architecture; if(Architecture=="AMD64" || Architecture=="x86_64") Architecture="x64";
            const auto* Project=GetCurrentProject();
            if(Target.Identity.Platform!=Platform || Target.Identity.Platform!=Host.Identity.Platform || Target.Identity.Architecture!=Architecture
                || Target.Identity.RuntimeVariant!="DurinGame" || (Target.Identity.Configuration!="Debug" && Target.Identity.Configuration!="Release")
                || Host.Identity.RuntimeVariant!="DurinEditor" || Target.Identity.ToolchainABI!=Host.Identity.ToolchainABI
                || !Target.Identity.EngineBuild.starts_with(std::string(GetEngineVersionString())+":")
                || Target.Compatibility!=DURIN_COOK_COMPATIBILITY || Target.ReceiptSha256.size()!=64 || !Project || Target.Project!=Project->Name)
                throw std::runtime_error("Cook host/target schema, providers, project or ABI are incompatible; rebuild matching host and Game products");
        }
    }
    auto ReadGameCookTarget(const std::filesystem::path& Receipt,std::string_view Platform,FGameCookTarget& OutTarget,std::string& OutReason) -> bool
    {
        try
        {
            FJsonDocument Document;
            const auto Bytes=FFileHelper::LoadFileToArray(Receipt);
            if (!Receipt.is_absolute() || !Bytes || !Document.Parse(std::string_view(reinterpret_cast<const char*>(Bytes->data()),Bytes->size())))
                throw std::runtime_error("Cook requires an absolute completed target receipt");
            const auto R=Document.GetRootView(),I=R.GetView("Identity");
            const auto Text=[](FJsonNodeView N,std::string_view K) {const auto V=N.GetView(K); if(!V.IsString() || V.GetString().empty()) throw std::runtime_error("Missing Cook target field: "+std::string(K)); return V.GetString();};
            FGameCookTarget Target;
            Target.Identity={Text(I,"EngineBuild"),Text(I,"Platform"),Text(I,"Architecture"),Text(I,"Configuration"),Text(I,"RuntimeVariant"),Text(I,"ToolchainABI")};
            Target.Project=Text(I,"Project"); Target.Compatibility=Text(R,"CookCompatibility"); Target.ReceiptSha256=BytesSha256(*Bytes);
            FPluginManagementContext Host;
            if(!MakePluginManagementContext(Host,OutReason)) return false;
            if(R.GetView("SchemaVersion").GetInt()!=1 || Text(R,"Status")!="succeeded" || Text(I,"Target")!="DurinLauncher")
                throw std::runtime_error("Cook requires a completed Game launcher receipt");
            ValidateGameCookTarget(Target,Platform,Host);
            OutTarget=std::move(Target); OutReason.clear(); return true;
        }
        catch(const std::exception& Error) {OutReason=Error.what(); return false;}
    }
    auto MakeRuntimeCookSnapshot(std::string_view EvidenceJson,std::string& OutJson,std::string& OutReason) -> bool
    {
        try
        {
            FJsonDocument Evidence,Selection,Descriptor;
            if (!Evidence.Parse(EvidenceJson)) throw std::runtime_error("Invalid Cook evidence");
            const auto E=Evidence.GetRootView();
            if (E.GetView("SchemaVersion").GetInt()!=1 || !Selection.Parse(E.GetView("Selection").GetString())
                || !Descriptor.Parse(E.GetView("ProjectDescriptor").GetString())) throw std::runtime_error("Unsupported Cook evidence; recook");
            FJsonDocument Runtime,Frozen; auto R=Runtime.GetMutableRoot(); R.EnsureObject(); R.SetChildValue("SchemaVersion",2);
            for (const auto* Key:{"EngineVersion","Platform","Architecture","Configuration","ToolchainABI","GameBuild","TargetReceiptSha256","CookCompatibility"}) R.SetChildValue(Key,E.GetView(Key).GetString());
            const auto D=Descriptor.GetRootView(); const auto Name=D.GetView("ProjectName").GetString();
            R.SetChildValue("ApplicationId",E.GetView("ApplicationId").GetString("org.durin."+Name));
            auto P=R.AddObject("Project"); P.SetChildValue("Name",Name); auto Roots=P.AddArray("BaseModules");
            auto Base=D.GetView("BaseModules"); std::set<std::string> Names;
            const auto Root=[&](std::string Name){if (Names.insert(Name).second) Roots.AppendValue(Name);};
            if (Base.IsArray()) for (size_t I=0;I<Base.Num();++I) Root(Base.GetView(I).GetString());
            else D.GetView("ModuleDirs").ForEachObjectMember([&](std::string_view N,FJsonNodeView){Root(std::string(N));});
            auto Extra=D.GetView("ExtraModules").GetView("DurinGame"); if (Extra.IsObject()) Extra=Extra.GetView("Modules");
            for (size_t I=0;I<Extra.Num();++I) Root(Extra.GetView(I).GetString());
            auto F=Frozen.GetMutableRoot(); F.EnsureObject(); F.SetChildValue("SchemaVersion",1); F.SetChildValue("Platform",E.GetView("Platform").GetString()); F.SetChildValue("RuntimeVariant","DurinGame");
            const auto S=Selection.GetRootView(); auto Plugins=F.AddArray("Plugins");
            const auto SourcePlugins=S.GetView("Plugins"); for (size_t I=0;I<SourcePlugins.Num();++I) Plugins.AppendObject().SetChildValue("Id",SourcePlugins.GetView(I).GetView("Id").GetString());
            auto Modules=F.AddArray("Modules"); const auto SourceModules=S.GetView("Modules");
            for (size_t I=0;I<SourceModules.Num();++I) {auto M=Modules.AppendObject(); for (const auto* Key:{"Name","Owner","Host","Phase"}) M.SetChildValue(Key,SourceModules.GetView(I).GetView(Key).GetString());}
            const auto FrozenJson=Frozen.ToString(EJsonFormat::Compact); R.SetChildValue("Selection",FrozenJson);
            R.SetChildValue("SelectionFingerprint",BytesSha256(std::as_bytes(std::span(FrozenJson.data(),FrozenJson.size()))));
            auto Mounts=R.AddArray("Mounts"); const auto SourceMounts=E.GetView("Mounts");
            for (size_t I=0;I<SourceMounts.Num();++I)
            { auto M=Mounts.AppendObject(); M.SetChildValue("VirtualRoot",SourceMounts.GetView(I).GetView("VirtualRoot").GetString()); auto Refs=M.AddArray("CanReference"); const auto Values=SourceMounts.GetView(I).GetView("CanReference"); for (size_t J=0;J<Values.Num();++J) Refs.AppendValue(Values.GetView(J).GetString()); }
            auto Hashes=R.AddObject("ConfigurationHashes");
            for (const auto* Key:{"Project.yaml","DurinGame.yaml","EngineGameDefaults"})
            {
                const auto V=E.GetView(Key); if (!V.IsString()) throw std::runtime_error("Missing frozen config");
                const auto Text=V.GetString(); Hashes.SetChildValue(Key,BytesSha256(std::as_bytes(std::span(Text.data(),Text.size()))));
            }
            OutJson=Runtime.ToString(); OutReason.clear(); return true;
        }
        catch (const std::exception& Error) { OutReason=Error.what(); return false; }
    }
    auto CapturePluginCookSnapshot(std::string_view Platform, const FGameCookTarget& Target, std::string& OutJson, std::string& OutReason) -> bool
    {
        try
        {
            FPluginManagementContext Context;
            if (!MakePluginManagementContext(Context,OutReason)) return false;
            Context.Request.Platform=Platform; Context.Request.RuntimeVariant="DurinGame";
            const auto Selection=ResolvePlugins(Context.Request);
            if (!Selection) { OutReason=Selection.error(); return false; }
            // The cook host must have admitted the same logical resources/providers.
            auto CookIds=Selection->Plugins; auto HostIds=Context.ActivePlugins;
            std::ranges::sort(CookIds); std::ranges::sort(HostIds);
            if (CookIds!=HostIds) throw std::runtime_error("Game Cook plugin closure differs from the admitted cook host");
            FPluginResources Resources;
            if (!ResolvePluginResources(*Selection,Resources,OutReason)) return false;
            FJsonDocument Snapshot; auto Root=Snapshot.GetMutableRoot(); Root.EnsureObject();
            Root.SetChildValue("SchemaVersion",1); Root.SetChildValue("Selection",Selection->ManifestJson);
            Root.SetChildValue("EngineVersion",GetEngineVersionString());
            Root.SetChildValue("Platform",Context.Request.Platform);
            Root.SetChildValue("Architecture",Context.Identity.Architecture);
            ValidateGameCookTarget(Target,Platform,Context);
            Root.SetChildValue("Configuration",Target.Identity.Configuration);
            Root.SetChildValue("GameBuild",Target.Identity.EngineBuild);
            Root.SetChildValue("TargetReceiptSha256",Target.ReceiptSha256);
            Root.SetChildValue("CookCompatibility",Target.Compatibility);
            auto Host=Root.AddObject("Host"); Host.SetChildValue("EngineBuild",Context.Identity.EngineBuild);
            Host.SetChildValue("Configuration",Context.Identity.Configuration); Host.SetChildValue("RuntimeVariant",Context.Identity.RuntimeVariant);
            Host.SetChildValue("Platform",Context.Identity.Platform); Host.SetChildValue("Architecture",Context.Identity.Architecture);
            Host.SetChildValue("ToolchainABI",Context.Identity.ToolchainABI); Host.SetChildValue("CookCompatibility",DURIN_COOK_COMPATIBILITY);
            Root.SetChildValue("ToolchainABI",Context.Identity.ToolchainABI);
            const auto* Project=GetCurrentProject();
            if (!Project) throw std::runtime_error("Cook requires a project");
            const auto ProjectBytes=FFileHelper::LoadFileToArray(Project->ProjectFile);
            if (!ProjectBytes) throw std::runtime_error("Cannot capture project descriptor");
            Root.SetChildValue("ProjectDescriptor",std::string(reinterpret_cast<const char*>(ProjectBytes->data()),ProjectBytes->size()));
            FYamlDocument Settings; std::string ApplicationId;
            if (Settings.LoadFromFile(std::filesystem::path(Project->ProjectDir)/"Configs/Project.yaml"))
                Settings.GetRootView().GetView("Export").GetView("ApplicationId").GetValue(ApplicationId);
            if (ApplicationId.empty()) ApplicationId="org.durin."+Project->Name;
            (void)ResolveRuntimeUserDataRoot(ApplicationId,std::filesystem::path(Project->ProjectDir));
            Root.SetChildValue("ApplicationId",ApplicationId);
            const auto Startup = ReadPluginStartupSnapshot(Project->ProjectFile);
            if (!Startup) throw std::runtime_error(Startup.error());
            Root.SetChildValue("PluginStartupConfig", Startup->empty() ? std::string("{}") : *Startup);
            auto Mounts=Root.AddArray("Mounts");
            for (const auto& Mount:Resources.ContentMounts)
            {
                auto Item=Mounts.AppendObject(); Item.SetChildValue("VirtualRoot",Mount.VirtualRoot);
                auto References=Item.AddArray("CanReference"); for (const auto& Reference:Mount.Dependencies) References.AppendValue(Reference);
            }
            const auto ProjectDir=std::filesystem::path(Project->ProjectDir);
            const auto GameDefaults=FFileHelper::LoadFileToArray(std::filesystem::path(FPaths::EngineDir())/"Configs/TP_DurinGame.yaml");
            if (!GameDefaults) throw std::runtime_error("Cannot capture Engine Game defaults");
            Root.SetChildValue("EngineGameDefaults",std::string(reinterpret_cast<const char*>(GameDefaults->data()),GameDefaults->size()));
            for (const auto* Config:{"Project.yaml","DurinGame.yaml"})
            {
                std::vector<std::filesystem::path> Layers{std::filesystem::path(FPaths::EngineDir())/"Configs"/(std::string_view(Config)=="DurinGame.yaml"?"TP_DurinGame.yaml":Config)};
                for (const auto& Directory:Resources.ConfigDirectories) Layers.push_back(Directory/Config);
                Layers.push_back(ProjectDir/"Configs"/Config);
                FYamlDocument Effective;
                if (!LoadConfigLayers(Layers,Effective,OutReason)) return false;
                Root.SetChildValue(Config,Effective.ToString());
            }
            OutJson=Snapshot.ToString(); OutReason.clear(); return true;
        }
        catch (const std::exception& Error) { OutReason=Error.what(); return false; }
    }
    auto ReadPluginRuntimeInventory(const std::filesystem::path& File,std::string_view ProjectFile,
        const FPluginBuildIdentity& Host,FPluginResolution& OutSelection,
        std::vector<FAdmittedPluginBinary>& OutBinaries,FPluginResources& OutResources,std::string& OutReason) -> bool
    {
        try
        {
            const auto Fail=[](std::string Reason) -> void { throw std::runtime_error(std::move(Reason)); };
            FJsonDocument Document;
            if (!Document.LoadFromFile(File)) Fail("Missing or invalid cooked plugin runtime inventory");
            const auto Root=Document.GetRootView();
            if (!Root.GetView("SchemaVersion").IsInt() || Root.GetView("SchemaVersion").GetInt()!=2) Fail("Unsupported cooked plugin runtime schema; restage the Game");
            const auto Text=[&](FJsonNodeView Node,std::string_view Key) {
                auto Value=Node.GetView(Key);
                if (!Value.IsString() || Value.GetString().empty()) Fail("Missing runtime inventory field: "+std::string(Key));
                return Value.GetString();
            };
            const auto Identity=Root.GetView("Identity");
            for (const auto& [Key,Expected]:{std::pair{"EngineBuild",Host.EngineBuild},{"Platform",Host.Platform},
                {"Architecture",Host.Architecture},{"Configuration",Host.Configuration},{"RuntimeVariant",Host.RuntimeVariant},{"ToolchainABI",Host.ToolchainABI}})
                if (Expected.empty() || Text(Identity,Key)!=Expected) Fail("Incompatible cooked plugin identity: "+std::string(Key));
            const auto Base=std::filesystem::canonical(File).parent_path();
            const auto Path=[&](const std::string& Relative) { return ResolveDeploymentPath(Base,Relative); };
            (void)ProjectFile; // Staged admission uses the detached runtime view.
            const auto Files=Root.GetView("Files");
            if (!Files.IsArray() || Files.Num()==0 || Files.Num()>65536) Fail("Invalid runtime file inventory");
            std::set<std::filesystem::path> Verified;
            for (size_t I=0;I<Files.Num();++I)
            {
                const auto Item=Files.GetView(I); const auto Resolved=Path(Text(Item,"Path"));
                if (!Verified.insert(Resolved).second || PluginFileSha256(Resolved)!=Text(Item,"Sha256")) Fail("Missing or modified staged runtime file: "+Resolved.generic_string());
            }
            const auto Required=[&](const std::string& Relative) {
                auto Resolved=Path(Relative); if (!Verified.contains(Resolved)) Fail("Unverified runtime dependency: "+Relative); return Resolved;
            };
            const auto Cook=Root.GetView("Cook");
            (void)Required(Text(Cook.GetView("Manifest"),"Path"));
            const auto SnapshotFile=Required(Text(Cook.GetView("Snapshot"),"Path"));
            FJsonDocument Snapshot, Frozen;
            if (!Snapshot.LoadFromFile(SnapshotFile) || !Frozen.Parse(Snapshot.GetRootView().GetView("Selection").GetString()))
                Fail("Invalid frozen Cook plugin selection");
            const auto Captured=Snapshot.GetRootView();
            if (!Captured.GetView("SchemaVersion").IsInt() || Captured.GetView("SchemaVersion").GetInt()!=2)
                Fail("Unsupported frozen runtime Cook schema; recook/restage");
            if (Text(Cook,"SelectionFingerprint")!=Text(Captured,"SelectionFingerprint")) Fail("Cook/runtime selection identity differs");
            const auto SelectionJson=Text(Captured,"Selection");
            if (BytesSha256(std::as_bytes(std::span(SelectionJson.data(),SelectionJson.size())))!=Text(Captured,"SelectionFingerprint"))
                Fail("Invalid frozen selection fingerprint");
            if (Text(Root,"ApplicationId")!=Text(Captured,"ApplicationId")) Fail("Cook/runtime application identity differs");
            if (Text(Root.GetView("Project"),"Name")!=Text(Captured.GetView("Project"),"Name")) Fail("Cook/runtime project identity differs");
            const auto BaseModules=Root.GetView("Project").GetView("BaseModules");
            const auto FrozenBaseModules=Captured.GetView("Project").GetView("BaseModules");
            if (!BaseModules.IsArray() || !FrozenBaseModules.IsArray() || BaseModules.Num()!=FrozenBaseModules.Num()) Fail("Cook/runtime base modules differ");
            for (size_t I=0;I<BaseModules.Num();++I)
                if (BaseModules.GetView(I).GetString()!=FrozenBaseModules.GetView(I).GetString()) Fail("Cook/runtime base modules differ");
            if (Text(Captured,"GameBuild")!=Host.EngineBuild) Fail("Cook/runtime Game build differs; recook/restage");
            const auto FrozenRoot=Frozen.GetRootView();
            if (!FrozenRoot.GetView("SchemaVersion").IsInt() || FrozenRoot.GetView("SchemaVersion").GetInt()!=1
                || Text(FrozenRoot,"RuntimeVariant")!="DurinGame" || Text(FrozenRoot,"Platform")!=Host.Platform
                || !FrozenRoot.GetView("Plugins").IsArray() || !FrozenRoot.GetView("Modules").IsArray())
                Fail("Invalid frozen Game plugin selection");
            for (const auto& [Key,Expected]:{std::pair{"Platform",Host.Platform},{"Architecture",Host.Architecture},
                {"Configuration",Host.Configuration},{"ToolchainABI",Host.ToolchainABI}})
            {
                auto Actual=Text(Captured,Key);
                if (std::string_view(Key)=="Architecture" && (Actual=="AMD64" || Actual=="x86_64")) Actual="x64";
                if (Actual!=Expected) Fail("Cook/runtime target identity differs: "+std::string(Key));
            }
            (void)Required(Text(Cook.GetView("ShaderLibrary"),"Path"));
            const auto Configs=Root.GetView("ConfigurationFiles");
            for (const auto& [Binding,Name]:{std::pair{"Project","Project.yaml"},{"Application","DurinGame.yaml"},{"Defaults","EngineGameDefaults"}})
            {
                const auto Bytes=FFileHelper::LoadFileToArray(Required(Text(Configs,Binding)));
                const auto Expected=Captured.GetView("ConfigurationHashes").GetView(Name);
                if (!Bytes || !Expected.IsString() || FileSha256(Required(Text(Configs,Binding)))!=Expected.GetString())
                    Fail("Runtime configuration differs from Cook: "+std::string(Binding));
            }
            FPluginResolution Selection;
            const auto Plugins=Root.GetView("Plugins");
            if (!Plugins.IsArray()) Fail("Invalid cooked plugin selection");
            std::set<std::string> PluginIds;
            for (size_t I=0;I<Plugins.Num();++I)
            {
                const auto Id=Plugins.GetView(I).GetString();
                if (!IsValidProjectIdentifier(Id) || !PluginIds.insert(StringUtils::FoldAscii(Id)).second) Fail("Invalid cooked plugin identity");
                Selection.Plugins.push_back(Id);
            }
            const auto FrozenPlugins=Frozen.GetRootView().GetView("Plugins");
            if (FrozenPlugins.Num()!=Selection.Plugins.size()) Fail("Runtime plugin selection differs from Cook");
            for (size_t I=0;I<FrozenPlugins.Num();++I)
                if (FrozenPlugins.GetView(I).GetView("Id").GetString()!=Selection.Plugins[I]) Fail("Runtime plugin selection differs from Cook");
            std::vector<FAdmittedPluginBinary> Binaries; std::set<std::string> Modules;
            const auto Native=Root.GetView("Binaries");
            if (!Native.IsArray()) Fail("Invalid cooked native plugin inventory");
            const auto FrozenModules=Frozen.GetRootView().GetView("Modules");
            if (FrozenModules.Num()!=Native.Num()) Fail("Cook/runtime module inventory differs");
            int LastPhase=-1;
            for (size_t I=0;I<Native.Num();++I)
            {
                const auto Item=Native.GetView(I);
                FResolvedPluginModule Module{Text(Item,"Module"),Text(Item,"Owner"),"Runtime",Text(Item,"Phase")};
                if (!IsValidProjectIdentifier(Module.Name) || !Modules.insert(StringUtils::FoldAscii(Module.Name)).second
                    || !PluginIds.contains(StringUtils::FoldAscii(Module.Owner))) Fail("Invalid runtime module owner/name");
                const auto FrozenModule=FrozenModules.GetView(I);
                if (Module.Name!=FrozenModule.GetView("Name").GetString() || Module.Owner!=FrozenModule.GetView("Owner").GetString()
                    || Module.Phase!=FrozenModule.GetView("Phase").GetString() || FrozenModule.GetView("Host").GetString()!="Runtime") Fail("Cook/runtime module inventory differs");
                const int Phase=Module.Phase=="Providers"?0:Module.Phase=="EngineReady"?1:-1;
                if (Phase<0 || Phase<LastPhase) Fail("Invalid runtime plugin phase"); LastPhase=Phase;
                FAdmittedPluginBinary Binary{Module,Required(Text(Item,"Path")),{}};
                const auto Dependencies=Item.GetView("RuntimeDependencies");
                if (!Dependencies.IsArray()) Fail("Invalid native dependency list");
                for (size_t J=0;J<Dependencies.Num();++J) Binary.RuntimeDependencies.push_back(Required(Dependencies.GetView(J).GetString()));
                Selection.Modules.push_back(Module); Binaries.push_back(std::move(Binary));
            }
            FPluginResources Resources;
            const auto Mounts=Root.GetView("Mounts");
            const auto FrozenMounts=Captured.GetView("Mounts");
            if (!Mounts.IsArray() || !FrozenMounts.IsArray() || Mounts.Num()!=FrozenMounts.Num()+2) Fail("Invalid cooked plugin mounts");
            std::set<std::string> VirtualRoots;
            for (size_t I=0;I<FrozenMounts.Num();++I)
            {
                const auto Item=Mounts.GetView(I+2); const auto Virtual=Text(Item,"VirtualRoot");
                const auto FrozenMount=FrozenMounts.GetView(I);
                if (Virtual!=Text(FrozenMount,"VirtualRoot")) Fail("Cook/runtime mount bindings differ");
                if (!Virtual.starts_with("/Plugins/") || !Virtual.ends_with('/') || !VirtualRoots.insert(StringUtils::FoldAscii(Virtual)).second
                    || !PluginIds.contains(StringUtils::FoldAscii(Virtual.substr(9,Virtual.size()-10)))) Fail("Invalid cooked plugin mount identity");
                FMountPoint Mount; Mount.VirtualRoot=Virtual; Mount.Root=Path(Text(Item,"Path")); Mount.ContentPath="."; Mount.bContentWritable=false;
                Mount.bAutoScan=true; Mount.Owner=EMountOwner::Extension;
                const auto References=Item.GetView("CanReference");
                const auto FrozenReferences=FrozenMount.GetView("CanReference");
                if (!References.IsArray() || !FrozenReferences.IsArray() || References.Num()!=FrozenReferences.Num()) Fail("Invalid cooked mount references");
                for (size_t J=0;J<References.Num();++J)
                {
                    if (!References.GetView(J).IsString() || References.GetView(J).GetString()!=FrozenReferences.GetView(J).GetString()) Fail("Cook/runtime mount references differ");
                    Mount.Dependencies.push_back(References.GetView(J).GetString());
                }
                Resources.ContentMounts.push_back(std::move(Mount));
            }
            OutSelection=std::move(Selection); OutBinaries=std::move(Binaries); OutResources=std::move(Resources); OutReason.clear(); return true;
        }
        catch (const std::exception& Error) { OutReason=Error.what(); return false; }
    }
}
