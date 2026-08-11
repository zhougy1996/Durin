#pragma once

#include "CoreAPI.h"

namespace Durin
{
	inline constexpr uint32 ProcessCrashBreadcrumbCapacity = 64;
	inline constexpr uint32 ProcessCrashPathCapacity = 1024;
	inline constexpr uint32 ProcessCrashIdentityCapacity = 64;

	// Identifies the coarse process lifecycle boundary visible to a crash writer.
	enum class EProcessCrashPhase : uint32
	{
		ProcessEntry = 0,
		PreInitialization = 1,
		EngineInitialization = 2,
		Running = 3,
		ConsumerDetachment = 4,
		AssetServiceShutdown = 5,
		TaskSystemShutdown = 6,
		AssetManagerShutdown = 7,
		ObjectCollection = 8,
		ModuleShutdown = 9,
		RenderingShutdown = 10,
		RHIShutdown = 11,
		ApplicationShutdown = 12,
		Exited = 13,
	};

	// Names the bounded lifecycle events retained independently of ordinary logs.
	enum class EProcessCrashBreadcrumbEvent : uint32
	{
		PhaseChanged = 0,
		ClassDefaultsReleased = 1,
		StructDefaultsReleased = 2,
		EngineRootRetired = 3,
		FirstObjectCollection = 4,
		RenderingCommandsFlushed = 5,
		SecondObjectCollection = 6,
		DeferredDestroyAudit = 7,
		ModulesUnloaded = 8,
	};

	// Stores one generation-qualified, allocation-free lifecycle observation.
	struct FProcessCrashBreadcrumb
	{
		uint64 Sequence = 0;
		uint64 MonotonicMicroseconds = 0;
		uint64 Argument0 = 0;
		uint64 Argument1 = 0;
		uint32 ThreadId = 0;
		EProcessCrashBreadcrumbEvent Event = EProcessCrashBreadcrumbEvent::PhaseChanged;
	};

	// Copies the fixed crash-readable state without locks, waits, or allocation.
	struct FProcessCrashContextSnapshot
	{
		EProcessCrashPhase Phase = EProcessCrashPhase::ProcessEntry;
		uint64 ProcessStartUtcMilliseconds = 0;
		uint64 ProcessStartMonotonicMicroseconds = 0;
		uint64 BreadcrumbWriteSequence = 0;
		uint64 FirstBreadcrumbSequence = 0;
		uint32 BreadcrumbCount = 0;
		std::array<FProcessCrashBreadcrumb, ProcessCrashBreadcrumbCapacity> Breadcrumbs{};
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
	CORE_API auto SetProcessCrashPhase(EProcessCrashPhase Phase) -> void;
	CORE_API auto GetProcessCrashPhase() -> EProcessCrashPhase;
	CORE_API auto AddProcessCrashBreadcrumb(
		EProcessCrashBreadcrumbEvent Event,
		uint64 Argument0 = 0,
		uint64 Argument1 = 0) -> uint64;
	CORE_API auto PublishProcessCrashLogPath(std::string_view Path) -> void;
	CORE_API auto PublishProcessCrashLogAccepted(uint64 Sequence) -> void;
	CORE_API auto PublishProcessCrashLogProcessed(uint64 Sequence) -> void;
	CORE_API auto PublishProcessCrashLogDurable(uint64 Sequence) -> void;
	CORE_API auto ReadProcessCrashContext() -> FProcessCrashContextSnapshot;
	CORE_API auto ProcessCrashPhaseName(EProcessCrashPhase Phase) -> const char*;
	CORE_API auto ProcessCrashBreadcrumbName(EProcessCrashBreadcrumbEvent Event) -> const char*;
}
