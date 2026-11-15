#include "ExportPipeline.h"
#include "NativeDeployment.h"
#include "Asset/Cook.h"
#include "Json/Json.h"
#include "Misc/FileHelper.h"
#include "Misc/RuntimePaths.h"
#include "Misc/StringHelper.h"
#include "Plugins/PluginResolution.h"
#include "Plugins/PluginCook.h"
#include "Project/Project.h"

namespace Durin
{
    namespace
    {
        using Path=std::filesystem::path;
        auto Require(bool Value,std::string Message) -> void { if (!Value) throw std::runtime_error(std::move(Message)); }
        auto Text(FJsonNodeView Node,std::string_view Key) -> std::string
        {
            auto V=Node.GetView(Key); Require(V.IsString() && !V.GetString().empty(),"Missing string: "+std::string(Key)); return V.GetString();
        }
        auto Read(const Path& File) -> FJsonDocument
        {
            FJsonDocument D; Require(D.LoadFromFile(File).has_value(),"Missing/invalid JSON: "+File.generic_string()); return D;
        }
        auto CopyJson(FJsonNodeRef To,FJsonNodeView From) -> void
        {
            if (From.IsObject()) { To.EnsureObject(); From.ForEachObjectMember([&](std::string_view Key,FJsonNodeView V){ CopyJson(To.AddObject(Key),V); }); }
            else if (From.IsArray()) { To.EnsureArray(); for (size_t I=0;I<From.Num();++I) CopyJson(To.AppendObject(),From.GetView(I)); }
            else if (From.IsString()) To.SetValue(From.GetString());
            else if (From.IsBool()) To.SetValue(From.GetBool());
            else if (From.IsInt()) To.SetValue(From.GetInt());
            else if (From.IsUInt()) To.SetValue(From.GetUInt());
            else To.SetValue(nullptr);
        }
        auto Save(const Path& File,std::string_view Bytes) -> void
        {
            std::filesystem::create_directories(File.parent_path());
            std::ofstream Stream(File,std::ios::binary); Stream.write(Bytes.data(),static_cast<std::streamsize>(Bytes.size()));
            Require(Stream.good(),"Cannot write: "+File.generic_string());
        }
        auto Imports(const Path& File) -> std::vector<std::string>
        {
            const auto Bytes=FFileHelper::LoadFileToArray(File); Require(Bytes.has_value(),"Cannot inspect PE: "+File.generic_string());
            const auto& B=*Bytes;
            const auto U16=[&](size_t O) { Require(O<=B.size() && B.size()-O>=2,"Truncated PE"); uint16 V; std::memcpy(&V,B.data()+O,2); return V; };
            const auto U32=[&](size_t O) { Require(O<=B.size() && B.size()-O>=4,"Truncated PE"); uint32 V; std::memcpy(&V,B.data()+O,4); return V; };
            const auto U64=[&](size_t O) { Require(O<=B.size() && B.size()-O>=8,"Truncated PE"); uint64 V; std::memcpy(&V,B.data()+O,8); return V; };
            Require(U16(0)==0x5a4d,"Not a PE binary"); const size_t PE=U32(0x3c); Require(U32(PE)==0x4550,"Invalid PE signature"); Require(U16(PE+4)==0x8664,"Only x64 PE machine staging is supported");
            const auto Count=U16(PE+6),Size=U16(PE+20); const size_t Opt=PE+24;
            const auto Magic=U16(Opt); Require(Magic==0x20b,"Only x64 PE staging is supported");
            const size_t Dirs=Opt+112,Sections=Opt+Size; Require(Size>=112+14*8,"Truncated PE directories");
            const auto Offset=[&](uint64 RVA) -> size_t {
                for (size_t I=0;I<Count;++I) { const size_t S=Sections+I*40; const uint64 Start=U32(S+12);
                    if (RVA>=Start && RVA<Start+std::max(U32(S+8),U32(S+16))) return static_cast<size_t>(U32(S+20)+RVA-Start); }
                throw std::runtime_error("Unmapped PE import RVA");
            };
            std::vector<std::string> Result;
            for (const auto& [Index,Stride]:{std::pair{1u,20u},std::pair{13u,32u}})
            {
                const auto RVA=U32(Dirs+Index*8),Length=U32(Dirs+Index*8+4); if (!RVA) continue;
                size_t Cursor=Offset(RVA); bool End=false;
                for (size_t I=0;I<std::min<size_t>(Length/Stride+1,65536);++I,Cursor+=Stride)
                {
                    bool Zero=true; for (size_t J=0;J<Stride;J+=4) Zero=Zero && U32(Cursor+J)==0;
                    if (Zero) { End=true; break; }
                    uint64 Name=U32(Cursor+(Index==1?12:4));
                    if (Index==13 && !(U32(Cursor)&1)) Name-=U64(Opt+24);
                    size_t Start=Offset(Name),Stop=Start;
                    while (Stop<B.size() && B[Stop]!=std::byte{0} && Stop-Start<4096) ++Stop;
                    Require(Stop<B.size() && Stop>Start && Stop-Start<4096,"Invalid PE library name");
                    std::string Library(reinterpret_cast<const char*>(B.data()+Start),Stop-Start);
                    Require(Library.find_first_of("/\\:")==std::string::npos,"Invalid PE library identity");
                    Result.push_back(StringUtils::FoldAscii(Library));
                }
                Require(End,"Unterminated PE imports");
            }
            return Result;
        }
        // Cook and completed native receipts determine the candidate generation.
        struct FStagingPlan
        {
            FJsonDocument Snapshot,RuntimeSnapshot,Selection,Descriptor;
            FNativeDeployment Native;
            FByteBuffer DeliveryManifest;
            std::set<std::string> NativeFiles,NativeBinaries,NativeLibraries,RequiredFiles;
            std::map<std::string,std::string> Destinations;
            std::map<std::string,Path> Files;
            std::map<std::string,std::string> Hashes;
            std::string SelectedReceiptJson,AvailabilityReceiptJson,Executable;
        };
        auto MakePlan(const FExportTarget& Target,const std::function<bool()>& Cancel,std::string& Category) -> FStagingPlan
        {
            FStagingPlan P;
            const auto Check=[&] { if (Cancel && Cancel()) throw std::runtime_error("cancelled"); };
            const auto Add=[&](const Path& Source,const std::string& Relative,std::string Expected="") {
                Check(); (void)ResolveDeploymentPath(Target.Output.parent_path(),Relative);
                Require(std::filesystem::is_regular_file(Source),"Missing required runtime file: "+Source.generic_string());
                const auto Hash=FileSha256(Source); Require(Expected.empty() || Hash==Expected,"Cook/native input changed: "+Source.generic_string());
                const auto Key=StringUtils::FoldAscii(Relative);
                if (P.Hashes.contains(Key)) Require(P.Hashes.at(Key)==Hash,"Conflicting runtime files: "+Relative);
                P.Hashes[Key]=Hash; P.Files[Relative]=std::filesystem::canonical(Source);
            };
            Category="identity";
            const auto ManifestBytes=FFileHelper::LoadFileToArray(Target.Cook/"CookManifest.bin"); FCookManifest CM;
            Require(ManifestBytes && DecodeCookManifest(*ManifestBytes,CM) && CM.TargetProfile==ECookTargetProfile::Game,"Invalid committed Game Cook manifest");
            for (const auto& Entry:CM.Entries)
            {
                const auto Source=ResolveDeploymentPath(Target.Cook,Entry.RelativePath);
                const auto Data=FFileHelper::LoadFileToArray(Source);
                Require(Data && Data->size()==Entry.FileSize && FXxHash128::HashBuffer(*Data)==FXxHash128{Entry.HashLow,Entry.HashHigh},"Cook output missing/corrupt: "+Entry.RelativePath);
                if (Entry.Kind==ECookManifestEntryKind::PluginSnapshot)
                {
                    if (Entry.RelativePath=="ExportEvidence.json") { Require(!P.Snapshot.IsValid(),"Duplicate Cook evidence"); P.Snapshot=Read(Source); }
                    else if (Entry.RelativePath=="RuntimeCook.json") { Require(!P.RuntimeSnapshot.IsValid(),"Duplicate runtime snapshot"); P.RuntimeSnapshot=Read(Source); }
                    else throw std::runtime_error("Unsupported Cook snapshot; recook with the current tool");
                }
            }
            Require(P.Snapshot.IsValid() && P.RuntimeSnapshot.IsValid(),"Runtime snapshot/evidence missing; recook before staging");
            std::string Projected,ProjectionError;
            Require(MakeRuntimeCookSnapshot(P.Snapshot.ToString(),Projected,ProjectionError),ProjectionError);
            FJsonDocument Projection; Require(Projection.Parse(Projected).has_value(),"Invalid runtime projection");
            Require(Projection.ToString(EJsonFormat::Compact)==P.RuntimeSnapshot.ToString(EJsonFormat::Compact),"Cook runtime/evidence drift; recook"); const auto S=P.Snapshot.GetRootView();
            Require(S.GetView("SchemaVersion").GetInt()==1,"Unsupported Cook snapshot; recook");
            Require(Text(S,"Platform")=="Win64" && Target.Platform=="Win64" && CM.TargetPlatform==ECookTargetPlatform::Win64,"Unsupported/mismatched Cook platform");
            Require(Text(S,"Configuration")==Target.Configuration,"Cook/Game configuration differs");
            Require(P.Selection.Parse(Text(S,"Selection")).has_value() && P.Descriptor.Parse(Text(S,"ProjectDescriptor")).has_value(),"Invalid frozen Cook selection");
            Require(Target.ProjectDescriptorJson==P.Descriptor.ToString(EJsonFormat::Compact),"Project changed since Cook; recook");
            const auto Startup=ReadPluginStartupSnapshot(Target.Project);
            Require(Startup.has_value(),Startup?"":Startup.error());
            Require(Text(S,"PluginStartupConfig")== (Startup->empty()?std::string("{}"):*Startup),
                "Plugin startup configuration differs from Cook; recook");
            FPluginResolveRequest Resolve; Resolve.EngineRoot=Target.Engine; Resolve.ProjectFile=Target.Project; Resolve.Platform="Win64"; Resolve.RuntimeVariant="DurinGame";
            const auto WorkspaceRoot=Target.Engine.parent_path();
            auto Workspace=Read(WorkspaceRoot/"Durin.dworkspace"); const auto Projects=Workspace.GetRootView().GetView("Projects");
            for (size_t I=0;I<Projects.Num();++I) {
                const auto File=ResolveDeploymentPath(WorkspaceRoot,Projects.GetView(I).GetString());
                Resolve.ProjectFiles.push_back(Text(Read(File).GetRootView(),"ProjectName")==Target.ProjectName?Target.Project:File);
            }
            auto Current=ResolvePlugins(Resolve); Require(Current.has_value(),Current?"":Current.error());
            FJsonDocument CurrentJson; Require(CurrentJson.Parse(Current->ManifestJson).has_value(),"Invalid resolved selection");
            Require(CurrentJson.ToString(EJsonFormat::Compact)==P.Selection.ToString(EJsonFormat::Compact),"Selection changed since Cook; recook");
            const auto Receipt=ReadNativeReceipt(Target.Receipt,{Target.Engine,Target.Project,Target.Artifacts,Target.Dependencies,Target.Prebuilt,Target.ProjectProducts},P.RuntimeSnapshot.GetRootView(),P.Selection.GetRootView());
            P.Native=SelectNativeDeployment(Receipt,P.RuntimeSnapshot.GetRootView(),*Current);
            ValidateNativeDeployment(P.Native,P.RuntimeSnapshot.GetRootView());
            P.AvailabilityReceiptJson=Receipt.Bytes;
            P.SelectedReceiptJson=P.Native.SelectedReceipt.ToString(EJsonFormat::Compact);
            for(const auto& Product:P.Native.Products)
            {
                if(Product.Kind=="NativeLibrary" || Product.Kind=="Executable") P.NativeBinaries.insert(Product.Destination);
                if(Product.Kind=="NativeLibrary") P.NativeLibraries.insert(Product.Destination);
                if(Product.Kind=="Executable") P.Executable=Product.Destination;
                Add(Product.Source,Product.Destination,Product.Sha256);
            }
            auto System=Read(Target.Policy); std::set<std::string> SystemNames;
            for (const auto* Key:{"SystemLibraries","ApiSetContracts",Text(S,"Configuration")=="Debug"?"DebugCRTLibraries":"ReleaseCRTLibraries"})
            { const auto List=System.GetRootView().GetView(Key); for (size_t I=0;I<List.Num();++I) SystemNames.insert(List.GetView(I).GetString()); }
            const auto External=System.GetRootView().GetView("ExternalPrerequisites");
            for (size_t I=0;I<External.Num();++I) SystemNames.insert(Text(External.GetView(I),"Library"));
            std::map<std::string,std::string> NativeNames; std::vector<std::string> Queue;
            for (const auto& Relative:P.NativeBinaries)
            { const auto Name=StringUtils::FoldAscii(Path(Relative).filename().generic_string()); Require(NativeNames.emplace(Name,Relative).second,"Duplicate native loader identity: "+Name); Queue.push_back(Relative); }
            for (size_t I=0;I<Queue.size();++I)
            {
                Check(); for (const auto& Name:Imports(P.Files.at(Queue[I])))
                {
                    if (NativeNames.contains(Name)) continue;
                    Require(SystemNames.contains(Name),"Missing declared native dependency: "+Name+" imported by "+Queue[I]);
                }
            }
            for (const auto& [Relative,Source]:P.Files) P.NativeFiles.insert(Relative);
            for (const auto& Entry:CM.Entries) Add(ResolveDeploymentPath(Target.Cook,Entry.RelativePath),Entry.RelativePath);
            Add(Target.Cook/"CookManifest.bin","CookManifest.bin");
            std::erase_if(CM.Entries,[](const auto& Entry){return Entry.RelativePath=="ExportEvidence.json";});
            Require(static_cast<bool>(EncodeCookManifest(CM,P.DeliveryManifest)),"Cannot project runtime Cook manifest");
            Require(P.Files.contains("Shaders/ShaderLibrary.dslb"),"Cooked Shader library missing; recook");
            P.RequiredFiles={"Content/CookManifest.bin","Content/RuntimeCook.json","Content/Shaders/ShaderLibrary.dslb","Config/PrerequisitePolicy.json","Config/Defaults/DurinGame.yaml","Config/Project.yaml","Config/DurinGame.yaml"};
            P.RequiredFiles.insert(P.NativeFiles.begin(),P.NativeFiles.end());
            std::set<std::string> DeliveryPaths{"runtime.json","rungame.bat","content/cookmanifest.bin","config/prerequisitepolicy.json","config/defaults/duringame.yaml","config/project.yaml","config/duringame.yaml"};
            for(const auto& [Relative,Source]:P.Files)
                if(Relative!="ExportEvidence.json" && Relative!="CookManifest.bin")
                {
                    const auto Destination=P.NativeFiles.contains(Relative)?Relative:"Content/"+Relative;
                    Require(DeliveryPaths.insert(StringUtils::FoldAscii(Destination)).second,"Colliding delivery destination: "+Destination);
                    P.Destinations.emplace(Relative,Destination);
                }
            return P;
        }
    }

