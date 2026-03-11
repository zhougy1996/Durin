#pragma once

#include "RHIResources.h"
#include "Rendering/MonaRenderer.h"

namespace Doge::Mona
{
	class MWindow;

	struct FMonaViewportInfo
	{
		TRefCountPtr<FRHIViewport> ViewportRHI;
		bool bFullScreen;
	};


	class MONACORE_API FMonaRHIRenderer : public FMonaRenderer
	{
	public:
		auto CreateViewport(const TSharedPtr<MWindow>& Window) -> void override;
		auto DrawWindows() -> void override;

		auto GetRHIViewport(const MWindow& Window) -> TRefCountPtr<FRHIViewport>;

		std::unordered_map<const MWindow*, FMonaViewportInfo*> WindowToViewportInfoMap;
	};
}