#pragma once

#include "CoreMinimal.h"
#include "CoreGlobals.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"

namespace Durin::RendererPrivate
{
	// Opt-in wall-clock stages include time blocked on the RHI executor.
	class FRenderStageTimings final
	{
	public:
		explicit FRenderStageTimings(bool InPrepareOnly, const char* InGroup = "RenderView", double InMinimumMS = 20.0)
			: bEnabled(Profiling::AreStartupDiagnosticsEnabled()), bPrepareOnly(InPrepareOnly), Group(InGroup), MinimumMS(InMinimumMS)
		{
			if (bEnabled) { Started = Last = std::chrono::steady_clock::now(); Stats = GCommandListExecutor.GetStats(); }
		}
		~FRenderStageTimings()
		{
			if (!bEnabled) return;
			Next(nullptr);
			const double Total = std::chrono::duration<double, std::milli>(Last - Started).count();
			if (Total < MinimumMS) return;
			DURIN_DEBUG("Render stages: group={} | frame={} | prepare={} | total={:.3f} ms", Group, GRenderFrameCounterRenderThread, bPrepareOnly, Total);
			for (size_t Index = 0; Index < Count; ++Index)
				DURIN_DEBUG("Render stage: group={} | {} | wall={:.3f} ms | rhi-wait={:.3f} ms | sync-ops={}",
					Group, Entries[Index].Name, Entries[Index].WallMS, Entries[Index].WaitMS, Entries[Index].SyncOps);
		}
		auto Next(const char* Name) -> void
		{
			if (!bEnabled) return;
			const auto Now = std::chrono::steady_clock::now();
			const auto Current = GCommandListExecutor.GetStats();
			check(Count < Entries.size());
			Entries[Count++] = {Stage, std::chrono::duration<double, std::milli>(Now - Last).count(),
				static_cast<double>(Current.WaitDurationNanoseconds - Stats.WaitDurationNanoseconds) / 1e6,
				Current.SynchronousOperationCount - Stats.SynchronousOperationCount};
			Stage = Name;
			Last = Now;
			Stats = Current;
		}
	private:
		struct FEntry { const char* Name; double WallMS; double WaitMS; uint64 SyncOps; };
		std::array<FEntry, 16> Entries{};
		size_t Count = 0;
		bool bEnabled;
		bool bPrepareOnly;
		const char* Group;
		double MinimumMS;
		const char* Stage = "Setup";
		std::chrono::steady_clock::time_point Started{}, Last{};
		FRHICommandListExecutorStats Stats{};
	};

}
