#include "PCH.Core.h"

#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		FEngineExitCoordinator GEngineExitCoordinator;

		auto CheckEngineExitCoordinatorThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}
	}

	bool GIsRequestingExit = false;
	bool GIsWindowDisplaySuppressed = false;

	std::vector<const char*> GMonaRequiredVulkanInstanceExtensions;

	uint32 GGameThreadId = 0;
	bool GIsGameThreadIdInitialized = false;


	uint64 GFrameCounter = 0;
	uint64 GFrameCounterRenderThread = 0;
	uint64 GRenderFrameCounter = 0;
	uint64 GRenderFrameCounterRenderThread = 0;

	auto FEngineExitCoordinator::GetPhase() const -> EEngineExitPhase
	{
		return Phase.load(std::memory_order_acquire);
	}

	auto FEngineExitCoordinator::AddPreExitCallback(
		std::function<void()> Callback) -> FDelegateHandle
	{
		CheckEngineExitCoordinatorThread();
		if (!Callback || GetPhase() != EEngineExitPhase::Running)
		{
			DURIN_WARN_CATEGORY(
				"EngineExit",
				"Pre-exit callback registration rejected in phase '{}'.",
				GetEngineExitPhaseName(GetPhase()));
			return {};
		}
		return OnEnginePreExit.AddLambda(std::move(Callback));
	}

	auto FEngineExitCoordinator::RemovePreExitCallback(
		FDelegateHandle Handle) -> bool
	{
		CheckEngineExitCoordinatorThread();
		return OnEnginePreExit.Remove(Handle);
	}

	auto FEngineExitCoordinator::BeginExit() -> bool
	{
		CheckEngineExitCoordinatorThread();
		EEngineExitPhase ExpectedPhase = EEngineExitPhase::Running;
		if (!Phase.compare_exchange_strong(
			ExpectedPhase,
			EEngineExitPhase::QuiescingProducers,
			std::memory_order_acq_rel))
		{
			DURIN_ERROR_CATEGORY(
				"EngineExit",
				"Engine exit re-entry was rejected in phase '{}'.",
				GetEngineExitPhaseName(ExpectedPhase));
			return false;
		}
		DURIN_INFO_CATEGORY("EngineExit", "Engine exit phase: {}.",
			GetEngineExitPhaseName(EEngineExitPhase::QuiescingProducers));
		OnEnginePreExit.Broadcast();
		return true;
	}

	auto FEngineExitCoordinator::AdvanceTo(
		EEngineExitPhase NewPhase) -> bool
	{
		CheckEngineExitCoordinatorThread();
		const EEngineExitPhase CurrentPhase = GetPhase();
		if (static_cast<uint8>(NewPhase)
			!= static_cast<uint8>(CurrentPhase) + 1)
		{
			DURIN_ERROR_CATEGORY(
				"EngineExit",
				"Engine exit phase transition from '{}' to '{}' was "
				"rejected.",
				GetEngineExitPhaseName(CurrentPhase),
				GetEngineExitPhaseName(NewPhase));
			return false;
		}
		Phase.store(NewPhase, std::memory_order_release);
		DURIN_INFO_CATEGORY("EngineExit", "Engine exit phase: {}.",
			GetEngineExitPhaseName(NewPhase));
		return true;
	}

	auto GetEngineExitPhase() -> EEngineExitPhase
	{
		return GEngineExitCoordinator.GetPhase();
	}

	auto GetEngineExitPhaseName(EEngineExitPhase Phase) -> const char*
	{
		switch (Phase)
		{
		case EEngineExitPhase::Running: return "Running";
		case EEngineExitPhase::QuiescingProducers: return "QuiescingProducers";
		case EEngineExitPhase::DetachingRenderConsumers: return "DetachingRenderConsumers";
		case EEngineExitPhase::DrainingObjects: return "DrainingObjects";
		case EEngineExitPhase::UnloadingModules: return "UnloadingModules";
		case EEngineExitPhase::ClosingRenderAdmission: return "ClosingRenderAdmission";
		case EEngineExitPhase::RenderingStopped: return "RenderingStopped";
		case EEngineExitPhase::Complete: return "Complete";
		}
		return "<unknown>";
	}

	auto AddOnEnginePreExit(std::function<void()> Callback) -> FDelegateHandle
	{
		return GEngineExitCoordinator.AddPreExitCallback(std::move(Callback));
	}

	auto RemoveOnEnginePreExit(FDelegateHandle Handle) -> bool
	{
		return GEngineExitCoordinator.RemovePreExitCallback(Handle);
	}

	auto BeginEngineExit() -> bool
	{
		return GEngineExitCoordinator.BeginExit();
	}

	auto AdvanceEngineExitPhase(EEngineExitPhase NewPhase) -> bool
	{
		return GEngineExitCoordinator.AdvanceTo(NewPhase);
	}
}
