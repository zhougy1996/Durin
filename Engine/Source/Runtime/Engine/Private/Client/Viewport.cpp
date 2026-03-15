#include "Client/Viewport.h"

namespace Doge
{
	FViewport::FViewport(FViewportClient* InViewportClient)
		: ViewportClient(InViewportClient)
	{
	}

	auto FViewport::SetInitialSize(FIntPoint InitialSizeXY) -> void
	{
		UpdateRHIViewport();
	}

	auto FViewport::InitRHIViewport() -> void
	{
	}

	auto FViewport::UpdateRHIViewport() -> void
	{
	}

	auto FViewport::GetRHIViewport() const -> const TRefCountPtr<FRHIViewport>&
	{
		return ViewportRHI;
	}
}