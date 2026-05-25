#pragma once

namespace Durin
{
	struct FGenericWindowDefinition
	{
		float XDesiredPositionOnScreen;
		float YDesiredPositionOnScreen;

		float WidthDesiredOnScreen;
		float HeightDesiredOnScreen;

		bool bHasOSWindowBorder = true;

		std::string Title;
	};
}
