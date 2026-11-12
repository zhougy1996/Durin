#pragma once
#include "CoreMinimal.h"
#include "Plugins/PluginManagement.h"

namespace Durin::MainFrame
{
    class FPluginManagementWindow
    {
    public:
        auto Draw(bool& bOpen, const std::function<void()>& RequestRestart) -> void;
        auto CheckRestartReady(std::string& Reason) -> bool;
        auto Report(std::string Message) -> void { Error = std::move(Message); }
    private:
        std::optional<FPluginEditSession> Session;
        std::string Error;
        std::array<char, 128> Filter{};
        bool bWasOpen = false;
        auto Reload() -> void;
    };
}
