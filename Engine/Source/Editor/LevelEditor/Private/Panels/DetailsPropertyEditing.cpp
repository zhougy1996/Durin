#include "Panels/DetailsPropertyEditing.h"

#include "Misc/StringHelper.h"

namespace Durin
{
	auto MakeDetailsPropertyDisplayName(
		std::string_view PropertyName,
		DurinCodeGen::EPropertyGenFlags Kind,
		std::string_view ExplicitDisplayName
	) -> std::string
	{
		if (!ExplicitDisplayName.empty()) return std::string(ExplicitDisplayName);

		// The leading b is a C++ type convention, not part of the user-facing name.
		// Requiring an uppercase successor keeps unrelated names such as "border" intact.
		if (Kind == DurinCodeGen::EPropertyGenFlags::Bool && PropertyName.size() > 1 && PropertyName.front() == 'b'
			&& std::isupper(static_cast<unsigned char>(PropertyName[1])))
		{
			PropertyName.remove_prefix(1);
		}
		return StringUtils::HumanizeName(PropertyName);
	}
}
