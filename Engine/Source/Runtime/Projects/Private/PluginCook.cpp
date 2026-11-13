#include "Plugins/PluginCook.h"
#include "Plugins/PluginManagement.h"
#include "Project/Project.h"
#include "ProjectDescriptor.h"
#include "Project/ProjectContextInternal.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/Version.h"
#include "Misc/StringHelper.h"
#include "Misc/AppConfig.h"
#include "Json/Json.h"
#if defined(_WIN32)
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#endif

namespace Durin
{
    auto PluginFileSha256(const std::filesystem::path& File) -> std::string
    {
        const auto Bytes = FFileHelper::LoadFileToArray(File);
        if (!Bytes) throw std::runtime_error("Cannot hash required runtime file: " + File.generic_string());
        std::array<unsigned char,32> Digest{};
#if defined(_WIN32)
        if (Bytes->size()>std::numeric_limits<ULONG>::max()
            || BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<std::byte*>(Bytes->data())),
                static_cast<ULONG>(Bytes->size()),Digest.data(),static_cast<ULONG>(Digest.size()))<0)
            throw std::runtime_error("Cannot hash runtime file");
#elif defined(__APPLE__)
        if (Bytes->size()>std::numeric_limits<CC_LONG>::max()) throw std::runtime_error("Runtime file exceeds hash limit");
        CC_SHA256(Bytes->data(),static_cast<CC_LONG>(Bytes->size()),Digest.data());
