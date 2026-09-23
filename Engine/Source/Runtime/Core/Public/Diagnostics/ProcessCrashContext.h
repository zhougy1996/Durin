#pragma once

#include "CoreAPI.h"

namespace Durin
{
	inline constexpr uint32 ProcessCrashPathCapacity = 1024;
	inline constexpr uint32 ProcessCrashIdentityCapacity = 64;

	// Copies the fixed crash-readable state without locks, waits, or allocation.
	struct FProcessCrashContextSnapshot
	{
		uint64 ProcessStartUtcMilliseconds = 0;
		uint64 ProcessStartMonotonicMicroseconds = 0;
		std::array<char, ProcessCrashIdentityCapacity> RuntimeVariant{};
		std::array<char, ProcessCrashIdentityCapacity> BuildConfiguration{};
		std::array<char, ProcessCrashIdentityCapacity> BuildIdentity{};
		std::array<char, ProcessCrashPathCapacity> ActiveLogPath{};
		uint64 LastAcceptedLogSequence = 0;
		uint64 LastProcessedLogSequence = 0;
		uint64 LastDurableLogSequence = 0;
	};

	CORE_API auto InitializeProcessCrashContext(
		std::string_view RuntimeVariant,
		std::string_view BuildConfiguration,
		std::string_view BuildIdentity) -> void;
	CORE_API auto PublishProcessCrashLogPath(std::string_view Path) -> void;
	CORE_API auto PublishProcessCrashLogAccepted(uint64 Sequence) -> void;
	CORE_API auto PublishProcessCrashLogProcessed(uint64 Sequence) -> void;
	CORE_API auto PublishProcessCrashLogDurable(uint64 Sequence) -> void;
	CORE_API auto ReadProcessCrashContext() -> FProcessCrashContextSnapshot;
}
