#pragma once
#include "CoreMinimal.h"
#include "Plugins/PluginResources.h"
namespace Durin
{
	auto ResolveProcessPlugins(std::string_view ProjectFile, FPluginResolution& OutSelection,
		std::vector<FAdmittedPluginBinary>& OutBinaries, FPluginResources& OutResources, std::string& OutReason,
        bool bAllowUnavailablePlugins, std::vector<std::string>& OutDiagnostics) -> bool;
}
