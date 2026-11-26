#pragma once

#include "CoreMinimal.h"
#include "RHICommandList.h"
#include "RendererAPI.h"
#include "Threading/Task.h"

namespace Durin { struct FSceneView; }

namespace Durin::RendererPrivate
{
	// UE-style per-pass recording group. Workers borrow the prepared pass until
	// Dispatch joins them; the parent owns the inserted RHI payload afterwards.
	class FParallelCommandListSet
	{
	public:
		RENDERER_API FParallelCommandListSet(const FSceneView& View, FRHICommandList& Parent);
		RENDERER_API ~FParallelCommandListSet();
		FParallelCommandListSet(const FParallelCommandListSet&) = delete;
		auto operator=(const FParallelCommandListSet&) -> FParallelCommandListSet& = delete;
		RENDERER_API auto NewParallelCommandList() -> FRHISubCommandList*;
		RENDERER_API auto AddParallelCommandList(FRHISubCommandList* Commands, Tasks::FTask Completion) -> void;
		RENDERER_API auto Dispatch() -> bool;
		auto GetParentCommandList() const -> FRHICommandList& { return Parent; }
		auto GetPrereqs() const -> const std::vector<Tasks::FTask>& { return Prerequisites; }
		float MinDepth = 0.0f, MaxDepth = 1.0f;
		uint32 Width = 8;
		uint32 MinDrawsPerCommandList = 64;
		std::vector<Tasks::FTask> Prerequisites;
	private:
		struct FRecording
		{
			std::unique_ptr<FRHISubCommandList> Commands;
			Tasks::FTask Completion;
		};
		FRHICommandList& Parent;
		std::array<float, 4> ViewRect;
		std::vector<FRecording> Recordings;
		bool bDispatched = false;
	};
	// Runs are contiguous in visible-command order. Store the range rather than
	// allocating a member vector for every singleton or compatible group.
	struct FMeshDrawCommandRun
	{
		uint32 FirstCommand = 0;
		uint32 NumCommands = 0;
		auto GetDrawIndices() const { return std::views::iota(FirstCommand, FirstCommand + NumCommands); }
	};
}
