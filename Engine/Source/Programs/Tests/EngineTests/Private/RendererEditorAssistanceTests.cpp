#include <gtest/gtest.h>

#include "RendererEditorAssistance.h"

namespace Durin
{
	using RendererEditorAssistance::EDrawOperation;

	TEST(FRendererEditorAssistanceTests, DrawOrderKeepsAllAssistanceAfterGridAndXRayBeforeVisible)
	{
		const std::span<const EDrawOperation> Order = RendererEditorAssistance::GetDrawOrder();
		const std::array Expected{
			EDrawOperation::EditorGrid,
			EDrawOperation::XRayGizmos,
			EDrawOperation::XRayOverlayLines,
			EDrawOperation::XRayOverlayIcons,
			EDrawOperation::VisibleGizmos,
			EDrawOperation::VisibleOverlayLines,
			EDrawOperation::VisibleOverlayIcons,
		};

		ASSERT_EQ(Order.size(), Expected.size());
		EXPECT_TRUE(std::ranges::equal(Order, Expected));
	}

	TEST(FRendererEditorAssistanceTests, EveryAssistanceOperationAppearsExactlyOnce)
	{
		const std::span<const EDrawOperation> Order = RendererEditorAssistance::GetDrawOrder();
		for (const EDrawOperation Operation : Order)
		{
			EXPECT_EQ(std::ranges::count(Order, Operation), 1);
		}
	}
} // namespace Durin
