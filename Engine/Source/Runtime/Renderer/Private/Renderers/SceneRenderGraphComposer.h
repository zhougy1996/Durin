#pragma once

#include "Renderers/SceneRenderGraphTypes.h"

namespace Durin
{
	class FSceneRenderer;
	struct FSceneFrameContext;

	// Wires feature contributions into the caller-owned parent graph.
	class FSceneRenderGraphComposer final
	{
	public:
		static auto Compose(
			FRDGBuilder& Graph,
			FSceneRenderer& Renderer,
			FSceneFrameContext& Context) -> void;
	};
} // namespace Durin
