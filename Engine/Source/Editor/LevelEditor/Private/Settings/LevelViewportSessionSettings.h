#pragma once

#include "Viewport/ViewportCameraTransform.h"

namespace Durin
{
	class FYamlNodeRef;
	class FYamlNodeView;

	using FLevelViewportStateMap = std::unordered_map<std::string, std::unordered_map<std::string, FLevelViewportCameraState>>;

	auto LoadLevelViewportStates(const FYamlNodeView& Root, FLevelViewportStateMap& OutStates) -> void;
	auto SaveLevelViewportStates(FYamlNodeRef Root, const FLevelViewportStateMap& States) -> void;
}
