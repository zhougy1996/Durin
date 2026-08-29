#include <gtest/gtest.h>

#include "PrimitiveDrawInterface.h"
#include "SceneView.h"

namespace Durin
{
	TEST(FPrimitiveDrawInterfaceTests, CopiesAndClassifiesSupportedCalls)
	{
		FSceneView View;
		FViewPrimitiveDrawInterface PDI(View);
		FVector3 Start{1.0, 2.0, 3.0};
		FVector4f Color{0.2f, 0.3f, 0.4f, 0.25f};
		PDI.DrawLine(Start, {4.0, 5.0, 6.0}, Color,
			ESceneDepthPriorityGroup::World,
			{.WidthPixels = 3.0f,
				.Pattern = ESimpleElementLinePattern::Dashed,
				.PatternPeriodPixels = 10.0f});
		PDI.DrawTranslucentLine({0.0, 0.0, 0.0}, {0.0, 1.0, 0.0},
			Color, ESceneDepthPriorityGroup::Foreground);
		PDI.DrawPoint({0.0, 0.0, 1.0}, Color, 7.0f);
		PDI.DrawSprite({0.0, 0.0, 2.0}, {32.0f, 24.0f},
			FSimpleElementTexture::EditorIconAtlas(), {0.0f, 0.0f},
			{0.25f, 1.0f}, Color,
			ESceneDepthPriorityGroup::Foreground);
		Start = FVector3(99.0);
		Color = FVector4f(1.0f);
		PDI.Seal();

		const auto Elements = View.SimpleElements.GetElements();
		ASSERT_EQ(Elements.size(), 4u);
		EXPECT_TRUE(View.SimpleElements.IsSealed());
		EXPECT_EQ(Elements[0].Type, ESimpleElementType::Line);
		EXPECT_EQ(Elements[0].BlendMode, ESimpleElementBlendMode::Opaque);
		const auto& OpaqueLine = std::get<FSimpleElementLine>(Elements[0].Value);
		EXPECT_EQ(OpaqueLine.Start, FVector3(1.0, 2.0, 3.0));
		EXPECT_FLOAT_EQ(OpaqueLine.Color.w, 1.0f);
		EXPECT_EQ(OpaqueLine.Style.Pattern,
			ESimpleElementLinePattern::Dashed);
		EXPECT_EQ(Elements[1].BlendMode,
			ESimpleElementBlendMode::Translucent);
		EXPECT_EQ(Elements[1].DepthPriorityGroup,
			ESceneDepthPriorityGroup::Foreground);
		EXPECT_FLOAT_EQ(
			std::get<FSimpleElementLine>(Elements[1].Value).Color.w, 0.25f);
		EXPECT_EQ(Elements[3].Type, ESimpleElementType::Sprite);
		EXPECT_TRUE(std::get<FSimpleElementSprite>(Elements[3].Value)
			.Texture.IsValid());
		for (uint64 Index = 0; Index < Elements.size(); ++Index)
			EXPECT_EQ(Elements[Index].SubmissionOrder, Index);
	}

	TEST(FPrimitiveDrawInterfaceTests, InvalidAndPostSealCallsAreRejected)
	{
		FSceneView View;
		FViewPrimitiveDrawInterface PDI(View);
		PDI.DrawLine({}, {}, FVector4f(1.0f));
		PDI.DrawPoint({}, FVector4f(1.0f), 0.0f);
		PDI.DrawSprite({}, {10.0f, 10.0f}, {}, FVector2f(0.0f), FVector2f(1.0f),
			FVector4f(1.0f));
		EXPECT_TRUE(View.SimpleElements.GetElements().empty());
		EXPECT_EQ(View.SimpleElements.GetDroppedElementCount(), 3u);
		PDI.Seal();
		const uint32 Dropped = View.SimpleElements.GetDroppedElementCount();
		PDI.DrawPoint({}, FVector4f(1.0f), 2.0f);
		EXPECT_TRUE(View.SimpleElements.GetElements().empty());
		EXPECT_EQ(View.SimpleElements.GetDroppedElementCount(), Dropped);
	}

	TEST(FPrimitiveDrawInterfaceTests, ViewsOwnIndependentSealedSubmissions)
	{
		FSceneView First;
		FSceneView Second;
		{
			FViewPrimitiveDrawInterface PDI(First);
			PDI.DrawPoint({1.0, 0.0, 0.0}, FVector4f(1.0f), 2.0f);
			PDI.Seal();
		}
		{
			FViewPrimitiveDrawInterface PDI(Second);
			PDI.DrawPoint({2.0, 0.0, 0.0}, FVector4f(1.0f), 3.0f);
			PDI.Seal();
		}
		ASSERT_EQ(First.SimpleElements.GetElements().size(), 1u);
		ASSERT_EQ(Second.SimpleElements.GetElements().size(), 1u);
		EXPECT_EQ(std::get<FSimpleElementPoint>(
			First.SimpleElements.GetElements()[0].Value).Position.x, 1.0);
		EXPECT_EQ(std::get<FSimpleElementPoint>(
			Second.SimpleElements.GetElements()[0].Value).Position.x, 2.0);
	}

	TEST(FPrimitiveDrawInterfaceTests, EnforcesPerViewElementBound)
	{
		FSceneView View;
		FViewPrimitiveDrawInterface PDI(View);
		for (uint32 Index = 0;
			Index < FSimpleElementViewSubmission::MaxElementCount + 1;
			++Index)
		{
			PDI.DrawPoint({static_cast<double>(Index), 0.0, 1.0},
				FVector4f(1.0f), 1.0f);
		}
		PDI.Seal();

		EXPECT_EQ(View.SimpleElements.GetElements().size(),
			FSimpleElementViewSubmission::MaxElementCount);
		EXPECT_EQ(View.SimpleElements.GetDroppedElementCount(), 1u);
		EXPECT_LE(View.SimpleElements.GetPayloadBytes(),
			FSimpleElementViewSubmission::MaxPayloadBytes);
	}
} // namespace Durin
