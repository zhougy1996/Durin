#pragma once

#include "EngineAPI.h"
#include "RHIResources.h"

namespace Durin
{
	class FViewportClient;

	class ENGINE_API FViewport
	{
	public:
		FViewport(FViewportClient* InViewportClient);
		virtual ~FViewport() = default;

		virtual auto SetInitialSize(FIntPoint InitialSizeXY) -> void;

		virtual auto InitRHIViewport() -> void;

		virtual auto UpdateRHIViewport() -> void;

		auto GetRHIViewport() const -> const TRefCountPtr<FRHIViewport>&;
		auto GetViewportClient() const -> FViewportClient* { return ViewportClient; }

	protected:
		FViewportClient* ViewportClient;

		TRefCountPtr<FRHIViewport> ViewportRHI;
	};
}
