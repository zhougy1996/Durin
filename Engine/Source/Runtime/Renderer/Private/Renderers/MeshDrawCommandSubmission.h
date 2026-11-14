#pragma once

#include "CoreMinimal.h"

namespace Durin::RendererPrivate
{
	// Runs are contiguous in visible-command order. Store the range rather than
	// allocating a member vector for every singleton or compatible group.
	struct FMeshDrawCommandRun
	{
		uint32 FirstCommand = 0;
		uint32 NumCommands = 0;
		auto GetDrawIndices() const { return std::views::iota(FirstCommand, FirstCommand + NumCommands); }
	};
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
			Runs.reserve(Members.size());
			for (auto& Draws : Members)
			{
				for (uint32 Draw : Draws.GetDrawIndices()) { require(Draw < DrawCount); RunByDraw[Draw] = static_cast<uint32>(Runs.size()); }
				Runs.push_back({std::move(Draws)});
			}
		}
		auto IsGPUSelected(uint32 Run) const -> bool { return bDispatched && Runs[Run].ArgumentIndex != InvalidArgument; }
		auto GetArgumentIndex(uint32 Draw) const -> uint32
		{
			if (Draw >= RunByDraw.size() || RunByDraw[Draw] == InvalidArgument || !IsGPUSelected(RunByDraw[Draw])) return InvalidArgument;
			const auto& Run = Runs[RunByDraw[Draw]];
			return Draw == Run.Draws.FirstCommand ? Run.ArgumentIndex : GroupedMember;
		}
	};
}
