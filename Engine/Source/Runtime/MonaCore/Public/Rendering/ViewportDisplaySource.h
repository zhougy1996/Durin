#pragma once

#include "Math/MathFwd.h"
#include "RHIResources.h"

namespace Durin
{
	// Publishes a size-matched RHI texture for presentation by a UI consumer.
	class IViewportDisplaySource
	{
	public:
		virtual ~IViewportDisplaySource() = default;

		// Makes the resource for the requested physical pixel size observable before this call returns.
		virtual auto PrepareDisplay(const FVector2f& DesiredSize) -> void = 0;

		virtual auto GetDisplayTexture() const -> const FTextureRHIRef& = 0;
	};
}
