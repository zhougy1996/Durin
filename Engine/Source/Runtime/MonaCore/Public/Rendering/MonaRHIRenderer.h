#pragma once

#include "RHIFwd.h"
#include "Rendering/MonaRenderer.h"

namespace Doge::Mona
{
	class MWindow;

	struct FMonaViewportInfo
	{
		std::shared_ptr<FRHIViewport> ViewportRHI;
		bool bFullScreen;
	};


	class MONACORE_API FMonaRHIRenderer : public FMonaRenderer
	{
	public:
		auto CreateViewport(const TSharedPtr<MWindow>& Window) -> void override;
		auto DrawWindows() -> void override;

		auto GetRHIViewport(const MWindow& Window) -> TSharedPtr<FRHIViewport>;

		std::unordered_map<const MWindow*, FMonaViewportInfo*> WindowToViewportInfoMap;
	};
}