#pragma once

namespace Durin::DerivedData::Private
{
	inline auto IsBuildValueIdentifier(std::string_view Id) -> bool
	{
		if (Id.empty() || Id.size() > 96) return false;
		return std::ranges::all_of(Id, [](unsigned char C) {
			return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z')
				|| (C >= '0' && C <= '9') || C == '.' || C == '/' || C == '_' || C == '-';
		});
	}
}
