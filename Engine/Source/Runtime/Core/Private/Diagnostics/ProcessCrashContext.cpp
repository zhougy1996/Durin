#include "Diagnostics/ProcessCrashContext.h"

#include "HAL/Platform.h"
#include "HAL/PlatformProcess.h"

namespace Durin
{
	namespace
	{
		template<size_t Capacity>
		struct FPublishedText
		{
			std::atomic<uint64> Sequence{0};
			std::array<char, Capacity> Buffers[2]{};
		};

		struct FProcessCrashState
		{
			std::atomic<uint64> ProcessStartUtcMilliseconds{0};
			std::atomic<uint64> ProcessStartMonotonicMicroseconds{0};
			FPublishedText<ProcessCrashIdentityCapacity> RuntimeVariant;
			FPublishedText<ProcessCrashIdentityCapacity> BuildConfiguration;
			FPublishedText<ProcessCrashIdentityCapacity> BuildIdentity;
			FPublishedText<ProcessCrashPathCapacity> ActiveLogPath;
			std::atomic<uint64> LastAcceptedLogSequence{0};
			std::atomic<uint64> LastProcessedLogSequence{0};
			std::atomic<uint64> LastDurableLogSequence{0};
		};

		static_assert(std::atomic<uint64>::is_always_lock_free);
		static_assert(std::atomic<uint32>::is_always_lock_free);

		FProcessCrashState GProcessCrashState;

		auto MonotonicMicroseconds() -> uint64
		{
			return static_cast<uint64>(std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		template<size_t Capacity>
		auto PublishText(FPublishedText<Capacity>& Target, std::string_view Text) -> void
		{
			const uint64 Current = Target.Sequence.load(std::memory_order_relaxed);
			const uint64 Writing = Current + 1 + (Current & 1);
			Target.Sequence.store(Writing, std::memory_order_release);
			auto& Buffer = Target.Buffers[((Writing + 1) >> 1) & 1];
			const size_t Count = std::min(Text.size(), Capacity - 1);
			std::memcpy(Buffer.data(), Text.data(), Count);
			Buffer[Count] = '\0';
			if (Count + 1 < Capacity) std::memset(Buffer.data() + Count + 1, 0, Capacity - Count - 1);
			Target.Sequence.store(Writing + 1, std::memory_order_release);
		}

		template<size_t Capacity>
		auto ReadText(const FPublishedText<Capacity>& Source, std::array<char, Capacity>& Out) -> void
		{
			for (uint32 Attempt = 0; Attempt < 4; ++Attempt)
			{
				const uint64 Before = Source.Sequence.load(std::memory_order_acquire);
				if (Before & 1) continue;
				Out = Source.Buffers[(Before >> 1) & 1];
				if (Before == Source.Sequence.load(std::memory_order_acquire)) return;
			}
			Out.fill(0);
		}
	}

	auto InitializeProcessCrashContext(
		std::string_view RuntimeVariant,
		std::string_view BuildConfiguration,
		std::string_view BuildIdentity) -> void
	{
		GProcessCrashState.ProcessStartUtcMilliseconds.store(static_cast<uint64>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::system_clock::now().time_since_epoch()).count()), std::memory_order_relaxed);
		GProcessCrashState.ProcessStartMonotonicMicroseconds.store(MonotonicMicroseconds(), std::memory_order_relaxed);
		PublishText(GProcessCrashState.RuntimeVariant, RuntimeVariant);
		PublishText(GProcessCrashState.BuildConfiguration, BuildConfiguration);
		PublishText(GProcessCrashState.BuildIdentity, BuildIdentity);
	}

	auto PublishProcessCrashLogPath(std::string_view Path) -> void { PublishText(GProcessCrashState.ActiveLogPath, Path); }
	auto PublishProcessCrashLogAccepted(uint64 Sequence) -> void { GProcessCrashState.LastAcceptedLogSequence.store(Sequence, std::memory_order_release); }
	auto PublishProcessCrashLogProcessed(uint64 Sequence) -> void { GProcessCrashState.LastProcessedLogSequence.store(Sequence, std::memory_order_release); }
	auto PublishProcessCrashLogDurable(uint64 Sequence) -> void { GProcessCrashState.LastDurableLogSequence.store(Sequence, std::memory_order_release); }

	auto ReadProcessCrashContext() -> FProcessCrashContextSnapshot
	{
		FProcessCrashContextSnapshot Result;
		Result.ProcessStartUtcMilliseconds = GProcessCrashState.ProcessStartUtcMilliseconds.load(std::memory_order_relaxed);
		Result.ProcessStartMonotonicMicroseconds = GProcessCrashState.ProcessStartMonotonicMicroseconds.load(std::memory_order_relaxed);
		ReadText(GProcessCrashState.RuntimeVariant, Result.RuntimeVariant);
		ReadText(GProcessCrashState.BuildConfiguration, Result.BuildConfiguration);
		ReadText(GProcessCrashState.BuildIdentity, Result.BuildIdentity);
		ReadText(GProcessCrashState.ActiveLogPath, Result.ActiveLogPath);
		Result.LastAcceptedLogSequence = GProcessCrashState.LastAcceptedLogSequence.load(std::memory_order_acquire);
		Result.LastProcessedLogSequence = GProcessCrashState.LastProcessedLogSequence.load(std::memory_order_acquire);
		Result.LastDurableLogSequence = GProcessCrashState.LastDurableLogSequence.load(std::memory_order_acquire);
		return Result;
	}

}
