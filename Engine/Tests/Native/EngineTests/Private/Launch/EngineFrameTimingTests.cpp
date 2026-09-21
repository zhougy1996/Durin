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
		RecordEngineFrameTickTimings(2.0f, 4.0f);
		RecordEngineFrameRenderTimings(6.0f, 8.0f);
		CalculateFPSTimings();
		const FEngineFrameTiming Measured = GetEngineFrameTiming();
		EXPECT_FLOAT_EQ(Measured.EngineTickMilliseconds, Before.EngineTickMilliseconds * 0.75f + 0.5f);
		EXPECT_FLOAT_EQ(Measured.UITickMilliseconds, Before.UITickMilliseconds * 0.75f + 1.0f);
		EXPECT_FLOAT_EQ(Measured.RenderSubmissionMilliseconds, Before.RenderSubmissionMilliseconds * 0.75f + 1.5f);
		EXPECT_FLOAT_EQ(Measured.RenderSyncWaitMilliseconds, Before.RenderSyncWaitMilliseconds * 0.75f + 2.0f);

		// A minimized frame records tick phases but performs no rendering.
		RecordEngineFrameTickTimings(2.0f, 4.0f);
		CalculateFPSTimings();
		const FEngineFrameTiming Minimized = GetEngineFrameTiming();
		EXPECT_FLOAT_EQ(Minimized.EngineTickMilliseconds, Measured.EngineTickMilliseconds * 0.75f + 0.5f);
		EXPECT_FLOAT_EQ(Minimized.UITickMilliseconds, Measured.UITickMilliseconds * 0.75f + 1.0f);
		EXPECT_FLOAT_EQ(Minimized.RenderSubmissionMilliseconds, Measured.RenderSubmissionMilliseconds * 0.75f);
		EXPECT_FLOAT_EQ(Minimized.RenderSyncWaitMilliseconds, Measured.RenderSyncWaitMilliseconds * 0.75f);

		CalculateFPSTimings();
		const FEngineFrameTiming Skipped = GetEngineFrameTiming();
		EXPECT_FLOAT_EQ(Skipped.EngineTickMilliseconds, Minimized.EngineTickMilliseconds * 0.75f);
		EXPECT_FLOAT_EQ(Skipped.UITickMilliseconds, Minimized.UITickMilliseconds * 0.75f);
		EXPECT_FLOAT_EQ(Skipped.RenderSubmissionMilliseconds, Minimized.RenderSubmissionMilliseconds * 0.75f);
		EXPECT_FLOAT_EQ(Skipped.RenderSyncWaitMilliseconds, Minimized.RenderSyncWaitMilliseconds * 0.75f);
	}
}
