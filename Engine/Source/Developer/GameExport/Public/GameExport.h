#pragma once
#include "CoreMinimal.h"
#include "GameExportAPI.h"

namespace Durin
{
    // Native policy owner. Clients supply wire requests and explicit backend inputs only.
    GAMEEXPORT_API auto ExecuteGameExportRequest(std::string_view Json,
        const std::function<bool()>& IsCancelled,
        const std::function<void(std::string_view)>& EmitEvent) -> int;
    // Thin process client shared by Editor and direct/native integrations.
    GAMEEXPORT_API auto RunGameExportWorker(const std::filesystem::path& Worker, std::string_view Request,
        const std::function<bool()>& IsCancelled, const std::function<void(std::string_view)>& OnEvent) -> int;
}
