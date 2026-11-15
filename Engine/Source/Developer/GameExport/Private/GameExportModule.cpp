#include "Modules/ModuleManager.h"

namespace Durin
{
    class FGameExportModule final : public IModuleInterface
    {
    };

    IMPLEMENT_MODULE(FGameExportModule, GameExport)
}
