#include <gtest/gtest.h>

#include "Engine/EngineGlobals.h"

namespace Durin
{
	TEST(FEngineFrameTimingTests, PublishesMeasuredPhasesAndClearsSkippedSamples)
	{
		// Establish the clock baseline and a published sample without depending on
		// wall-clock sleeps or the order in which another caller initialized timing.
		CalculateFPSTimings();
		CalculateFPSTimings();
		const FEngineFrameTiming Before = GetEngineFrameTiming();
		RecordEngineFrameTickTiming(2.0f);
		RecordEngineFrameRenderTimings(6.0f, 8.0f, 10.0f, 12.0f);
		CalculateFPSTimings();
		const FEngineFrameTiming Measured = GetEngineFrameTiming();
		EXPECT_FLOAT_EQ(Measured.EngineTickMilliseconds, Before.EngineTickMilliseconds * 0.75f + 0.5f);
		EXPECT_FLOAT_EQ(Measured.UIFrameBuildMilliseconds, Before.UIFrameBuildMilliseconds * 0.75f + 1.5f);
		EXPECT_FLOAT_EQ(Measured.SceneSubmissionMilliseconds, Before.SceneSubmissionMilliseconds * 0.75f + 2.0f);
		EXPECT_FLOAT_EQ(Measured.UISubmissionMilliseconds, Before.UISubmissionMilliseconds * 0.75f + 2.5f);
		EXPECT_FLOAT_EQ(Measured.RenderSyncWaitMilliseconds, Before.RenderSyncWaitMilliseconds * 0.75f + 3.0f);

		// A minimized frame records tick phases but performs no rendering.
		RecordEngineFrameTickTiming(2.0f);
		CalculateFPSTimings();
		const FEngineFrameTiming Minimized = GetEngineFrameTiming();
		EXPECT_FLOAT_EQ(Minimized.EngineTickMilliseconds, Measured.EngineTickMilliseconds * 0.75f + 0.5f);
		EXPECT_FLOAT_EQ(Minimized.UIFrameBuildMilliseconds, Measured.UIFrameBuildMilliseconds * 0.75f);
		EXPECT_FLOAT_EQ(Minimized.SceneSubmissionMilliseconds, Measured.SceneSubmissionMilliseconds * 0.75f);
		EXPECT_FLOAT_EQ(Minimized.UISubmissionMilliseconds, Measured.UISubmissionMilliseconds * 0.75f);
		EXPECT_FLOAT_EQ(Minimized.RenderSyncWaitMilliseconds, Measured.RenderSyncWaitMilliseconds * 0.75f);

		CalculateFPSTimings();
		const FEngineFrameTiming Skipped = GetEngineFrameTiming();
		EXPECT_FLOAT_EQ(Skipped.EngineTickMilliseconds, Minimized.EngineTickMilliseconds * 0.75f);
		EXPECT_FLOAT_EQ(Skipped.UIFrameBuildMilliseconds, Minimized.UIFrameBuildMilliseconds * 0.75f);
		EXPECT_FLOAT_EQ(Skipped.SceneSubmissionMilliseconds, Minimized.SceneSubmissionMilliseconds * 0.75f);
		EXPECT_FLOAT_EQ(Skipped.UISubmissionMilliseconds, Minimized.UISubmissionMilliseconds * 0.75f);
		EXPECT_FLOAT_EQ(Skipped.RenderSyncWaitMilliseconds, Minimized.RenderSyncWaitMilliseconds * 0.75f);
	}
}
