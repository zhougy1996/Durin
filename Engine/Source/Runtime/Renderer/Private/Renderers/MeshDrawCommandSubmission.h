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
}
