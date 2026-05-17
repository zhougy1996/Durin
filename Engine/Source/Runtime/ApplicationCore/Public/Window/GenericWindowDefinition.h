#pragma once

#include "ApplicationCoreAPI.h"

namespace Durin
{
	struct APPLICATIONCORE_API FGenericWindowDefinition
	{
		float XDesiredPositionOnScreen;
		float YDesiredPositionOnScreen;

		float WidthDesiredOnScreen;
		float HeightDesiredOnScreen;

		std::string Title;
	};
}