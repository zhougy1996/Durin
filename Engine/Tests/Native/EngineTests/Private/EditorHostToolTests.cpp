#include <gtest/gtest.h>

#include "Panels/ConsolePanelLayout.h"

namespace Durin::Editor::MainFrame
{
	TEST(FEditorHostToolTests, ClipsVariableHeightConsoleRecordsWithOverscan)
	{
		const std::array<float, 5> Offsets{0.0f, 20.0f, 60.0f, 80.0f, 140.0f};
		const FConsoleVisibleRange Middle =
			ResolveConsoleVisibleRange(Offsets, 55.0f, 30.0f);
		EXPECT_EQ(Middle.Begin, 0u);
		EXPECT_EQ(Middle.End, 4u);

		const FConsoleVisibleRange Bottom =
			ResolveConsoleVisibleRange(Offsets, 120.0f, 30.0f);
		EXPECT_EQ(Bottom.Begin, 2u);
		EXPECT_EQ(Bottom.End, 4u);
		EXPECT_EQ(ResolveConsoleVisibleRange({}, 0.0f, 100.0f).End, 0u);
	}
} // namespace Durin::Editor::MainFrame
