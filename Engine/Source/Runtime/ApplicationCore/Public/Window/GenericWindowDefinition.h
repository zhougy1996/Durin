#pragma once

#include "ApplicationCoreAPI.h"

namespace Doge
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