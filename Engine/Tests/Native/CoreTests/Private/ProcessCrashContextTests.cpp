#include "Diagnostics/ProcessCrashContext.h"

#include <gtest/gtest.h>

namespace Durin
{
	TEST(FProcessCrashContextTests, PublishesIdentityPhaseAndFixedText)
	{
		InitializeProcessCrashContext("DurinEditor", "Debug", "1.2.3-dev");
		SetProcessCrashPhase(EProcessCrashPhase::Running);
		PublishProcessCrashLogPath("C:/Runtime/Saved/Logs/Durin.log");
		PublishProcessCrashLogAccepted(9);
		PublishProcessCrashLogProcessed(7);
		PublishProcessCrashLogDurable(5);

		const FProcessCrashContextSnapshot Snapshot = ReadProcessCrashContext();
		EXPECT_EQ(Snapshot.Phase, EProcessCrashPhase::Running);
		EXPECT_STREQ(Snapshot.RuntimeVariant.data(), "DurinEditor");
		EXPECT_STREQ(Snapshot.BuildConfiguration.data(), "Debug");
		EXPECT_STREQ(Snapshot.BuildIdentity.data(), "1.2.3-dev");
		EXPECT_STREQ(Snapshot.ActiveLogPath.data(), "C:/Runtime/Saved/Logs/Durin.log");
		EXPECT_EQ(Snapshot.LastAcceptedLogSequence, 9u);
		EXPECT_EQ(Snapshot.LastProcessedLogSequence, 7u);
		EXPECT_EQ(Snapshot.LastDurableLogSequence, 5u);
	}

	TEST(FProcessCrashContextTests, RetainsOnlyCommittedRingGenerationsAfterWraparound)
	{
		const uint64 Before = ReadProcessCrashContext().BreadcrumbWriteSequence;
		for (uint64 Index = 0; Index < ProcessCrashBreadcrumbCapacity + 11; ++Index)
		{
			AddProcessCrashBreadcrumb(EProcessCrashBreadcrumbEvent::FirstObjectCollection, Index, Index + 1);
		}

		const FProcessCrashContextSnapshot Snapshot = ReadProcessCrashContext();
		ASSERT_EQ(Snapshot.BreadcrumbCount, ProcessCrashBreadcrumbCapacity);
		EXPECT_EQ(Snapshot.BreadcrumbWriteSequence, Before + ProcessCrashBreadcrumbCapacity + 11);
		uint64 Previous = 0;
		for (uint32 Index = 0; Index < Snapshot.BreadcrumbCount; ++Index)
		{
			EXPECT_GT(Snapshot.Breadcrumbs[Index].Sequence, Previous);
			EXPECT_EQ(Snapshot.Breadcrumbs[Index].Event, EProcessCrashBreadcrumbEvent::FirstObjectCollection);
			Previous = Snapshot.Breadcrumbs[Index].Sequence;
		}
	}

	TEST(FProcessCrashContextTests, ConcurrentWritersNeverExposeTornCommittedRecords)
	{
		constexpr uint32 WriterCount = 8;
		constexpr uint32 RecordsPerWriter = 1000;
		std::vector<std::jthread> Writers;
		for (uint32 Writer = 0; Writer < WriterCount; ++Writer)
		{
			Writers.emplace_back([Writer] {
				for (uint32 Index = 0; Index < RecordsPerWriter; ++Index)
				{
					AddProcessCrashBreadcrumb(
						EProcessCrashBreadcrumbEvent::DeferredDestroyAudit,
						Writer,
						(static_cast<uint64>(Writer) << 32) | Index);
				}
			});
		}
		Writers.clear();

		const FProcessCrashContextSnapshot Snapshot = ReadProcessCrashContext();
		ASSERT_EQ(Snapshot.BreadcrumbCount, ProcessCrashBreadcrumbCapacity);
		for (uint32 Index = 0; Index < Snapshot.BreadcrumbCount; ++Index)
		{
			const FProcessCrashBreadcrumb& Record = Snapshot.Breadcrumbs[Index];
			EXPECT_EQ(Record.Event, EProcessCrashBreadcrumbEvent::DeferredDestroyAudit);
			EXPECT_EQ(Record.Argument0, Record.Argument1 >> 32);
		}
	}
}
