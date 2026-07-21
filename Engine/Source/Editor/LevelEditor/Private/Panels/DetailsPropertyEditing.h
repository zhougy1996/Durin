#pragma once

#include "DObject/DObjectGlobals.h"

namespace Durin
{
	auto MakeDetailsPropertyDisplayName(
		std::string_view PropertyName,
		DurinCodeGen::EPropertyGenFlags Kind,
		std::string_view ExplicitDisplayName = {}
	) -> std::string;
}
