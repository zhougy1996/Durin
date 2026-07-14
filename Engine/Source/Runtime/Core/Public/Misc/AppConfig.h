#pragma once

#include "CoreAPI.h"
#include "CoreFwd.h"
#include "Yaml/Yaml.h"

namespace Durin
{
	CORE_API auto LoadAppConfig(std::string_view ConfigFile) -> bool;
	// Modules receive only their top-level section; a missing section produces an invalid view.
	CORE_API auto GetModuleConfig(std::string_view ModuleName) -> FYamlNodeView;
}
