#pragma once

namespace Durin
{
	// Selects which layer owns a window's visible frame and title bar.
	enum class EWindowDecorationMode : uint8
	{
		System,
		CustomTitleBar,
		None
	};

	// Describes the initial placement and presentation of a platform window.
	struct FGenericWindowDefinition
	{
		// Desired position and client size in screen pixels.
		float XDesiredPositionOnScreen;
		float YDesiredPositionOnScreen;

		float WidthDesiredOnScreen;
		float HeightDesiredOnScreen;

		EWindowDecorationMode DecorationMode = EWindowDecorationMode::System;

		std::string Title;
	};
}
