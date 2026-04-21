#pragma once

#include "Engine/API.h"
#include "RHIResources.h"

namespace Doge
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

	protected:
		FViewportClient* ViewportClient;

		TRefCountPtr<FRHIViewport> ViewportRHI;
	};
}