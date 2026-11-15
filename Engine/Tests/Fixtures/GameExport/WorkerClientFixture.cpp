#include "CoreMinimal.h"
#include "GameExport.h"
#include "Json/Json.h"

int main(int Count,char** Args)
{
    if(Count<3) return 2;
    try {
        std::ifstream Input(Args[2],std::ios::binary);
        const std::string Request((std::istreambuf_iterator<char>(Input)),{});
        bool Cancelled=false;
        return Durin::RunGameExportWorker(std::filesystem::u8path(Args[1]),Request,[&]{return Cancelled;},[&](std::string_view Line){
            std::cout<<Line<<'\n'<<std::flush;
            if(Count>3) {Durin::FJsonDocument Event; if(Event.Parse(Line) && Event.GetRootView().GetView("Phase").GetString()==Args[3]) Cancelled=true;}
        });
    } catch(const std::exception& E) {std::cerr<<E.what()<<'\n'; return 70;}
}
