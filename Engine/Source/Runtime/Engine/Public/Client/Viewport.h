#pragma once

namespace Doge
{
	class FViewportClient;
	class FRHIViewport;

	class ENGINE_API FViewport
	{
	public:
		FViewport(FViewportClient* InViewportClient);
		virtual ~FViewport() = default;

		virtual auto SetInitialSize(FIntPoint InitialSizeXY) -> void;

		virtual auto InitRHIViewport() -> void;

		virtual auto UpdateRHIViewport() -> void;

		auto GetRHIViewport() const -> const TSharedPtr<FRHIViewport>&;

	protected:
		FViewportClient* ViewportClient;

		TSharedPtr<FRHIViewport> ViewportRHI;
	};
}