#pragma once

#include "ApplicationCore/API.h"

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