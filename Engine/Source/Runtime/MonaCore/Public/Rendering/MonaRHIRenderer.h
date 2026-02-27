#pragma once

#include "RHIFwd.h"
#include "Rendering/MonaRenderer.h"

namespace Doge::Mona
{
	class MWindow;

	class MONACORE_API FMonaRHIRenderer : public FMonaRenderer
	{
	public:
		virtual auto GetRHIViewport(MWindow& Window) -> TSharedPtr<FRHIViewport>;

		virtual auto CreateViewport(const TSharedPtr<MWindow>& Window) -> void;
	};
}