    void StageGame(const FExportTarget& Target,const std::function<bool()>& IsCancelled,
        const FStageProgress& Progress,std::string& Category,std::string& Fingerprint)
    {
        auto Plan=MakePlan(Target,IsCancelled,Category); Category="stage";
        const auto Snapshot=Plan.Snapshot.GetRootView();
        // Content fingerprint is independent of output/correlation paths.
        std::string DigestInput; for (const auto& [Relative,Source]:Plan.Files) DigestInput+=Relative+":"+Plan.Hashes.at(StringUtils::FoldAscii(Relative))+"\n";
        // Bind the selected input bytes in the completed product fingerprint.
        DigestInput+="NativeReceipt:"+BytesSha256(std::as_bytes(std::span(Plan.SelectedReceiptJson.data(),Plan.SelectedReceiptJson.size())))+"\n";
        DigestInput+="PrerequisitePolicy:"+FileSha256(Target.Policy)+"\n";
        Fingerprint=BytesSha256(std::as_bytes(std::span(DigestInput.data(),DigestInput.size())));
        const auto Check=[&] { if (IsCancelled && IsCancelled()) throw std::runtime_error("cancelled"); };
        Check();
        std::filesystem::create_directories(Target.Output.parent_path());
        Require(std::filesystem::create_directory(Target.Output),"Stage destination already exists; choose a fresh directory");
        const auto Runtime=Target.Output;
        const auto Name=Text(Plan.Descriptor.GetRootView(),"ProjectName");

        uint64 Copied=0;
        for (const auto& [Relative,Source]:Plan.Files)
        {
            Progress("stage",Relative,Copied++,static_cast<uint64>(Plan.Files.size()));
            Check();
            Require(FileSha256(Source)==Plan.Hashes.at(StringUtils::FoldAscii(Relative)),"Input changed during Stage: "+Relative);
            if (!Plan.Destinations.contains(Relative)) continue;
            const auto Target=ResolveDeploymentPath(Runtime,Plan.Destinations.at(Relative)); std::filesystem::create_directories(Target.parent_path());
            std::filesystem::copy_file(Source,Target); Require(FileSha256(Target)==Plan.Hashes.at(StringUtils::FoldAscii(Relative)),"Copied file differs: "+Relative);
        }
        Save(Runtime/"Content/CookManifest.bin",std::string_view(reinterpret_cast<const char*>(Plan.DeliveryManifest.data()),Plan.DeliveryManifest.size()));
        Save(Runtime/"Config/Defaults/DurinGame.yaml",Text(Snapshot,"EngineGameDefaults"));
        Save(Runtime/"Config/Project.yaml",Text(Snapshot,"Project.yaml")); Save(Runtime/"Config/DurinGame.yaml",Text(Snapshot,"DurinGame.yaml"));
        FJsonDocument Manifest; auto M=Manifest.GetMutableRoot(); M.EnsureObject(); M.SetChildValue("SchemaVersion",2);
        auto Identity=M.AddObject("Identity"); Identity.SetChildValue("EngineBuild",Plan.Native.EngineBuild); Identity.SetChildValue("RuntimeVariant","DurinGame");
        for (const auto* Key:{"Platform","Configuration","ToolchainABI"}) Identity.SetChildValue(Key,Text(Snapshot,Key));
        Identity.SetChildValue("Architecture","x64"); Identity.SetChildValue("Target","DurinLauncher"); Identity.SetChildValue("Project",Name);

        M.SetChildValue("ApplicationId",Text(Plan.RuntimeSnapshot.GetRootView(),"ApplicationId")); M.SetChildValue("Executable",Plan.Executable);
        auto Paths=M.AddObject("Paths"); Paths.SetChildValue("Engine","."); Paths.SetChildValue("Project",".");
        Paths.SetChildValue("EngineContent","Content/Engine"); Paths.SetChildValue("ProjectContent","Content/Game"); Paths.SetChildValue("Binaries","Binaries/Win64"); Paths.SetChildValue("ThirdParty","Binaries/Win64"); Paths.SetChildValue("Config","Config"); Paths.SetChildValue("Cook","Content");
        auto Project=M.AddObject("Project"); Project.SetChildValue("Name",Name); auto Roots=Project.AddArray("BaseModules");
        const auto Base=Plan.Descriptor.GetRootView().GetView("BaseModules");
        if (Base.IsArray()) CopyJson(Roots,Base); else Plan.Descriptor.GetRootView().GetView("ModuleDirs").ForEachObjectMember([&](std::string_view K,FJsonNodeView){Roots.AppendValue(K);});
        auto Extra=Plan.Descriptor.GetRootView().GetView("ExtraModules").GetView("DurinGame"); if (Extra.IsObject()) Extra=Extra.GetView("Modules"); for (size_t I=0;I<Extra.Num();++I) Roots.AppendValue(Extra.GetView(I).GetString());
        auto User=M.AddObject("UserData"); User.SetChildValue("Policy","LocalAppData"); User.SetChildValue("Namespace","Durin");
        CopyJson(M.AddObject("Prerequisites"),Plan.Native.SelectedReceipt.GetRootView().GetView("Prerequisites"));
        const auto HashEntry=[&](FJsonNodeRef Item,const std::string& Relative) { Item.SetChildValue("Path",Relative); Item.SetChildValue("Sha256",FileSha256(Runtime/Relative)); };
        auto Cook=M.AddObject("Cook"); HashEntry(Cook.AddObject("Manifest"),"Content/CookManifest.bin"); HashEntry(Cook.AddObject("Snapshot"),"Content/RuntimeCook.json"); HashEntry(Cook.AddObject("ShaderLibrary"),"Content/Shaders/ShaderLibrary.dslb");
        Cook.SetChildValue("SelectionFingerprint",Text(Plan.RuntimeSnapshot.GetRootView(),"SelectionFingerprint"));
        // Full source/backend/native provenance is an external immutable generation.
        const auto Evidence=Path(Target.Output.generic_string()+".evidence");
        Require(std::filesystem::create_directory(Evidence),"Stage evidence directory already exists");
        Save(Evidence/"TargetReceipt.json",Plan.SelectedReceiptJson); M.SetChildValue("NativeReceiptSha256",FileSha256(Evidence/"TargetReceipt.json"));
        Save(Evidence/"AvailabilityReceipt.json",Plan.AvailabilityReceiptJson);
        Save(Evidence/"ExportEvidence.json",Plan.Snapshot.ToString()); Save(Evidence/"Backend.json",Target.BackendJson);
        FJsonDocument Sources; auto Inputs=Sources.GetMutableRoot(); Inputs.EnsureArray();
        for (const auto& [Relative,Source]:Plan.Files) {auto I=Inputs.AppendObject(); I.SetChildValue("Source",Source.generic_string()); I.SetChildValue("Logical",Relative); I.SetChildValue("Sha256",Plan.Hashes.at(StringUtils::FoldAscii(Relative)));}
        Save(Evidence/"Sources.json",Sources.ToString());
        std::filesystem::copy_file(Target.Cook/"CookManifest.bin",Evidence/"CookManifest.bin");
        std::filesystem::create_directories(Runtime/"Config"); std::filesystem::copy_file(Target.Policy,Runtime/"Config/PrerequisitePolicy.json");
        auto NativeLibraries=M.AddArray("NativeLibraries");
        for(const auto& Relative:Plan.NativeLibraries) NativeLibraries.AppendValue(Relative);
        auto Plugins=M.AddArray("Plugins"); const auto Frozen=Plan.Selection.GetRootView(); const auto Selected=Frozen.GetView("Plugins"); for (size_t I=0;I<Selected.Num();++I) Plugins.AppendValue(Text(Selected.GetView(I),"Id"));
        auto Binaries=M.AddArray("Binaries");
        for (const auto& Binary:Plan.Native.Plugins) { auto Item=Binaries.AppendObject(); Item.SetChildValue("Module",Binary.Module.Name); Item.SetChildValue("Owner",Binary.Module.Owner); Item.SetChildValue("Phase",Binary.Module.Phase); Item.SetChildValue("Path",Binary.Binary); auto Deps=Item.AddArray("RuntimeDependencies"); for (const auto& File:Binary.Dependencies) Deps.AppendValue(File); }
        auto Mounts=M.AddArray("Mounts");
        const auto Mount=[&](std::string_view Virtual,std::string_view Relative) { auto Item=Mounts.AppendObject(); Item.SetChildValue("VirtualRoot",Virtual); Item.SetChildValue("Path","Content/"+std::string(Relative)); return Item.AddArray("CanReference"); };
        Mount("/Engine/","Engine"); Mount("/Game/","Game").AppendValue("/Engine/"); const auto PluginMounts=Snapshot.GetView("Mounts");
        for (size_t I=0;I<PluginMounts.Num();++I) { const auto V=Text(PluginMounts.GetView(I),"VirtualRoot"); auto Refs=Mount(V,V.substr(1,V.size()-2)); CopyJson(Refs,PluginMounts.GetView(I).GetView("CanReference")); }
        auto Config=M.AddObject("ConfigurationFiles"); Config.SetChildValue("Project","Config/Project.yaml"); Config.SetChildValue("Application","Config/DurinGame.yaml"); Config.SetChildValue("Defaults","Config/Defaults/DurinGame.yaml");
        auto Files=M.AddArray("Files");
        // CookManifest alone catalogs assets; runtime Files binds native/config/snapshot/Shader inputs.
        for (const auto& Relative:Plan.RequiredFiles) HashEntry(Files.AppendObject(),Relative);
        Save(Runtime/"Runtime.json",Manifest.ToString());
        Save(Runtime/"RunGame.bat","@echo off\r\n\"%~dp0"+Plan.Executable+"\" --runtime-manifest=\"%~dp0Runtime.json\" %*\r\n");
        auto Valid=ReadRuntimePathContext(Runtime/"Runtime.json",Runtime/Plan.Executable,false); Require(Valid.has_value(),Valid?"":Valid.error());
        FPluginResolution AdmittedSelection; std::vector<FAdmittedPluginBinary> Admitted; FPluginResources Resources; std::string Reason;
        const FPluginBuildIdentity TargetIdentity{Plan.Native.EngineBuild,"Win64","x64",Text(Snapshot,"Configuration"),"DurinGame",Text(Snapshot,"ToolchainABI")};
        const auto AdmittedOk=ReadPluginRuntimeInventory(Runtime/"Runtime.json","",TargetIdentity,AdmittedSelection,Admitted,Resources,Reason);
        Require(AdmittedOk,"Delivery admission failed: "+Reason);
        Check();
    }
}
