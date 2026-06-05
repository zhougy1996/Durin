#pragma once

#include "RHIFwd.h"

namespace Durin::Mona
{
	enum class EMonaViewportRenderMode
	{
		Window,
		RenderTarget
	};

	class IMonaViewport
	{
	public:
		virtual ~IMonaViewport() = default;

		virtual auto GetRenderMode() const -> EMonaViewportRenderMode = 0;

		virtual auto IsWindowBacked() const -> bool = 0;

		virtual auto GetDesiredSize() const -> FVector2f = 0;

		virtual auto UpdateRHIViewport() -> void = 0;

		virtual auto GetRenderTargetTexture() const -> FRHITexture* = 0;

		virtual auto IsRenderTargetReady() const -> bool = 0;
	};
}