#endif
        std::string Result; for (auto Byte:Digest) Result+=std::format("{:02x}",Byte); return Result;
    }
    auto CapturePluginCookSnapshot(std::string_view Platform, std::string& OutJson, std::string& OutReason) -> bool
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
            Root.SetChildValue("Configuration",Context.Identity.Configuration);
            Root.SetChildValue("ToolchainABI",Context.Identity.ToolchainABI);
            const auto* Project=GetCurrentProject();
            if (!Project) throw std::runtime_error("Cook requires a project");
            const auto ProjectBytes=FFileHelper::LoadFileToArray(Project->ProjectFile);
            if (!ProjectBytes) throw std::runtime_error("Cannot capture project descriptor");
            Root.SetChildValue("ProjectDescriptor",std::string(reinterpret_cast<const char*>(ProjectBytes->data()),ProjectBytes->size()));
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
            if (!Root.GetView("SchemaVersion").IsInt() || Root.GetView("SchemaVersion").GetInt()!=1) Fail("Unsupported cooked plugin runtime schema");
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
            const auto Path=[&](const std::string& Relative) {
                const std::filesystem::path Value(Relative);
                if (Value.empty() || Value.is_absolute() || Value.has_root_name() || Value.lexically_normal()!=Value
                    || std::ranges::any_of(Value,[](const auto& Part){return Part=="..";})) Fail("Invalid runtime relative path: "+Relative);
                const auto Result=std::filesystem::weakly_canonical(Base/Value);
                const auto Relation=Result.lexically_relative(Base);
                if (Relation.empty() || *Relation.begin()=="..") Fail("Runtime file escapes deployment: "+Relative);
                return Result;
            };
            if (std::filesystem::weakly_canonical(std::filesystem::path(ProjectFile))!=Path(Text(Root,"ProjectFile"))) Fail("Staged project/selection mismatch");
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
            (void)Required(Text(Root,"ProjectFile"));
            (void)Required("CookManifest.bin");
            const auto SnapshotFile=Required("PluginCook.json");
            FJsonDocument Snapshot, Frozen;
            if (!Snapshot.LoadFromFile(SnapshotFile) || !Frozen.Parse(Snapshot.GetRootView().GetView("Selection").GetString()))
                Fail("Invalid frozen Cook plugin selection");
            const auto Captured=Snapshot.GetRootView();
            if (!Captured.GetView("SchemaVersion").IsInt() || Captured.GetView("SchemaVersion").GetInt()!=1)
                Fail("Unsupported frozen Cook plugin schema");
            const auto FrozenRoot=Frozen.GetRootView();
            if (!FrozenRoot.GetView("SchemaVersion").IsInt() || FrozenRoot.GetView("SchemaVersion").GetInt()!=1
                || Text(FrozenRoot,"RuntimeVariant")!="DurinGame" || Text(FrozenRoot,"Platform")!=Host.Platform
                || !FrozenRoot.GetView("Plugins").IsArray() || !FrozenRoot.GetView("Modules").IsArray())
                Fail("Invalid frozen Game plugin selection");
            for (const auto& [Key,Expected]:{std::pair{"Platform",Host.Platform},{"Architecture",Host.Architecture},
                {"Configuration",Host.Configuration},{"ToolchainABI",Host.ToolchainABI}})
                if (Text(Captured,Key)!=Expected) Fail("Cook/runtime target identity differs: "+std::string(Key));
            (void)Required("Shaders/ShaderLibrary.dslb");
            FJsonDocument Authored;
            if (!Authored.Parse(Snapshot.GetRootView().GetView("ProjectDescriptor").GetString())) Fail("Invalid frozen project request");
            FProjectDescriptor Project;
            if (!Project.Load(ProjectFile,OutReason)) Fail(OutReason);
            FProjectDescriptor FrozenProject;
            if (!FrozenProject.Read(Authored, ProjectFile, OutReason) || FrozenProject.PluginDependencies != Project.PluginDependencies)
                Fail("Staged project dependencies differ from Cook");
            const auto StartupFile = Required((std::filesystem::path(Text(Root,"ProjectFile")).parent_path()/"Configs/Plugins.json").generic_string());
            const auto Startup = ReadPluginStartupSnapshot(ProjectFile);
            if (!Startup || !Captured.GetView("PluginStartupConfig").IsString() || *Startup != Captured.GetView("PluginStartupConfig").GetString())
                Fail("Staged plugin startup configuration differs from Cook");

            const auto Configs=Root.GetView("ConfigurationFiles");
            if (!Configs.IsArray() || Configs.Num()!=2) Fail("Missing staged runtime configuration");
            std::set<std::filesystem::path> Configuration;
            for (size_t I=0;I<Configs.Num();++I) Configuration.insert(Required(Configs.GetView(I).GetString()));
            const auto ConfigRoot=std::filesystem::path(ProjectFile).parent_path()/"Configs";
            if (Configuration!=std::set<std::filesystem::path>{std::filesystem::weakly_canonical(ConfigRoot/"Project.yaml"),std::filesystem::weakly_canonical(ConfigRoot/"DurinGame.yaml")})
                Fail("Staged runtime configuration bindings differ from the active project");
            for (const auto* Name:{"Project.yaml","DurinGame.yaml"})
            {
                const auto Bytes=FFileHelper::LoadFileToArray(ConfigRoot/Name);
                const auto Expected=Captured.GetView(Name);
                if (!Bytes || !Expected.IsString()
                    || std::string_view(reinterpret_cast<const char*>(Bytes->data()),Bytes->size())!=Expected.GetString())
                    Fail("Runtime configuration differs from Cook: "+std::string(Name));
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
            if (!Mounts.IsArray() || !FrozenMounts.IsArray() || Mounts.Num()!=FrozenMounts.Num()) Fail("Invalid cooked plugin mounts");
            std::set<std::string> VirtualRoots;
            for (size_t I=0;I<Mounts.Num();++I)
            {
                const auto Item=Mounts.GetView(I); const auto Virtual=Text(Item,"VirtualRoot");
                const auto FrozenMount=FrozenMounts.GetView(I);
                if (Virtual!=Text(FrozenMount,"VirtualRoot")) Fail("Cook/runtime mount bindings differ");
                if (!Virtual.starts_with("/Plugins/") || !Virtual.ends_with('/') || !VirtualRoots.insert(StringUtils::FoldAscii(Virtual)).second
                    || !PluginIds.contains(StringUtils::FoldAscii(Virtual.substr(9,Virtual.size()-10)))) Fail("Invalid cooked plugin mount identity");
                FMountPoint Mount; Mount.VirtualRoot=Virtual; Mount.Root=Base/Virtual.substr(1,Virtual.size()-2); Mount.ContentPath="."; Mount.bContentWritable=false;
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
