#include "Client/Viewport.h"

namespace Doge
{
	FViewport::FViewport(FViewportClient* ViewportClient)
		: ViewportClient_(ViewportClient)
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

	auto FViewport::GetRHIViewport() const -> const TSharedPtr<FRHIViewport>&
	{
		return RHIViewport_;
	}
}