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

		struct FBreadcrumbSlot
		{
			std::atomic<uint64> CommittedSequence{0};
			std::atomic<uint64> MonotonicMicroseconds{0};
			std::atomic<uint64> Argument0{0};
			std::atomic<uint64> Argument1{0};
			std::atomic<uint32> ThreadId{0};
			std::atomic<EProcessCrashBreadcrumbEvent> Event{EProcessCrashBreadcrumbEvent::PhaseChanged};
		};

		struct FProcessCrashState
		{
			std::atomic<EProcessCrashPhase> Phase{EProcessCrashPhase::ProcessEntry};
			std::atomic<uint64> ProcessStartUtcMilliseconds{0};
			std::atomic<uint64> ProcessStartMonotonicMicroseconds{0};
			std::atomic<uint64> BreadcrumbWriteSequence{0};
			std::array<FBreadcrumbSlot, ProcessCrashBreadcrumbCapacity> Breadcrumbs{};
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
		SetProcessCrashPhase(EProcessCrashPhase::ProcessEntry);
	}

	auto SetProcessCrashPhase(EProcessCrashPhase Phase) -> void
	{
		GProcessCrashState.Phase.store(Phase, std::memory_order_release);
		AddProcessCrashBreadcrumb(EProcessCrashBreadcrumbEvent::PhaseChanged, static_cast<uint64>(Phase));
	}

	auto GetProcessCrashPhase() -> EProcessCrashPhase
	{
		return GProcessCrashState.Phase.load(std::memory_order_acquire);
	}

	auto AddProcessCrashBreadcrumb(EProcessCrashBreadcrumbEvent Event, uint64 Argument0, uint64 Argument1) -> uint64
	{
		const uint64 Sequence = GProcessCrashState.BreadcrumbWriteSequence.fetch_add(1, std::memory_order_relaxed) + 1;
		FBreadcrumbSlot& Slot = GProcessCrashState.Breadcrumbs[(Sequence - 1) & (ProcessCrashBreadcrumbCapacity - 1)];
		Slot.CommittedSequence.store(0, std::memory_order_relaxed);
		Slot.MonotonicMicroseconds.store(MonotonicMicroseconds(), std::memory_order_relaxed);
		Slot.Argument0.store(Argument0, std::memory_order_relaxed);
		Slot.Argument1.store(Argument1, std::memory_order_relaxed);
		Slot.ThreadId.store(FPlatformLTS::GetCurrentThreadId(), std::memory_order_relaxed);
		Slot.Event.store(Event, std::memory_order_relaxed);
		Slot.CommittedSequence.store(Sequence, std::memory_order_release);
		return Sequence;
	}

	auto PublishProcessCrashLogPath(std::string_view Path) -> void { PublishText(GProcessCrashState.ActiveLogPath, Path); }
	auto PublishProcessCrashLogAccepted(uint64 Sequence) -> void { GProcessCrashState.LastAcceptedLogSequence.store(Sequence, std::memory_order_release); }
	auto PublishProcessCrashLogProcessed(uint64 Sequence) -> void { GProcessCrashState.LastProcessedLogSequence.store(Sequence, std::memory_order_release); }
	auto PublishProcessCrashLogDurable(uint64 Sequence) -> void { GProcessCrashState.LastDurableLogSequence.store(Sequence, std::memory_order_release); }

	auto ReadProcessCrashContext() -> FProcessCrashContextSnapshot
	{
		FProcessCrashContextSnapshot Result;
		Result.Phase = GProcessCrashState.Phase.load(std::memory_order_acquire);
		Result.ProcessStartUtcMilliseconds = GProcessCrashState.ProcessStartUtcMilliseconds.load(std::memory_order_relaxed);
		Result.ProcessStartMonotonicMicroseconds = GProcessCrashState.ProcessStartMonotonicMicroseconds.load(std::memory_order_relaxed);
		ReadText(GProcessCrashState.RuntimeVariant, Result.RuntimeVariant);
		ReadText(GProcessCrashState.BuildConfiguration, Result.BuildConfiguration);
		ReadText(GProcessCrashState.BuildIdentity, Result.BuildIdentity);
		ReadText(GProcessCrashState.ActiveLogPath, Result.ActiveLogPath);
		Result.LastAcceptedLogSequence = GProcessCrashState.LastAcceptedLogSequence.load(std::memory_order_acquire);
		Result.LastProcessedLogSequence = GProcessCrashState.LastProcessedLogSequence.load(std::memory_order_acquire);
		Result.LastDurableLogSequence = GProcessCrashState.LastDurableLogSequence.load(std::memory_order_acquire);
		Result.BreadcrumbWriteSequence = GProcessCrashState.BreadcrumbWriteSequence.load(std::memory_order_acquire);
		Result.FirstBreadcrumbSequence = Result.BreadcrumbWriteSequence > ProcessCrashBreadcrumbCapacity
			? Result.BreadcrumbWriteSequence - ProcessCrashBreadcrumbCapacity + 1 : 1;
		for (uint64 Sequence = Result.FirstBreadcrumbSequence; Sequence <= Result.BreadcrumbWriteSequence; ++Sequence)
		{
			const FBreadcrumbSlot& Slot = GProcessCrashState.Breadcrumbs[(Sequence - 1) & (ProcessCrashBreadcrumbCapacity - 1)];
			if (Slot.CommittedSequence.load(std::memory_order_acquire) != Sequence) continue;
			const FProcessCrashBreadcrumb Value{
				.Sequence = Sequence,
				.MonotonicMicroseconds = Slot.MonotonicMicroseconds.load(std::memory_order_relaxed),
				.Argument0 = Slot.Argument0.load(std::memory_order_relaxed),
				.Argument1 = Slot.Argument1.load(std::memory_order_relaxed),
				.ThreadId = Slot.ThreadId.load(std::memory_order_relaxed),
				.Event = Slot.Event.load(std::memory_order_relaxed)};
			if (Slot.CommittedSequence.load(std::memory_order_acquire) != Sequence) continue;
			Result.Breadcrumbs[Result.BreadcrumbCount++] = Value;
		}
		return Result;
	}

	auto ProcessCrashPhaseName(EProcessCrashPhase Phase) -> const char*
	{
		switch (Phase)
		{
		case EProcessCrashPhase::ProcessEntry: return "ProcessEntry";
		case EProcessCrashPhase::PreInitialization: return "PreInitialization";
		case EProcessCrashPhase::EngineInitialization: return "EngineInitialization";
		case EProcessCrashPhase::Running: return "Running";
		case EProcessCrashPhase::ConsumerDetachment: return "ConsumerDetachment";
		case EProcessCrashPhase::AssetServiceShutdown: return "AssetServiceShutdown";
		case EProcessCrashPhase::TaskSystemShutdown: return "TaskSystemShutdown";
		case EProcessCrashPhase::AssetManagerShutdown: return "AssetManagerShutdown";
		case EProcessCrashPhase::ObjectCollection: return "ObjectCollection";
		case EProcessCrashPhase::ModuleShutdown: return "ModuleShutdown";
		case EProcessCrashPhase::RenderingShutdown: return "RenderingShutdown";
		case EProcessCrashPhase::RHIShutdown: return "RHIShutdown";
		case EProcessCrashPhase::ApplicationShutdown: return "ApplicationShutdown";
		case EProcessCrashPhase::Exited: return "Exited";
		default: return "Unknown";
		}
	}

	auto ProcessCrashBreadcrumbName(EProcessCrashBreadcrumbEvent Event) -> const char*
	{
		switch (Event)
		{
		case EProcessCrashBreadcrumbEvent::PhaseChanged: return "PhaseChanged";
		case EProcessCrashBreadcrumbEvent::ClassDefaultsReleased: return "ClassDefaultsReleased";
		case EProcessCrashBreadcrumbEvent::StructDefaultsReleased: return "StructDefaultsReleased";
		case EProcessCrashBreadcrumbEvent::EngineRootRetired: return "EngineRootRetired";
		case EProcessCrashBreadcrumbEvent::FirstObjectCollection: return "FirstObjectCollection";
		case EProcessCrashBreadcrumbEvent::RenderingCommandsFlushed: return "RenderingCommandsFlushed";
		case EProcessCrashBreadcrumbEvent::SecondObjectCollection: return "SecondObjectCollection";
		case EProcessCrashBreadcrumbEvent::DeferredDestroyAudit: return "DeferredDestroyAudit";
		case EProcessCrashBreadcrumbEvent::ModulesUnloaded: return "ModulesUnloaded";
		default: return "Unknown";
		}
	}
}
