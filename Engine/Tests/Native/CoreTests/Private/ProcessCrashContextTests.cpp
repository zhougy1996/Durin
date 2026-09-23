#include "Diagnostics/ProcessCrashContext.h"

#include <gtest/gtest.h>

namespace Durin
{
	TEST(FProcessCrashContextTests, PublishesIdentityAndFixedText)
	{
		InitializeProcessCrashContext("DurinEditor", "Debug", "1.2.3-dev");
		PublishProcessCrashLogPath("C:/Runtime/Saved/Logs/Durin.log");
		PublishProcessCrashLogAccepted(9);
		PublishProcessCrashLogProcessed(7);
		PublishProcessCrashLogDurable(5);

		const FProcessCrashContextSnapshot Snapshot = ReadProcessCrashContext();
		EXPECT_STREQ(Snapshot.RuntimeVariant.data(), "DurinEditor");
		EXPECT_STREQ(Snapshot.BuildConfiguration.data(), "Debug");
		EXPECT_STREQ(Snapshot.BuildIdentity.data(), "1.2.3-dev");
		EXPECT_STREQ(Snapshot.ActiveLogPath.data(), "C:/Runtime/Saved/Logs/Durin.log");
		EXPECT_EQ(Snapshot.LastAcceptedLogSequence, 9u);
		EXPECT_EQ(Snapshot.LastProcessedLogSequence, 7u);
		EXPECT_EQ(Snapshot.LastDurableLogSequence, 5u);
	}

}
