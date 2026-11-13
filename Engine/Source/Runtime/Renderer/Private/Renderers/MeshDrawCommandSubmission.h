#pragma once

#include "CoreMinimal.h"

namespace Durin::RendererPrivate
{
	using FMeshDrawCommandRun = std::vector<uint32>;
	// One membership/selection authority shared by CPU fallback and graph recording.
	struct FMeshDrawCommandSubmission
	{
		static constexpr uint32 InvalidArgument = UINT32_MAX;
		static constexpr uint32 GroupedMember = UINT32_MAX - 1;
		struct FRun { FMeshDrawCommandRun Draws; uint32 ArgumentIndex = InvalidArgument; };
		std::vector<FRun> Runs;
		std::vector<uint32> RunByDraw;
		bool bDispatched = false;
		FMeshDrawCommandSubmission(std::vector<FMeshDrawCommandRun> Members, size_t DrawCount) : RunByDraw(DrawCount, InvalidArgument)
		{
			for (auto& Draws : Members)
			{
				for (uint32 Draw : Draws) { require(Draw < DrawCount); RunByDraw[Draw] = static_cast<uint32>(Runs.size()); }
				Runs.push_back({std::move(Draws)});
			}
		}
		auto IsGPUSelected(uint32 Run) const -> bool { return bDispatched && Runs[Run].ArgumentIndex != InvalidArgument; }
		auto GetArgumentIndex(uint32 Draw) const -> uint32
		{
			if (Draw >= RunByDraw.size() || RunByDraw[Draw] == InvalidArgument || !IsGPUSelected(RunByDraw[Draw])) return InvalidArgument;
			const auto& Run = Runs[RunByDraw[Draw]];
			return Draw == Run.Draws.front() ? Run.ArgumentIndex : GroupedMember;
		}
	};
}
