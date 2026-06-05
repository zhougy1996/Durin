#pragma once

#include "CoreAPI.h"
#include "Yaml/Yaml.h"

namespace Durin
{
	CORE_API extern FYamlNodeView GAppConfig;

	CORE_API auto IsAppConfigLoaded() -> bool;

	namespace CoreInternal
	{
		CORE_API auto LoadApplicationConfig(std::string_view ConfigFile) -> bool;
	}
} // namespace Durin
