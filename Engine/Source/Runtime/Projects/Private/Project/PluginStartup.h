#pragma once
#include "CoreMinimal.h"
#include "Plugins/PluginResolution.h"
namespace Durin
{
	auto ResolveProcessPlugins(std::string_view ProjectFile, FPluginResolution& OutSelection,
		std::vector<FAdmittedPluginBinary>& OutBinaries, std::string& OutReason) -> bool;
}
