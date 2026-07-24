#pragma once

namespace Durin
{
	// Describes the initial placement and presentation of a platform window.
	struct FGenericWindowDefinition
	{
		// Desired position and client size in screen pixels.
		float XDesiredPositionOnScreen;
		float YDesiredPositionOnScreen;

		float WidthDesiredOnScreen;
		float HeightDesiredOnScreen;

		bool bHasOSWindowBorder = true;

		std::string Title;
	};
}
