#pragma once

#include "RHIResources.h"

namespace Durin::Mona
{
	// Distinguishes native-window presentation from offscreen texture rendering.
	enum class EMonaViewportRenderMode
	{
		Window,
		RenderTarget
	};

	// Defines the sizing and RHI resource contract for a Mona viewport.
	class IMonaViewport
	{
	public:
		virtual ~IMonaViewport() = default;

		virtual auto GetRenderMode() const -> EMonaViewportRenderMode = 0;

		virtual auto IsWindowBacked() const -> bool = 0;

		virtual auto GetDesiredSize() const -> FVector2f = 0;

		virtual auto UpdateRHIViewport() -> void = 0;

		virtual auto GetDisplayTexture() const -> const FTextureRHIRef& = 0;
	};
}
