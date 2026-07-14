#pragma once

#include "CoreAPI.h"

namespace Durin::StringUtils
{
	[[nodiscard]] CORE_API auto ContainsInsensitive(std::string_view Text, std::string_view Filter) -> bool;
}
