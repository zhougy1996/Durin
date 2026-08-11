#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	enum class EEngineFrameMode : uint8
	{
		Startup,
		Running,
	};

	constexpr auto ShouldRedrawEngineViewports(EEngineFrameMode Mode) -> bool
	{
		return Mode == EEngineFrameMode::Running;
	}

	// Submits and synchronizes one non-minimized engine render frame.
	auto RenderEngineFrame() -> void;
	// Submits Mona/ImGui only while concrete engine initialization is in progress.
	auto RenderEngineStartupFrame() -> void;
}
