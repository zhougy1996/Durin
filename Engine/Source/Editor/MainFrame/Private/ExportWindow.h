#pragma once
#include "CoreMinimal.h"
namespace Durin { class DAssetEditorSubsystem; }
namespace Durin::MainFrame {
    class FExportWindow {
    public:
        ~FExportWindow();
        void Stop();
        void Draw(bool& Open, DAssetEditorSubsystem* Editors);
    private:
        void Start(bool Preview);
        std::jthread Worker;
        std::atomic<bool> Running=false, Cancel=false;
        std::mutex Mutex;
        std::string Message;
        char Output[2048]{};
        int Configuration=1;
        bool Verify=false, Archive=false, ConfirmSave=false;
    };
}
