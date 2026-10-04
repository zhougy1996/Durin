#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"

#include <gtest/gtest.h>

namespace
{
	class FScopedEnvironmentVariable
	{
	public:
		FScopedEnvironmentVariable(const char* InName, const char* Value)
			: Name(InName)
		{
			if (const char* Previous = std::getenv(InName)) Original = Previous;
			setenv(InName, Value, 1);
		}

		~FScopedEnvironmentVariable()
		{
			if (Original) setenv(Name, Original->c_str(), 1);
			else unsetenv(Name);
		}
	private:
		const char* Name;
		std::optional<std::string> Original;
	};

	struct FScopedRHIExit
	{
		~FScopedRHIExit()
		{
			if (Durin::GDynamicRHI) Durin::RHIExit();
		}
	};
}

TEST(FMetalRHIHeadlessTests, DeviceAndSingleQueueInitializeInBothExecutionModes)
{
	FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
	for (const char* Mode : {"inline", "threaded"})
	{
		SCOPED_TRACE(Mode);
		FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", Mode);
		ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
		FScopedRHIExit Exit;
		ASSERT_NE(Durin::GDynamicRHI, nullptr);
		EXPECT_EQ(Durin::GDynamicRHI->RHIGetCapabilities(), nullptr);
		const auto& Queues = Durin::GDynamicRHI->RHIGetQueueCapabilities();
		ASSERT_EQ(Queues.Queues.size(), 1u);
		EXPECT_EQ(Queues.Graphics, Queues.Compute);
		EXPECT_FALSE(Queues.bIndependentCompute);
		auto& Commands = Durin::FRHICommandListImmediate::Get();
		const auto First = Commands.BeginGPUSubmission({.Queue = Queues.Graphics});
		Commands.EndGPUSubmission();
		const auto Second = Commands.BeginGPUSubmission(
			{.Queue = Queues.Graphics, .Waits = {First}});
		Commands.EndGPUSubmission();
		EXPECT_EQ(First.GetState(), Durin::ERHIGPUSubmissionState::Pending);
		Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread);
		EXPECT_EQ(First.GetState(), Durin::ERHIGPUSubmissionState::Pending);
		EXPECT_EQ(Second.GetState(), Durin::ERHIGPUSubmissionState::Pending);
		EXPECT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Second, 0),
			Durin::ERHIGPUWaitResult::Pending);
		Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
			Durin::ERHISubmitFlags::SubmitToGPU);
		EXPECT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Second, 1'000'000'000),
			Durin::ERHIGPUWaitResult::Complete);
		EXPECT_EQ(First.GetState(), Durin::ERHIGPUSubmissionState::Complete);
		EXPECT_TRUE(First.IsRetirementEligible());
		EXPECT_TRUE(Second.IsRetirementEligible());
		Durin::RHIExit();
		EXPECT_EQ(Durin::GDynamicRHI, nullptr);
	}
}

TEST(FMetalRHIHeadlessTests, ShutdownCancelsReplayedButUnsubmittedWork)
{
	FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
	FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", "inline");
	ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
	FScopedRHIExit Exit;
	auto& Commands = Durin::FRHICommandListImmediate::Get();
	const auto Signal = Commands.BeginGPUSubmission(
		{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
	Commands.EndGPUSubmission();
	Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread);
	EXPECT_EQ(Signal.GetState(), Durin::ERHIGPUSubmissionState::Pending);
	Durin::RHIExit();
	EXPECT_EQ(Signal.GetState(), Durin::ERHIGPUSubmissionState::Canceled);
	EXPECT_TRUE(Signal.IsRetirementEligible());
}
