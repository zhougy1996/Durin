#include "GameExport.h"
#include "Json/Json.h"
#include "HAL/PlatformProcess.h"
#include "Misc/RuntimePaths.h"
#include <csignal>
#if defined(_WIN32)
#include "Windows/WindowsPlatform.h"
#else
#include <poll.h>
#include <unistd.h>
#endif

namespace
{
    volatile std::sig_atomic_t Cancelled=0;
    void Cancel(int) { Cancelled=1; }
    auto ReadFrame(std::istream& Input) -> std::string
    {
        std::string Line; char C;
        (void)Input;
        for (;;)
        {
#if defined(_WIN32)
            DWORD Count=0; if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE),&C,1,&Count,nullptr) || !Count) break;
#else
            if (::read(STDIN_FILENO,&C,1)!=1) break;
#endif
            if (C=='\n') return Line;
            Line+=C; if (Line.size()>1024*1024) break;
        }
        return Line;
    }
}
int main(int ArgC,char** ArgV)
{
    if (ArgC==2 && std::string_view(ArgV[1])=="--help")
    { std::cout<<"DurinExportTool preview|stage|export --project PATH --platform Win64 --configuration Debug|Release --output PATH --engine-root PATH --backend-metadata PATH --cook-dir PATH\nDurinExportTool --protocol=1 (JSON-line request/cancel on stdin; events on stdout)\n"; return 0; }
    std::signal(SIGINT,Cancel); std::signal(SIGTERM,Cancel);
    bool Protocol=ArgC==2 && std::string_view(ArgV[1])=="--protocol=1";
    Durin::FJsonDocument Request;
    std::string Json;
    if (Protocol) Json=ReadFrame(std::cin);
    else
    {
        bool ExplicitBuild=false, ExplicitCook=false, ExplicitBackend=false;
        auto R=Request.GetMutableRoot(); R.EnsureObject(); R.SetChildValue("ProtocolVersion",1); R.SetChildValue("Kind","request"); R.SetChildValue("RequestId","Direct1");
        R.SetChildValue("Operation",ArgC>1?ArgV[1]:"help"); R.SetChildValue("Backend","source"); R.SetChildValue("Build","reuse"); R.SetChildValue("Cook","reuse"); R.SetChildValue("Verify",false); R.SetChildValue("Archive",false);
        const std::map<std::string,std::string> Fields{{"--project","ProjectFile"},{"--platform","Platform"},{"--configuration","Configuration"},{"--output","Output"},{"--engine-root","EngineRoot"},{"--backend-metadata","BackendMetadata"},{"--cook-dir","CookDirectory"},{"--backend","Backend"},{"--build","Build"},{"--cook","Cook"},{"--game-preset","Preset"}};
        for (int I=2;I<ArgC;++I)
        {
            const std::string Option=ArgV[I];
            if(Option=="--verify" || Option=="--archive" || Option=="--preview") { R.SetChildValue(Option=="--verify"?"Verify":Option=="--archive"?"Archive":"Preview",true); continue; }
            if(Option=="--build") ExplicitBuild=true;
            if(Option=="--cook") ExplicitCook=true;
            if(Option=="--backend") ExplicitBackend=true;
            const auto It=Fields.find(Option);
            if (It==Fields.end() || I+1>=ArgC) { std::cerr<<"Invalid export option: "<<Option<<'\n'; return 2; }
            R.SetChildValue(It->second,ArgV[++I]);
        }
        if(R.GetView("Operation").GetString()=="export") {
            const auto Directory=std::filesystem::path(Durin::FPlatformProcess::ExecutablePath()).parent_path();
            Durin::FJsonDocument Component;
            if(!Component.LoadFromFile(Directory/"ExportComponent.json")) {std::cerr<<"Missing export component metadata beside worker\n"; return 3;}
            const auto C=Component.GetRootView();
            if(!ExplicitBackend) R.SetChildValue("Backend",C.GetView("DefaultBackend").GetString("source"));
            if(!R.GetView("EngineRoot").IsString()) {auto P=std::filesystem::u8path(C.GetView("EngineRoot").GetString()); R.SetChildValue("EngineRoot",(P.is_absolute()?P:Directory/P).generic_string());}
            if(!R.GetView("BackendMetadata").IsString()) R.SetChildValue("BackendMetadata",(Directory/C.GetView("BackendMetadata").GetString()).generic_string());
            if(!R.GetView("Configuration").IsString()) R.SetChildValue("Configuration","Release");
            if(!R.GetView("Platform").IsString()) R.SetChildValue("Platform","Win64");
            if(!ExplicitBuild && R.GetView("Backend").GetString()=="source") R.SetChildValue("Build","auto");
            if(!ExplicitCook && !R.GetView("CookDirectory").IsString()) R.SetChildValue("Cook",R.GetView("Backend").GetString()=="source"?"auto":"reuse");
        }
        Json=Request.ToString(Durin::EJsonFormat::Compact);
    }
    Durin::FJsonDocument Parsed; std::string Id;
    if (Parsed.Parse(Json))
    {
        const auto R=Parsed.GetRootView(); Id=R.GetView("RequestId").GetString();
        // Manifest package-path decoding initializes Core mounts. Bind the tool's
        // explicit engine root before that first consumer, including installed tools.
        const auto Engine=std::filesystem::u8path(R.GetView("EngineRoot").GetString());
        if (Engine.is_absolute())
        {
            Durin::FRuntimePathContext Paths;
            Paths.Bindings["Engine"]=Engine.lexically_normal().generic_string()+"/";
            Durin::PublishRuntimePathContext(std::move(Paths));
        }
    }
    std::string Pending;
    const auto IsCancelled=[&]() -> bool {
        if (Cancelled || !Protocol) return Cancelled!=0;
        // Poll on the service thread: no detached stdin reader or live thread at result.
#if defined(_WIN32)
        const HANDLE Input=GetStdHandle(STD_INPUT_HANDLE); DWORD Available=0;
        if (GetFileType(Input)==FILE_TYPE_PIPE)
        {
            if (!PeekNamedPipe(Input,nullptr,0,nullptr,&Available,nullptr)) { Cancelled=1; return true; }
            while (Available--)
            {
                char C; DWORD Read=0; if (!ReadFile(Input,&C,1,&Read,nullptr) || !Read) { Cancelled=1; return true; }
                if (C=='\n')
                {
                    Durin::FJsonDocument D;
                    if (!D.Parse(Pending) || D.GetRootView().Num()!=3 || !D.GetRootView().GetView("ProtocolVersion").IsInt()
                        || D.GetRootView().GetView("ProtocolVersion").GetInt()!=1
                        || D.GetRootView().GetView("Kind").GetString()!="cancel" || D.GetRootView().GetView("RequestId").GetString()!=Id)
                        throw std::invalid_argument("Invalid cancel frame/version/correlation");
                    Cancelled=1;
                    Pending.clear();
                }
                else Pending+=C;
                if (Pending.size()>1024*1024) throw std::invalid_argument("Cancel frame exceeds protocol limit");
            }
        }
#else
        pollfd FD{STDIN_FILENO,POLLIN,0};
        while (poll(&FD,1,0)>0 && (FD.revents&(POLLIN|POLLHUP)))
        {
            char C; if (::read(STDIN_FILENO,&C,1)!=1) { Cancelled=1; break; }
            if (C=='\n')
            {
                Durin::FJsonDocument D;
                if (!D.Parse(Pending) || D.GetRootView().Num()!=3 || D.GetRootView().GetView("ProtocolVersion").GetInt()!=1
                    || D.GetRootView().GetView("Kind").GetString()!="cancel" || D.GetRootView().GetView("RequestId").GetString()!=Id)
                    throw std::invalid_argument("Invalid cancel frame/version/correlation");
                Cancelled=1; Pending.clear();
            }
            else Pending+=C;
            if (Pending.size()>1024*1024) throw std::invalid_argument("Cancel frame exceeds protocol limit");
        }
#endif
        return Cancelled!=0;
    };
    return Durin::ExecuteGameExportRequest(Json,IsCancelled,[](std::string_view Event){std::cout<<Event<<'\n'<<std::flush;});
}
