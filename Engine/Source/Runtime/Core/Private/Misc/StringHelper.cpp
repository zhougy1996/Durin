#include "Misc/StringHelper.h"

namespace Durin::StringUtils
{
	auto ContainsInsensitive(std::string_view Text, std::string_view Filter) -> bool
	{
		if (Filter.empty()) return true;
		if (Filter.size() > Text.size()) return false;

		const auto EqualsInsensitive = [](const char Left, const char Right) {
			return std::tolower(static_cast<unsigned char>(Left)) == std::tolower(static_cast<unsigned char>(Right));
		};
		return std::search(Text.begin(), Text.end(), Filter.begin(), Filter.end(), EqualsInsensitive) != Text.end();
	}
} // namespace Durin::StringUtils
