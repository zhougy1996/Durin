#pragma once

#include "EngineAPI.h"
#include "RHIResources.h"

namespace Durin
{
	class FViewportClient;

	// Couples a viewport client to the RHI viewport used for presentation.
	class FViewport
	{
	public:
		ENGINE_API FViewport(FViewportClient* InViewportClient);
		virtual ~FViewport() = default;

		ENGINE_API virtual auto SetInitialSize(FIntPoint InitialSizeXY) -> void;

		ENGINE_API virtual auto InitRHIViewport() -> void;

		ENGINE_API virtual auto UpdateRHIViewport() -> void;

		ENGINE_API auto GetRHIViewport() const -> const TRefCountPtr<FRHIViewport>&;
		auto GetViewportClient() const -> FViewportClient* { return ViewportClient; }

	protected:
		FViewportClient* ViewportClient;

		TRefCountPtr<FRHIViewport> ViewportRHI;
	};
}
