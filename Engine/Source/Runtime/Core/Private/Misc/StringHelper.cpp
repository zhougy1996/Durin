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

	auto HumanizeName(std::string_view Name) -> std::string
	{
		std::string Result;
		Result.reserve(Name.size() + 8);
		for (size_t Index = 0; Index < Name.size(); ++Index)
		{
			const unsigned char Current = static_cast<unsigned char>(Name[Index]);
			const bool bCurrentUpper = std::isupper(Current) != 0;
			const bool bPreviousLowerOrDigit = Index > 0 && (std::islower(static_cast<unsigned char>(Name[Index - 1])) || std::isdigit(static_cast<unsigned char>(Name[Index - 1])));
			const bool bAcronymBoundary = Index > 0 && Index + 1 < Name.size() && bCurrentUpper
				&& std::isupper(static_cast<unsigned char>(Name[Index - 1])) && std::islower(static_cast<unsigned char>(Name[Index + 1]));
			if (bCurrentUpper && (bPreviousLowerOrDigit || bAcronymBoundary)) Result.push_back(' ');
			Result.push_back(Name[Index]);
		}
		return Result;
	}
} // namespace Durin::StringUtils
