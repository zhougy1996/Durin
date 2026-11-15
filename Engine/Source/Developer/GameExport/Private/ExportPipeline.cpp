#include "GameExport.h"
#include "ExportPipeline.h"
#include "ExportComponent.h"
#include "HAL/PlatformProcess.h"
#include "Json/Json.h"
#include "Misc/FileHelper.h"
#include "Misc/RuntimePaths.h"

namespace Durin
{
    namespace
    {
        using Path = std::filesystem::path;
        void Need(bool OK, std::string Message) { if(!OK) throw std::runtime_error(std::move(Message)); }
        auto Text(FJsonNodeView N, std::string_view K) -> std::string { auto V=N.GetView(K); Need(V.IsString() && !V.GetString().empty(),"Missing field: "+std::string(K)); return V.GetString(); }
        auto Read(const Path& P) -> FJsonDocument { FJsonDocument D; Need(D.LoadFromFile(P).has_value(),"Missing/invalid metadata: "+P.generic_string()); return D; }
        auto Absolute(std::string_view S) -> Path { auto P=std::filesystem::u8path(S); Need(P.is_absolute(),"Expected absolute path: "+std::string(S)); return std::filesystem::weakly_canonical(P); }
        void Save(const Path& P, std::string_view S) { auto B=std::as_bytes(std::span(S.data(),S.size())); const auto R=FFileHelper::SaveArrayToFileAtomically(B,P); Need(R.has_value(),"Cannot write: "+P.generic_string()); }
        auto Hash(std::string_view S) -> std::string { return BytesSha256(std::as_bytes(std::span(S.data(),S.size()))); }
        auto Resolve(const Path& Metadata,std::string_view S) -> Path { const auto P=std::filesystem::u8path(S); return P.is_absolute()?Absolute(S):ResolveDeploymentPath(Metadata.parent_path(),S); }
    }
    auto RunGameExportWorker(const Path& Worker,std::string_view Request,const std::function<bool()>& Cancel,
        const std::function<void(std::string_view)>& OnEvent) -> int
    {
        (void)ReadExportComponent(Worker.parent_path()/"ExportComponent.json");
        FJsonDocument D; Need(D.Parse(Request).has_value(),"Invalid worker request");
        const auto Id=Text(D.GetRootView(),"RequestId");
        std::string Pending; int Result=-1; uint64 Next=0;
        FProcessExecutionControl Control;
        Control.StandardInput=std::string(Request)+"\n";
        Control.CancellationInput="{\"ProtocolVersion\":1,\"Kind\":\"cancel\",\"RequestId\":\""+Id+"\"}\n";
        Control.IsCancelled=Cancel;
        Control.OnOutput=[&](std::string_view Chunk) {
            Pending.append(Chunk);
            size_t End;
            while((End=Pending.find('\n'))!=std::string::npos) {
                auto Line=Pending.substr(0,End); Pending.erase(0,End+1);
                FJsonDocument Event;
                if(!Event.Parse(Line) || !Event.GetRootView().GetView("Kind").IsString()) { std::cerr<<Line<<'\n'; continue; }
                const auto E=Event.GetRootView();
                Need(E.GetView("ProtocolVersion").GetInt()==1 && Text(E,"RequestId")==Id
                    && E.GetView("Sequence").GetUInt()==Next++ && Result<0,"Invalid worker event correlation/sequence");
                if(Text(E,"Kind")=="result") { Need(E.GetView("ChildrenRetired").GetBool(),"Worker children not retired"); Result=static_cast<int>(E.GetView("ExitCode").GetInt()); }
                if(OnEvent) OnEvent(Line);
            }
            Need(Pending.size()<=1024*1024,"Worker frame exceeds 1 MiB");
        };
        const auto Code=FPlatformProcess::ExecuteControlledProcess(Worker.generic_string(),{{"--protocol=1"}},Control);
        Need(Code.has_value(),Code?"":Code.error().ToString());
        Need(Result>=0 && Result==*Code,"Worker exited without a matching terminal result"); return Result;
    }
    auto ExecuteGameExportRequest(std::string_view Json,const std::function<bool()>& Cancel,
        const std::function<void(std::string_view)>& Emit) -> int
    {
        FJsonDocument Request;
        std::string Id="InvalidRequest",Category="request",Fingerprint; uint64 Sequence=0;
        const auto Send=[&](std::string_view Kind,const std::function<void(FJsonNodeRef)>& Fill) {
            FJsonDocument D; auto E=D.GetMutableRoot(); E.EnsureObject(); E.SetChildValue("ProtocolVersion",1); E.SetChildValue("RequestId",Id); E.SetChildValue("Sequence",Sequence++); E.SetChildValue("Kind",Kind); Fill(E); Emit(D.ToString(EJsonFormat::Compact));
        };
        const auto Result=[&](int Code,std::string_view Message,const Path& Output=Path{}) {
            Send("result",[&](FJsonNodeRef E) { E.SetChildValue("Status",Code==0?"succeeded":Code==130?"cancelled":"failed"); E.SetChildValue("ExitCode",Code);
                E.SetChildValue("Category",Code==0?"none":Code==130?"cancelled":Category); E.SetChildValue("Code",Code==0?"completed":Code==130?"cancelled":"export-failed"); E.SetChildValue("Message",Message); E.SetChildValue("ChildrenRetired",true);
                if(Output.empty()) E.SetChildValue("Output",nullptr); else E.SetChildValue("Output",Output.generic_string()); if(Fingerprint.empty()) E.SetChildValue("PlanFingerprint",nullptr); else E.SetChildValue("PlanFingerprint",Fingerprint); }); return Code;
        };
        const auto Check=[&] { if(Cancel && Cancel()) throw std::runtime_error("cancelled"); };
        try {
            Category="protocol";
            Need(Json.size()<=1024*1024 && Request.Parse(Json).has_value(),"Malformed/oversized export request");
            const auto V=Request.GetRootView();
            const auto RequestedId=Text(V,"RequestId"); const auto Letter=[](char C){return (C>='A' && C<='Z') || (C>='a' && C<='z');};
            Need(RequestedId.size()<=128 && Letter(RequestedId.front()) && std::ranges::all_of(RequestedId,[&](char C){return Letter(C) || (C>='0' && C<='9') || C=='_';}),"Invalid request ID"); Id=RequestedId;
            Need(V.GetView("ProtocolVersion").IsInt() && V.GetView("ProtocolVersion").GetInt()==1 && Text(V,"Kind")=="request","Unsupported request protocol");
            const std::set<std::string> Fields{"ProtocolVersion","Kind","RequestId","Operation","ProjectFile","Platform","Configuration","Output","EngineRoot","Backend","BackendMetadata","Build","Cook","Verify","Archive","CookDirectory","Receipt","Preview","Preset"};
            std::set<std::string> Seen; V.ForEachObjectMember([&](std::string_view K,FJsonNodeView){Need(Fields.contains(std::string(K)) && Seen.insert(std::string(K)).second,"Unknown/duplicate export field");});
            Need(V.GetView("Verify").IsBool() && V.GetView("Archive").IsBool() && (!V.GetView("Preview").IsValid() || V.GetView("Preview").IsBool()),"Verify/Archive/Preview must be boolean");
            Need(!V.GetView("Preset").IsValid() || (V.GetView("Preset").IsString() && !V.GetView("Preset").GetString().empty()),"Preset must be a non-empty string");
            Category="capability";
            const auto Operation=Text(V,"Operation");
            Need(Operation=="export" || Operation=="stage" || Operation=="preview","Unsupported export operation");
            const bool Export=Operation=="export";
            Need(Export || (!V.GetView("Verify").GetBool() && !V.GetView("Archive").GetBool()
                && Text(V,"Build")=="reuse" && Text(V,"Cook")=="reuse"),"Stage only supports reuse without verification/archive");
            Category="request";
            const auto Project=Absolute(Text(V,"ProjectFile")), Output=Absolute(Text(V,"Output")), Engine=Absolute(Text(V,"EngineRoot"));
            Need(Text(V,"Platform")=="Win64" && (Text(V,"Configuration")=="Debug" || Text(V,"Configuration")=="Release"),"Unsupported export target");
            Need(Text(V,"Build")=="auto" || Text(V,"Build")=="reuse","Invalid Build policy"); Need(Text(V,"Cook")=="auto" || Text(V,"Cook")=="reuse","Invalid Cook policy");
            const auto Overlaps=[](const Path& A,const Path& B) {const auto X=A.lexically_relative(B); return !X.empty() && *X.begin()!="..";};
            for(const auto& Input:{Project.parent_path(),Engine}) Need(!Overlaps(Output,Input) && !Overlaps(Input,Output),"Export output overlaps input");
            Category="capability";
            const auto Metadata=Absolute(Text(V,"BackendMetadata")); auto Backend=Read(Metadata); const auto B=Backend.GetRootView();
            Need(B.GetView("Kind").GetString()!="prebuilt-stage" || B.GetView("Component").IsString(),"Prebuilt backend requires an export component");
            if(B.GetView("Component").IsString()) (void)ReadExportComponent(Resolve(Metadata,Text(B,"Component")));
            Need(B.GetView("SchemaVersion").GetInt()==1 && (Text(B,"Kind")=="source-stage" || Text(B,"Kind")=="prebuilt-stage"),"Unsupported export backend metadata");
            auto Game=B; const auto Games=B.GetView("Games"); auto Descriptor=Read(Project); const auto ProjectName=Text(Descriptor.GetRootView(),"ProjectName");
            if(Games.IsArray()) {
                size_t Matches=0;
                for(size_t I=0;I<Games.Num();++I) { auto G=Games.GetView(I); if(Text(G,"Configuration")==Text(V,"Configuration") && G.GetView("Project").GetString(ProjectName)==ProjectName && G.GetView("Platform").GetString(Text(V,"Platform"))==Text(V,"Platform") && (V.GetView("Preset").GetString().empty() || Text(G,"Name")==V.GetView("Preset").GetString()) && (B.GetView("SelectedPreset").GetString().empty() || Text(G,"Name")==B.GetView("SelectedPreset").GetString())) {Game=G; ++Matches;} }
                Need(Matches==1,"Backend requires one matching Game product set");
            }
            const auto Receipt=Resolve(Metadata,Text(Game,"TargetReceipt"));
            if(V.GetView("Receipt").IsValid()) Need(std::filesystem::equivalent(Absolute(Text(V,"Receipt")),Receipt),"Requested receipt conflicts with backend selection");
            Path Cook=V.GetView("CookDirectory").GetString().empty()?(B.GetView("CookDirectory").IsString()?Resolve(Metadata,Text(B,"CookDirectory")):Resolve(Metadata,Text(Game,"ArtifactsRoot"))/"ExportCook"/ProjectName):Absolute(Text(V,"CookDirectory"));
            Need(!Overlaps(Output,Cook) && !Overlaps(Cook,Output),"Export output overlaps Cook input");
            for(const auto& K:{"ArtifactsRoot","DependenciesRoot"}) {const auto Input=Resolve(Metadata,Text(Game,K)); Need(!Overlaps(Output,Input) && !Overlaps(Input,Output),"Export output overlaps backend input");}
            FExportTarget Target;
            Target.Engine=Engine; Target.Project=Project; Target.Cook=Cook; Target.Output=Export?Output/"Delivery":Output;
            Target.Receipt=Receipt; Target.Artifacts=Resolve(Metadata,Text(Game,"ArtifactsRoot")); Target.Dependencies=Resolve(Metadata,Text(Game,"DependenciesRoot"));
            Target.ProjectName=ProjectName; Target.Configuration=Text(V,"Configuration"); Target.Platform=Text(V,"Platform");
            Target.BackendJson=Backend.ToString(); Target.ProjectDescriptorJson=Descriptor.ToString(EJsonFormat::Compact);
            Target.Policy=B.GetView("PolicyFile").IsString()?Resolve(Metadata,Text(B,"PolicyFile")):Engine/"Source/Developer/GameExport/Contracts/win64-system-v1.json";
            if(B.GetView("ProjectProductsRoot").IsString()) Target.ProjectProducts=ResolveDeploymentPath(Metadata.parent_path(),Text(B,"ProjectProductsRoot"));
            Fingerprint=Hash(ProjectName+":"+Text(V,"Configuration")+":"+Text(V,"Build")+":"+Text(V,"Cook")+":"+FileSha256(Metadata));
            Category="capability"; const auto BackendKind=Text(V,"Backend"); Need(BackendKind=="source" || BackendKind=="prebuilt","Unsupported backend");
            Need((BackendKind=="prebuilt")== (Text(B,"Kind")=="prebuilt-stage"),"Backend capability/metadata mismatch");
            Need(BackendKind!="prebuilt" || Text(V,"Build")=="reuse","Prebuilt backend cannot build native products; select reuse");
            if(BackendKind=="prebuilt") {
                Need(Text(V,"Cook")=="reuse","This prebuilt component requires compatible committed Cook; offline Cook capability is not declared");
            }
            Target.Prebuilt=BackendKind=="prebuilt";
            const bool Preview=Operation=="preview" || V.GetView("Preview").GetBool(false), Verify=V.GetView("Verify").GetBool(), Archive=V.GetView("Archive").GetBool();
            const auto Progress=[&](std::string_view Phase,std::string_view Message) {Send("progress",[&](FJsonNodeRef E){E.SetChildValue("Phase",Phase); E.SetChildValue("Completed",0); E.SetChildValue("Total",0); E.SetChildValue("Message",Message);});};
            const auto Run=[&](std::string_view Phase,const Path& Exe,const std::vector<std::string>& Args,const Path& Cwd) {
                Check(); Category=std::string(Phase); Progress(Phase,Exe.generic_string());
                std::ofstream Log(Path(Output.generic_string()+"."+std::string(Phase)+".log"),std::ios::binary|std::ios::app);
                FProcessExecutionControl Control; Control.IsCancelled=Cancel; Control.OnOutput=[&](std::string_view S){Log.write(S.data(),static_cast<std::streamsize>(S.size()));};
                FProcessLaunchOptions Options{Args,Cwd};
                if(B.GetView("Environment").IsObject()) B.GetView("Environment").ForEachObjectMember([&](std::string_view K,FJsonNodeView Value){Options.EnvironmentOverrides[std::string(K)]=Value.GetString();});
                const auto Code=FPlatformProcess::ExecuteControlledProcess(Exe.generic_string(),Options,Control);
                Check(); Need(Code.has_value(),Code?"":Code.error().ToString()); Need(*Code==0,std::string(Phase)+" child failed; see "+Output.generic_string()+"."+std::string(Phase)+".log");
            };
            if(Text(V,"Build")=="auto" || Archive) Need(std::filesystem::is_regular_file(Resolve(Metadata,Text(B,"CMake"))),"Missing CMake backend executable");
            if(Text(V,"Build")=="auto") Need(std::filesystem::is_directory(Resolve(Metadata,Text(B,"WorkingDirectory"))),"Missing source build directory");
            if(Text(V,"Cook")=="auto") Need(std::filesystem::is_regular_file(Resolve(Metadata,Text(B,"AssetTool"))),"Missing compatible Cook host component");
            Category="request"; Check(); Need(!std::filesystem::exists(Output),"Export destination already exists; choose a fresh invocation directory");
            if(Preview) {
                Fingerprint.clear();
                Progress("validate","Configuration checked; product freshness and closure are checked during execution");
                Progress("build",Text(V,"Build")); Progress("cook",Text(V,"Cook")); Progress("stage","Copy declared runtime closure to a fresh directory");
                if(Verify) Progress("verify","Run bounded Game startup");
                if(Archive) Progress("archive","Create Game.zip");
                return Result(0,"Validated export configuration; no phases executed");
            }
            if(Export) { std::filesystem::create_directories(Output.parent_path()); Need(std::filesystem::create_directory(Output),"Another writer owns the invocation directory"); }
            if(Text(V,"Build")=="auto") {
                const auto CMake=Resolve(Metadata,Text(B,"CMake")), Cwd=Resolve(Metadata,Text(B,"WorkingDirectory"));
                if(!std::filesystem::exists(Resolve(Metadata,Text(Game,"ArtifactsRoot"))/"CMakeCache.txt")) Run("build",CMake,{"--preset",Text(Game,"Name")},Cwd);
                Run("build",CMake,{"--build",Resolve(Metadata,Text(Game,"ArtifactsRoot")).generic_string(),"--target",ProjectName+"GameReceipt"},Cwd);
            }
            Category="identity"; Need(std::filesystem::is_regular_file(Receipt),"Missing completed Game receipt; build compatible products");
            if(Text(V,"Cook")=="auto") {
                Run("cook",Resolve(Metadata,Text(B,"AssetTool")),{"cook","--project="+Project.generic_string(),"--output="+Cook.generic_string(),"--target=win64","--profile=game","--target-receipt="+Receipt.generic_string(),"--json"},Engine.parent_path());
            }
            Check(); Category="stage";
            StageGame(Target,Cancel,[&](std::string_view Phase,std::string_view Message,uint64 Completed,uint64 Total) {
                Send("progress",[&](FJsonNodeRef E) { E.SetChildValue("Phase",Phase); E.SetChildValue("Completed",Completed); E.SetChildValue("Total",Total); E.SetChildValue("Message",Message); });
            },Category,Fingerprint);
            const auto Delivery=Export?Output/"Delivery":Output;
            if(Verify) {
                auto Runtime=Read(Delivery/"Runtime.json"); const auto Exe=ResolveDeploymentPath(Delivery,Text(Runtime.GetRootView(),"Executable"));
                Run("verify",Exe,{"--user-data-root="+Path(Output.generic_string()+".verify").generic_string(),"--exit-after-ticks=30"},Output);
            }
            std::string ArchiveHash;
            if(Archive) {
                const auto CMake=Resolve(Metadata,Text(B,"CMake"));
                Run("archive",CMake,{"-E","tar","cf",(Output/"Game.zip").generic_string(),"--format=zip","--","Delivery"},Output);
                ArchiveHash=FileSha256(Output/"Game.zip");
            }
            Check(); Category="publication"; Progress("publish","Writing completion marker"); Check();
            FJsonDocument Complete; auto C=Complete.GetMutableRoot(); C.EnsureObject(); C.SetChildValue("SchemaVersion",1); C.SetChildValue("RuntimeManifestSha256",FileSha256(Delivery/"Runtime.json"));
            if(!ArchiveHash.empty()) C.SetChildValue("ArchiveSha256",ArchiveHash);
            Save(Output/"ExportComplete.json",Complete.ToString(EJsonFormat::Compact));
            return Result(0,"Completed export: "+Output.generic_string(),Output);
        } catch(const std::invalid_argument& E) { Category="protocol"; return Result(2,E.what()); }
        catch(const std::exception& E) {
            if(std::string_view(E.what())=="cancelled") {Send("cancelAcknowledged",[](FJsonNodeRef E){E.SetChildValue("ChildrenRetired",true);}); return Result(130,"Export cancelled; incomplete directory retained");}
            const int Code=Category=="request" || Category=="protocol"?2:Category=="capability"?3:Category=="identity"?5:Category=="build"?6:Category=="cook"?7:Category=="verify"?9:Category=="archive"?10:Category=="publication"?11:8;
            return Result(Code,E.what());
        }
    }
}
