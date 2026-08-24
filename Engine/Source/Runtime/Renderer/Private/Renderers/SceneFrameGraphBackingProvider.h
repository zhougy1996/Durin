#pragma once

#include "Renderers/SceneFrameGraphTypes.h"

namespace Durin
{
	// Translates retained graph requests into one typed allocation request and
	// publishes a complete logical-to-physical target set atomically.
	class RENDERER_API FSceneFrameGraphBackingProvider final
	{
	public:
		[[nodiscard]] static auto BuildRetainedTopology(
			std::span<const FRenderGraphPreparationRequest> Requests,
			const FSceneFrameTopology& Frame,
			std::string& Error) -> std::optional<FSceneFrameTopology>;

		[[nodiscard]] static auto Publish(
			std::span<const FRenderGraphPreparationRequest> Requests,
			FRenderGraphResourceBackings& Backings,
			const FSceneFrameGraphResources& Resources,
			const FResolvedSceneFrameTargets& Targets,
			std::string& Error) -> bool;
	};
} // namespace Durin
